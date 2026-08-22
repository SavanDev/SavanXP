#include "kernel/ata.hpp"

#include <stddef.h>
#include <stdint.h>

namespace {

constexpr uint16_t kPrimaryIoBase = 0x1f0;
constexpr uint16_t kPrimaryControlBase = 0x3f6;
constexpr uint16_t kSecondaryIoBase = 0x170;
constexpr uint16_t kSecondaryControlBase = 0x376;
constexpr size_t kSlotCount = 4;

constexpr uint8_t kStatusErr = 0x01;
constexpr uint8_t kStatusDrq = 0x08;
constexpr uint8_t kStatusDfq = 0x20;
constexpr uint8_t kStatusBsy = 0x80;

constexpr uint8_t kCommandIdentify = 0xec;
constexpr uint8_t kCommandReadSectors = 0x20;
constexpr uint8_t kCommandWriteSectors = 0x30;
constexpr uint8_t kCommandCacheFlush = 0xe7;

// Tope de sectores por comando del PIO de 28 bits. Es del protocolo ATA, no del
// contrato de block::, asi que la validacion vive aca.
constexpr uint32_t kMaxSectorsPerCommand = 255;

// Prioridad alta: los ATA enumeran antes que el ramdisk, asi un disco IDE
// persistente (dev) le gana a la imagen del LiveCD cuando svfs elige que montar.
constexpr int kDriverPriority = 100;

struct Slot {
    uint16_t io_base;
    uint16_t control_base;
    uint8_t drive_select;
    uint32_t sector_count;
};

Slot g_slots[kSlotCount] = {
    {kPrimaryIoBase, kPrimaryControlBase, 0x00, 0},
    {kPrimaryIoBase, kPrimaryControlBase, 0x10, 0},
    {kSecondaryIoBase, kSecondaryControlBase, 0x00, 0},
    {kSecondaryIoBase, kSecondaryControlBase, 0x10, 0},
};

const char* const kSlotNames[kSlotCount] = {"ata0", "ata1", "ata2", "ata3"};

void outb(uint16_t port, uint8_t value) {
    asm volatile("outb %0, %1" : : "a"(value), "Nd"(port));
}

uint8_t inb(uint16_t port) {
    uint8_t value = 0;
    asm volatile("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

uint16_t inw(uint16_t port) {
    uint16_t value = 0;
    asm volatile("inw %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

void outw(uint16_t port, uint16_t value) {
    asm volatile("outw %0, %1" : : "a"(value), "Nd"(port));
}

void io_wait() {
    outb(0x80, 0);
}

void wait_400ns(const Slot& slot) {
    (void)inb(slot.control_base);
    (void)inb(slot.control_base);
    (void)inb(slot.control_base);
    (void)inb(slot.control_base);
}

void select_drive(const Slot& slot, uint32_t lba) {
    outb(
        static_cast<uint16_t>(slot.io_base + 6),
        static_cast<uint8_t>(0xe0u | slot.drive_select | ((lba >> 24) & 0x0f))
    );
    wait_400ns(slot);
}

bool poll_status(const Slot& slot, bool require_drq) {
    uint8_t status = 0;
    uint32_t spins = 1000000;
    do {
        status = inb(static_cast<uint16_t>(slot.io_base + 7));
    } while ((status & kStatusBsy) != 0 && --spins != 0);
    if (spins == 0) {
        return false;
    }

    if ((status & (kStatusErr | kStatusDfq)) != 0) {
        return false;
    }

    if (require_drq) {
        spins = 1000000;
        while ((status & kStatusDrq) == 0 && --spins != 0) {
            status = inb(static_cast<uint16_t>(slot.io_base + 7));
            if ((status & (kStatusErr | kStatusDfq)) != 0) {
                return false;
            }
        }
        if ((status & kStatusDrq) == 0) {
            return false;
        }
    }

    return true;
}

bool identify(Slot& slot) {
    slot.sector_count = 0;

    select_drive(slot, 0);
    outb(static_cast<uint16_t>(slot.control_base), 0);
    outb(static_cast<uint16_t>(slot.io_base + 2), 0);
    outb(static_cast<uint16_t>(slot.io_base + 3), 0);
    outb(static_cast<uint16_t>(slot.io_base + 4), 0);
    outb(static_cast<uint16_t>(slot.io_base + 5), 0);
    outb(static_cast<uint16_t>(slot.io_base + 7), kCommandIdentify);

    const uint8_t initial_status = inb(static_cast<uint16_t>(slot.io_base + 7));
    if (initial_status == 0) {
        return false;
    }

    uint8_t lba_mid = inb(static_cast<uint16_t>(slot.io_base + 4));
    uint8_t lba_high = inb(static_cast<uint16_t>(slot.io_base + 5));
    if (lba_mid != 0 || lba_high != 0) {
        return false;
    }

    if (!poll_status(slot, true)) {
        return false;
    }

    uint16_t identify_data[256] = {};
    for (size_t index = 0; index < 256; ++index) {
        identify_data[index] = inw(slot.io_base);
    }

    const uint32_t sector_count =
        static_cast<uint32_t>(identify_data[60]) |
        (static_cast<uint32_t>(identify_data[61]) << 16);
    if (sector_count == 0) {
        return false;
    }

    slot.sector_count = sector_count;
    return true;
}

// El rango de LBA ya lo valido block::read/write; aca solo queda el tope por
// comando del protocolo.
bool rw_sectors(Slot& slot, uint32_t lba, uint32_t sector_count, void* buffer, bool write) {
    if (sector_count > kMaxSectorsPerCommand) {
        return false;
    }

    auto* bytes = static_cast<uint8_t*>(buffer);

    select_drive(slot, lba);
    outb(static_cast<uint16_t>(slot.io_base + 1), 0);
    outb(static_cast<uint16_t>(slot.io_base + 2), static_cast<uint8_t>(sector_count));
    outb(static_cast<uint16_t>(slot.io_base + 3), static_cast<uint8_t>(lba & 0xff));
    outb(static_cast<uint16_t>(slot.io_base + 4), static_cast<uint8_t>((lba >> 8) & 0xff));
    outb(static_cast<uint16_t>(slot.io_base + 5), static_cast<uint8_t>((lba >> 16) & 0xff));
    outb(static_cast<uint16_t>(slot.io_base + 7), write ? kCommandWriteSectors : kCommandReadSectors);

    for (uint32_t sector = 0; sector < sector_count; ++sector) {
        if (!poll_status(slot, true)) {
            return false;
        }

        if (write) {
            for (size_t word = 0; word < (block::kSectorSize / sizeof(uint16_t)); ++word) {
                const size_t byte_index = static_cast<size_t>(sector) * block::kSectorSize + word * sizeof(uint16_t);
                const uint16_t value =
                    static_cast<uint16_t>(bytes[byte_index]) |
                    (static_cast<uint16_t>(bytes[byte_index + 1]) << 8);
                outw(slot.io_base, value);
            }
        } else {
            for (size_t word = 0; word < (block::kSectorSize / sizeof(uint16_t)); ++word) {
                const uint16_t value = inw(slot.io_base);
                const size_t byte_index = static_cast<size_t>(sector) * block::kSectorSize + word * sizeof(uint16_t);
                bytes[byte_index] = static_cast<uint8_t>(value & 0xff);
                bytes[byte_index + 1] = static_cast<uint8_t>((value >> 8) & 0xff);
            }
        }
    }

    if (write) {
        outb(static_cast<uint16_t>(slot.io_base + 7), kCommandCacheFlush);
        if (!poll_status(slot, false)) {
            return false;
        }
    }

    return true;
}

bool read_op(void* context, uint32_t lba, uint32_t sector_count, void* buffer) {
    return rw_sectors(*static_cast<Slot*>(context), lba, sector_count, buffer, false);
}

bool write_op(void* context, uint32_t lba, uint32_t sector_count, const void* buffer) {
    return rw_sectors(*static_cast<Slot*>(context), lba, sector_count, const_cast<void*>(buffer), true);
}

const block::DeviceOps kOps = {
    &read_op,
    &write_op,
};

void enumerate() {
    for (size_t index = 0; index < kSlotCount; ++index) {
        if (identify(g_slots[index])) {
            (void)block::register_device(
                kOps, &g_slots[index], g_slots[index].sector_count, /*writable=*/true, kSlotNames[index]);
        }
        io_wait();
    }
}

const block::Driver kDriver = {
    "ata",
    kDriverPriority,
    &enumerate,
};

} // namespace

namespace ata {

const block::Driver& driver() { return kDriver; }

} // namespace ata
