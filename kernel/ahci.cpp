#include "kernel/ahci.hpp"

#include <stddef.h>
#include <stdint.h>

#include "arch/cpu.hpp"
#include "kernel/console.hpp"
#include "kernel/pci.hpp"
#include "kernel/physical_memory.hpp"
#include "kernel/string.hpp"
#include "kernel/vmm.hpp"

namespace {

// PCI class of an AHCI HBA: mass storage, SATA, AHCI 1.0 programming interface.
constexpr uint8_t kClassMassStorage = 0x01;
constexpr uint8_t kSubclassSata = 0x06;
constexpr uint8_t kProgIfAhci = 0x01;
constexpr uint8_t kAbarIndex = 5;

constexpr uint8_t kPciCommandOffset = 0x04;
constexpr uint16_t kPciCommandMemory = 1u << 1;
constexpr uint16_t kPciCommandBusMaster = 1u << 2;
constexpr uint16_t kPciCommandInterruptDisable = 1u << 10;

// HBA registers (offsets from ABAR).
constexpr uint32_t kHbaCap = 0x00;
constexpr uint32_t kHbaGhc = 0x04;
constexpr uint32_t kHbaPortsImplemented = 0x0c;
constexpr uint32_t kHbaCap2 = 0x24;
constexpr uint32_t kHbaBohc = 0x28;
constexpr uint32_t kCapS64a = 1u << 31;
constexpr uint32_t kGhcAhciEnable = 1u << 31;
constexpr uint32_t kGhcInterruptEnable = 1u << 1;
constexpr uint32_t kCap2BiosHandoff = 1u << 0;
constexpr uint32_t kBohcBiosOwned = 1u << 0;
constexpr uint32_t kBohcOsOwned = 1u << 1;

// Port registers (offsets from the port's 0x80-byte block).
constexpr uint32_t kPortBase = 0x100;
constexpr uint32_t kPortStride = 0x80;
constexpr uint32_t kPxClb = 0x00;
constexpr uint32_t kPxClbu = 0x04;
constexpr uint32_t kPxFb = 0x08;
constexpr uint32_t kPxFbu = 0x0c;
constexpr uint32_t kPxIs = 0x10;
constexpr uint32_t kPxIe = 0x14;
constexpr uint32_t kPxCmd = 0x18;
constexpr uint32_t kPxTfd = 0x20;
constexpr uint32_t kPxSig = 0x24;
constexpr uint32_t kPxSsts = 0x28;
constexpr uint32_t kPxSerr = 0x30;
constexpr uint32_t kPxCi = 0x38;

constexpr uint32_t kCmdStart = 1u << 0;
constexpr uint32_t kCmdSpinUp = 1u << 1;
constexpr uint32_t kCmdPowerOn = 1u << 2;
constexpr uint32_t kCmdFisReceiveEnable = 1u << 4;
constexpr uint32_t kCmdFisReceiveRunning = 1u << 14;
constexpr uint32_t kCmdListRunning = 1u << 15;

constexpr uint32_t kIsTaskFileError = 1u << 30;
constexpr uint32_t kIsFatalErrors = (1u << 30) | (1u << 29) | (1u << 28) | (1u << 27);

constexpr uint8_t kTfdErr = 1u << 0;
constexpr uint8_t kTfdDrq = 1u << 3;
constexpr uint8_t kTfdBsy = 1u << 7;

constexpr uint32_t kSstsDetPresent = 3;
constexpr uint32_t kSigSataDisk = 0x00000101u;

// ATA commands. All data commands use 48-bit LBA.
constexpr uint8_t kAtaIdentify = 0xec;
constexpr uint8_t kAtaReadDmaExt = 0x25;
constexpr uint8_t kAtaWriteDmaExt = 0x35;
constexpr uint8_t kAtaFlushCacheExt = 0xea;
constexpr uint8_t kFisTypeRegH2d = 0x27;

// Per-port memory, one page: command list (32 headers x 32 bytes, 1 KiB
// aligned), received-FIS area (256 bytes, 256 aligned), and the one command
// table this driver uses (128-byte header + PRDT, 128 aligned).
constexpr size_t kCommandListOffset = 0;
constexpr size_t kReceivedFisOffset = 1024;
constexpr size_t kCommandTableOffset = 1280;
constexpr size_t kPrdtOffsetInTable = 0x80;

// DMA bounce buffer: one physically contiguous run, so one PRDT entry covers
// a whole request. 256 sectors is also what a 16-bit count of 0x100 carries.
constexpr uint32_t kMaxSectorsPerCommand = 256;
constexpr size_t kBounceBytes = static_cast<size_t>(kMaxSectorsPerCommand) * block::kSectorSize;
constexpr uint64_t kBouncePages = kBounceBytes / memory::kPageSize;

// Polling budgets, in register reads. Generous: a spin is an MMIO read, which
// under virtualization is a VM exit of a microsecond or more.
constexpr uint32_t kCommandSpins = 200000000u;
constexpr uint32_t kEngineSpins = 1000000u;

constexpr size_t kMaxDisks = 4;

struct [[gnu::packed]] CommandHeader {
    uint16_t flags;          // CFL in bits 0-4, W (write) bit 6
    uint16_t prdt_length;
    uint32_t prd_byte_count;
    uint32_t table_base;
    uint32_t table_base_upper;
    uint32_t reserved[4];
};
static_assert(sizeof(CommandHeader) == 32);

struct [[gnu::packed]] PrdtEntry {
    uint32_t data_base;
    uint32_t data_base_upper;
    uint32_t reserved;
    uint32_t byte_count_flags; // byte count - 1 in bits 0-21
};
static_assert(sizeof(PrdtEntry) == 16);

struct Disk {
    volatile uint8_t* port;
    memory::PageAllocation port_memory;
    uint32_t sector_count;
    uint8_t port_index;
    char name[8];
};

volatile uint8_t* g_abar = nullptr;
bool g_s64a = false;
memory::PageAllocation g_bounce = {};
Disk g_disks[kMaxDisks] = {};
size_t g_disk_count = 0;

uint32_t read_reg(volatile uint8_t* base, uint32_t offset) {
    return *reinterpret_cast<volatile uint32_t*>(base + offset);
}

void write_reg(volatile uint8_t* base, uint32_t offset, uint32_t value) {
    *reinterpret_cast<volatile uint32_t*>(base + offset) = value;
}

void barrier() {
    arch::memory_barrier();
}

bool wait_clear(volatile uint8_t* base, uint32_t offset, uint32_t mask, uint32_t spins) {
    while ((read_reg(base, offset) & mask) != 0) {
        if (--spins == 0) {
            return false;
        }
    }
    return true;
}

bool address_fits(uint64_t physical) {
    return g_s64a || (physical >> 32) == 0;
}

// Stops the command list and FIS receive engines (AHCI 1.3, 10.1.2).
bool stop_port(volatile uint8_t* port) {
    write_reg(port, kPxCmd, read_reg(port, kPxCmd) & ~kCmdStart);
    if (!wait_clear(port, kPxCmd, kCmdListRunning, kEngineSpins)) {
        return false;
    }
    write_reg(port, kPxCmd, read_reg(port, kPxCmd) & ~kCmdFisReceiveEnable);
    return wait_clear(port, kPxCmd, kCmdFisReceiveRunning, kEngineSpins);
}

bool start_port(volatile uint8_t* port) {
    if (!wait_clear(port, kPxTfd, kTfdBsy | kTfdDrq, kEngineSpins)) {
        return false;
    }
    write_reg(port, kPxCmd, read_reg(port, kPxCmd) | kCmdFisReceiveEnable);
    write_reg(port, kPxCmd, read_reg(port, kPxCmd) | kCmdStart);
    return true;
}

// After a task-file error the port stays stopped until software restarts it:
// clear the error state and start the engines again so the next request runs.
void recover_port(volatile uint8_t* port) {
    (void)stop_port(port);
    write_reg(port, kPxSerr, 0xffffffffu);
    write_reg(port, kPxIs, 0xffffffffu);
    (void)start_port(port);
}

// Issues one command in slot 0 and polls until the HBA clears it. `bytes` is
// the transfer through the bounce buffer, 0 for a non-data command.
bool issue(Disk& disk, uint8_t command, uint64_t lba, uint32_t sector_count, size_t bytes, bool write) {
    uint8_t* memory_base = static_cast<uint8_t*>(disk.port_memory.virtual_address);
    const uint64_t memory_physical = disk.port_memory.physical_address;
    auto* header = reinterpret_cast<CommandHeader*>(memory_base + kCommandListOffset);
    uint8_t* table = memory_base + kCommandTableOffset;
    auto* prdt = reinterpret_cast<PrdtEntry*>(table + kPrdtOffsetInTable);

    memset(table, 0, kPrdtOffsetInTable + sizeof(PrdtEntry));
    uint8_t* fis = table;
    fis[0] = kFisTypeRegH2d;
    fis[1] = 0x80; // C: this FIS carries a command
    fis[2] = command;
    fis[4] = static_cast<uint8_t>(lba);
    fis[5] = static_cast<uint8_t>(lba >> 8);
    fis[6] = static_cast<uint8_t>(lba >> 16);
    fis[7] = command == kAtaIdentify ? 0 : 0x40; // LBA mode
    fis[8] = static_cast<uint8_t>(lba >> 24);
    fis[9] = static_cast<uint8_t>(lba >> 32);
    fis[10] = static_cast<uint8_t>(lba >> 40);
    fis[12] = static_cast<uint8_t>(sector_count);
    fis[13] = static_cast<uint8_t>(sector_count >> 8);

    if (bytes != 0) {
        prdt->data_base = static_cast<uint32_t>(g_bounce.physical_address);
        prdt->data_base_upper = static_cast<uint32_t>(g_bounce.physical_address >> 32);
        prdt->byte_count_flags = static_cast<uint32_t>(bytes - 1u);
    }

    const uint64_t table_physical = memory_physical + kCommandTableOffset;
    memset(header, 0, sizeof(*header));
    header->flags = static_cast<uint16_t>(5u | (write ? (1u << 6) : 0u)); // CFL = 5 dwords
    header->prdt_length = bytes != 0 ? 1 : 0;
    header->table_base = static_cast<uint32_t>(table_physical);
    header->table_base_upper = static_cast<uint32_t>(table_physical >> 32);

    write_reg(disk.port, kPxIs, 0xffffffffu);
    if (!wait_clear(disk.port, kPxTfd, kTfdBsy | kTfdDrq, kEngineSpins)) {
        recover_port(disk.port);
        return false;
    }
    barrier();
    write_reg(disk.port, kPxCi, 1u);

    uint32_t spins = kCommandSpins;
    while ((read_reg(disk.port, kPxCi) & 1u) != 0) {
        if ((read_reg(disk.port, kPxIs) & kIsFatalErrors) != 0 || --spins == 0) {
            console::printf("ahci: %s command %x failed is=%x tfd=%x\n",
                disk.name, static_cast<unsigned>(command),
                static_cast<unsigned>(read_reg(disk.port, kPxIs)),
                static_cast<unsigned>(read_reg(disk.port, kPxTfd)));
            recover_port(disk.port);
            return false;
        }
    }
    barrier();
    if ((read_reg(disk.port, kPxIs) & kIsTaskFileError) != 0 ||
        (read_reg(disk.port, kPxTfd) & kTfdErr) != 0) {
        recover_port(disk.port);
        return false;
    }
    return true;
}

bool rw_sectors(void* context, uint32_t lba, uint32_t sector_count, void* buffer, bool write) {
    Disk& disk = *static_cast<Disk*>(context);
    auto* bytes = static_cast<uint8_t*>(buffer);
    uint8_t* bounce = static_cast<uint8_t*>(g_bounce.virtual_address);
    while (sector_count != 0) {
        const uint32_t chunk = sector_count < kMaxSectorsPerCommand ? sector_count : kMaxSectorsPerCommand;
        const size_t chunk_bytes = static_cast<size_t>(chunk) * block::kSectorSize;
        if (write) {
            memcpy(bounce, bytes, chunk_bytes);
        }
        if (!issue(disk, write ? kAtaWriteDmaExt : kAtaReadDmaExt, lba, chunk, chunk_bytes, write)) {
            return false;
        }
        if (!write) {
            memcpy(bytes, bounce, chunk_bytes);
        }
        lba += chunk;
        bytes += chunk_bytes;
        sector_count -= chunk;
    }
    return true;
}

bool read_op(void* context, uint32_t lba, uint32_t sector_count, void* buffer) {
    return rw_sectors(context, lba, sector_count, buffer, false);
}

bool write_op(void* context, uint32_t lba, uint32_t sector_count, const void* buffer) {
    return rw_sectors(context, lba, sector_count, const_cast<void*>(buffer), true);
}

bool flush_op(void* context) {
    Disk& disk = *static_cast<Disk*>(context);
    return issue(disk, kAtaFlushCacheExt, 0, 0, 0, false);
}

const block::DeviceOps kOps = {
    &read_op,
    &write_op,
    &flush_op,
    /*deferred_writes=*/true,
};

// Takes the HBA from the firmware if it advertises BIOS/OS handoff.
void claim_from_firmware() {
    if ((read_reg(g_abar, kHbaCap2) & kCap2BiosHandoff) == 0) {
        return;
    }
    write_reg(g_abar, kHbaBohc, read_reg(g_abar, kHbaBohc) | kBohcOsOwned);
    (void)wait_clear(g_abar, kHbaBohc, kBohcBiosOwned, kEngineSpins);
}

// Points the port at our command list and FIS area, and starts it. Returns
// false for anything that is not a SATA disk ready to take commands.
bool bring_up_port(uint8_t port_index, Disk& disk) {
    volatile uint8_t* port = g_abar + kPortBase + static_cast<uint32_t>(port_index) * kPortStride;
    if ((read_reg(port, kPxSsts) & 0x0fu) != kSstsDetPresent || read_reg(port, kPxSig) != kSigSataDisk) {
        return false;
    }
    if (!stop_port(port)) {
        console::printf("ahci: port %u does not stop\n", static_cast<unsigned>(port_index));
        return false;
    }

    memory::PageAllocation port_memory = {};
    if (!memory::allocate_page(port_memory) || !address_fits(port_memory.physical_address)) {
        return false;
    }
    memset(port_memory.virtual_address, 0, memory::kPageSize);
    const uint64_t list_physical = port_memory.physical_address + kCommandListOffset;
    const uint64_t fis_physical = port_memory.physical_address + kReceivedFisOffset;
    write_reg(port, kPxClb, static_cast<uint32_t>(list_physical));
    write_reg(port, kPxClbu, static_cast<uint32_t>(list_physical >> 32));
    write_reg(port, kPxFb, static_cast<uint32_t>(fis_physical));
    write_reg(port, kPxFbu, static_cast<uint32_t>(fis_physical >> 32));
    write_reg(port, kPxIe, 0);
    write_reg(port, kPxSerr, 0xffffffffu);
    write_reg(port, kPxIs, 0xffffffffu);
    write_reg(port, kPxCmd, read_reg(port, kPxCmd) | kCmdSpinUp | kCmdPowerOn);
    if (!start_port(port)) {
        console::printf("ahci: port %u does not start\n", static_cast<unsigned>(port_index));
        (void)memory::free_allocation(port_memory);
        return false;
    }

    disk = {};
    disk.port = port;
    disk.port_memory = port_memory;
    disk.port_index = port_index;
    disk.name[0] = 'a';
    disk.name[1] = 'h';
    disk.name[2] = 'c';
    disk.name[3] = 'i';
    disk.name[4] = static_cast<char>('0' + g_disk_count);
    disk.name[5] = 0;

    // IDENTIFY DEVICE: words 100-103 are the 48-bit sector count, 60-61 the
    // 28-bit one for drives without LBA48.
    if (!issue(disk, kAtaIdentify, 0, 0, block::kSectorSize, false)) {
        console::printf("ahci: port %u IDENTIFY failed\n", static_cast<unsigned>(port_index));
        (void)stop_port(port);
        (void)memory::free_allocation(port_memory);
        return false;
    }
    const auto* identify = static_cast<const uint16_t*>(g_bounce.virtual_address);
    uint64_t sectors = static_cast<uint64_t>(identify[100]) |
        (static_cast<uint64_t>(identify[101]) << 16) |
        (static_cast<uint64_t>(identify[102]) << 32) |
        (static_cast<uint64_t>(identify[103]) << 48);
    if (sectors == 0) {
        sectors = static_cast<uint64_t>(identify[60]) | (static_cast<uint64_t>(identify[61]) << 16);
    }
    if (sectors == 0) {
        (void)stop_port(port);
        (void)memory::free_allocation(port_memory);
        return false;
    }
    // block:: addresses 32-bit sector counts; a bigger disk is exposed up to
    // its first 2 TiB.
    disk.sector_count = sectors > 0xffffffffu ? 0xffffffffu : static_cast<uint32_t>(sectors);
    return true;
}

bool find_hba(pci::DeviceInfo& hba) {
    const size_t count = pci::device_count();
    for (size_t index = 0; index < count; ++index) {
        pci::DeviceInfo info = {};
        if (pci::device_info(index, info) && info.class_code == kClassMassStorage &&
            info.subclass == kSubclassSata && info.prog_if == kProgIfAhci) {
            hba = info;
            return true;
        }
    }
    return false;
}

void enumerate() {
    g_disk_count = 0;
    pci::DeviceInfo hba = {};
    if (!pci::ready() || !find_hba(hba)) {
        return;
    }

    pci::BarInfo abar = {};
    if (!pci::bar_info(hba, kAbarIndex, abar) || abar.io_space || abar.base == 0) {
        console::write_line("ahci: HBA without a memory ABAR");
        return;
    }
    void* abar_virtual = nullptr;
    if (!vm::map_kernel_mmio(abar.base, static_cast<size_t>(abar.size),
            vm::kPageWriteThrough | vm::kPageCacheDisable, &abar_virtual)) {
        console::write_line("ahci: cannot map ABAR");
        return;
    }
    g_abar = static_cast<volatile uint8_t*>(abar_virtual);

    uint16_t command = pci::read_config_u16(hba.bus, hba.slot, hba.function, kPciCommandOffset);
    command = static_cast<uint16_t>((command | kPciCommandMemory | kPciCommandBusMaster) | kPciCommandInterruptDisable);
    pci::write_config_u16(hba.bus, hba.slot, hba.function, kPciCommandOffset, command);

    claim_from_firmware();
    g_s64a = (read_reg(g_abar, kHbaCap) & kCapS64a) != 0;
    // AHCI mode, interrupts off: requests are completed by polling.
    write_reg(g_abar, kHbaGhc, (read_reg(g_abar, kHbaGhc) | kGhcAhciEnable) & ~kGhcInterruptEnable);

    if (!memory::allocate_contiguous_pages(kBouncePages, g_bounce) || !address_fits(g_bounce.physical_address)) {
        console::write_line("ahci: no memory for the DMA buffer");
        return;
    }

    const uint32_t implemented = read_reg(g_abar, kHbaPortsImplemented);
    for (uint8_t port_index = 0; port_index < 32 && g_disk_count < kMaxDisks; ++port_index) {
        if ((implemented & (1u << port_index)) == 0) {
            continue;
        }
        Disk& disk = g_disks[g_disk_count];
        if (!bring_up_port(port_index, disk)) {
            continue;
        }
        ++g_disk_count;
        (void)block::register_device(kOps, &disk, disk.sector_count, /*writable=*/true, disk.name);
        console::printf("ahci: %s port %u, %u sectors\n", disk.name,
            static_cast<unsigned>(port_index), static_cast<unsigned>(disk.sector_count));
    }
}

// Above ata (100), below virtio-blk (110): the three never serve the same disk,
// so this only fixes the device order if they coexist on different disks.
constexpr int kDriverPriority = 105;

const block::Driver kDriver = {
    "ahci",
    kDriverPriority,
    &enumerate,
    nullptr,
};

} // namespace

namespace ahci {

const block::Driver& driver() { return kDriver; }

} // namespace ahci
