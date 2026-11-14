/*
 * NVMe driver. The controller bring-up order, the shutdown sequence, the
 * register layout and the wire formats are taken from OpenBSD's nvme(4)
 * (sys/dev/ic/nvme.c and nvmereg.h, commit 287e5b695b75cb0c9c794575ed99e90305f680e8);
 * the rest is SavanXP's. See docs/kernel/NVME.md.
 *
 * Copyright (c) 2014 David Gwynne <dlg@openbsd.org>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include "kernel/nvme.hpp"

#include <stddef.h>
#include <stdint.h>

#include "arch/cpu.hpp"
#include "kernel/console.hpp"
#include "kernel/pci.hpp"
#include "kernel/physical_memory.hpp"
#include "kernel/string.hpp"
#include "kernel/timer.hpp"
#include "kernel/vmm.hpp"

namespace {

// PCI class of an NVMe controller: mass storage, non-volatile memory, NVM
// Express programming interface.
constexpr uint8_t kClassMassStorage = 0x01;
constexpr uint8_t kSubclassNvm = 0x08;
constexpr uint8_t kProgIfNvme = 0x02;
constexpr uint8_t kBarIndex = 0;

constexpr uint8_t kPciCommandOffset = 0x04;
constexpr uint16_t kPciCommandMemory = 1u << 1;
constexpr uint16_t kPciCommandBusMaster = 1u << 2;
constexpr uint16_t kPciCommandInterruptDisable = 1u << 10;

// Controller registers (offsets from BAR0), nvmereg.h.
constexpr uint32_t kRegCap = 0x00;   // 64-bit
constexpr uint32_t kRegVs = 0x08;
constexpr uint32_t kRegIntms = 0x0c;
constexpr uint32_t kRegCc = 0x14;
constexpr uint32_t kRegCsts = 0x1c;
constexpr uint32_t kRegAqa = 0x24;
constexpr uint32_t kRegAsq = 0x28;   // 64-bit
constexpr uint32_t kRegAcq = 0x30;   // 64-bit
constexpr uint32_t kDoorbellBase = 0x1000;

constexpr uint32_t kCcEnable = 1u << 0;
constexpr uint32_t kCcCssMask = 0x7u << 4;
constexpr uint32_t kCcMpsMask = 0xfu << 7;
constexpr uint32_t kCcAmsMask = 0x7u << 11;
constexpr uint32_t kCcShnMask = 0x3u << 14;
constexpr uint32_t kCcShnNormal = 0x1u << 14;
constexpr uint32_t kCcIosqesMask = 0xfu << 16;
constexpr uint32_t kCcIocqesMask = 0xfu << 20;
// 2^6 = 64-byte submission entries, 2^4 = 16-byte completion entries.
constexpr uint32_t kCcQueueEntrySizes = (6u << 16) | (4u << 20);

constexpr uint32_t kCstsReady = 1u << 0;
constexpr uint32_t kCstsFatal = 1u << 1;
constexpr uint32_t kCstsShstMask = 0x3u << 2;
constexpr uint32_t kCstsShstDone = 0x2u << 2;

// Admin and NVM command opcodes.
constexpr uint8_t kAdminDeleteSq = 0x00;
constexpr uint8_t kAdminCreateSq = 0x01;
constexpr uint8_t kAdminDeleteCq = 0x04;
constexpr uint8_t kAdminCreateCq = 0x05;
constexpr uint8_t kAdminIdentify = 0x06;
constexpr uint8_t kNvmFlush = 0x00;
constexpr uint8_t kNvmWrite = 0x01;
constexpr uint8_t kNvmRead = 0x02;

constexpr uint32_t kIdentifyNamespace = 0;
constexpr uint32_t kIdentifyController = 1;

constexpr uint16_t kCqePhase = 1u << 0;

// Identify Controller fields (byte offsets, nvm_identify_controller).
constexpr size_t kIdCtrlSerial = 4;    // sn[20]
constexpr size_t kIdCtrlModel = 24;    // mn[40]
constexpr size_t kIdCtrlFirmware = 64; // fr[8]
constexpr size_t kIdCtrlMdts = 77;
constexpr size_t kIdCtrlNn = 516;
// Identify Namespace fields (nvm_identify_namespace).
constexpr size_t kIdNsNsze = 0;
constexpr size_t kIdNsNcap = 8;
constexpr size_t kIdNsNsfeat = 24;
constexpr size_t kIdNsFlbas = 26;
constexpr size_t kIdNsLbaf = 128; // 16 x {u16 ms, u8 lbads, u8 rp}
constexpr uint8_t kNsfeatThinProvisioning = 1u << 0;

constexpr uint16_t kAdminQueueEntries = 16;
constexpr uint16_t kIoQueueEntries = 64;
constexpr uint16_t kIoQueueId = 1;

// Data goes through one physically contiguous bounce buffer, as in ahci.cpp.
// A request of more than two pages points PRP2 at a list of the remaining
// pages, which fits in one page for anything up to 2 MiB.
constexpr uint32_t kMaxSectorsPerCommand = 256;
constexpr size_t kBounceBytes = static_cast<size_t>(kMaxSectorsPerCommand) * block::kSectorSize;
constexpr uint64_t kBouncePages = kBounceBytes / memory::kPageSize;
static_assert(kBounceBytes % memory::kPageSize == 0);

// Timeouts. OpenBSD's: 5 s for queue operations, 10 s for Identify; a data
// command gets 30 s, which only a dying disk reaches.
constexpr uint64_t kAdminTimeoutMs = 10000;
constexpr uint64_t kIoTimeoutMs = 30000;
constexpr uint64_t kShutdownTimeoutMs = 4000;
// The backstop when the monotonic clock is not calibrated: a spin is an MMIO
// or DMA-memory read, a microsecond or less.
constexpr uint64_t kSpinsPerMs = 2000;

constexpr size_t kMaxControllers = 2;
constexpr size_t kMaxDisks = 4;
// Namespaces looked at per controller. Consumer drives have one; some
// controllers report hundreds of mostly inactive ones.
constexpr uint32_t kMaxNamespacesScanned = 8;

struct [[gnu::packed]] SubmissionEntry {
    uint8_t opcode;
    uint8_t flags;
    uint16_t cid;
    uint32_t nsid;
    uint64_t reserved;
    uint64_t mptr;
    uint64_t prp1;
    uint64_t prp2;
    uint32_t cdw10;
    uint32_t cdw11;
    uint32_t cdw12;
    uint32_t cdw13;
    uint32_t cdw14;
    uint32_t cdw15;
};
static_assert(sizeof(SubmissionEntry) == 64);

struct [[gnu::packed]] CompletionEntry {
    uint32_t cdw0;
    uint32_t reserved;
    uint16_t sqhd;
    uint16_t sqid;
    uint16_t cid;
    uint16_t flags; // phase in bit 0, status in bits 1-15
};
static_assert(sizeof(CompletionEntry) == 16);

struct Queue {
    memory::PageAllocation sq_memory;
    memory::PageAllocation cq_memory;
    uint16_t id;
    uint16_t entries;
    uint16_t sq_tail;
    uint16_t cq_head;
    uint16_t phase;
    uint32_t sq_doorbell;
    uint32_t cq_doorbell;
};

struct Controller {
    volatile uint8_t* registers;
    uint32_t doorbell_stride;
    uint64_t ready_timeout_ms;
    uint32_t max_sectors;
    uint16_t next_cid;
    // A command timed out or the controller reported a fatal status: the
    // queues can no longer be trusted, so every later request fails.
    bool failed;
    bool io_queue_created;
    Queue admin;
    Queue io;
};

struct Disk {
    Controller* controller;
    uint32_t nsid;
    uint32_t sector_count;
    char name[8];
    block::DeviceIdentity identity;
};

Controller g_controllers[kMaxControllers] = {};
size_t g_controller_count = 0;
Disk g_disks[kMaxDisks] = {};
size_t g_disk_count = 0;
memory::PageAllocation g_bounce = {};
memory::PageAllocation g_prp_list = {};
memory::PageAllocation g_identify = {};
bool g_writable = false;

uint32_t read32(const Controller& controller, uint32_t offset) {
    return *reinterpret_cast<volatile uint32_t*>(controller.registers + offset);
}

void write32(const Controller& controller, uint32_t offset, uint32_t value) {
    *reinterpret_cast<volatile uint32_t*>(controller.registers + offset) = value;
}

// 64-bit registers as two 32-bit accesses, low half first: the specification
// allows it and some bridges do not pass 64-bit MMIO.
uint64_t read64(const Controller& controller, uint32_t offset) {
    const uint64_t low = read32(controller, offset);
    const uint64_t high = read32(controller, offset + 4);
    return low | (high << 32);
}

void write64(const Controller& controller, uint32_t offset, uint64_t value) {
    write32(controller, offset, static_cast<uint32_t>(value));
    write32(controller, offset + 4, static_cast<uint32_t>(value >> 32));
}

// A deadline on the monotonic clock, or a spin count when the clock is not
// running yet (before calibration it reads 0).
struct Deadline {
    uint64_t end_ms;
    uint64_t spins_left;
    bool clock;
    uint32_t spins;

    explicit Deadline(uint64_t timeout_ms)
        : end_ms(timer::monotonic_ms() + timeout_ms), spins_left(timeout_ms * kSpinsPerMs),
          clock(timer::monotonic_ms() != 0), spins(0) {}

    bool expired() {
        if (!clock) {
            return spins_left-- == 0;
        }
        // Reading the clock may be a port read (the PM timer): not every spin.
        if ((++spins & 0xffu) != 0) {
            return false;
        }
        return timer::monotonic_ms() >= end_ms;
    }
};

bool wait_ready(const Controller& controller, bool ready) {
    Deadline deadline(controller.ready_timeout_ms);
    while (((read32(controller, kRegCsts) & kCstsReady) != 0) != ready) {
        if (deadline.expired()) {
            return false;
        }
    }
    return true;
}

// nvme_disable(): a controller that is enabled but not yet ready must become
// ready before it may be disabled, unless it already reports a fatal status.
bool disable(Controller& controller) {
    uint32_t cc = read32(controller, kRegCc);
    if ((cc & kCcEnable) != 0) {
        if ((read32(controller, kRegCsts) & kCstsFatal) == 0 && !wait_ready(controller, true)) {
            return false;
        }
    }
    cc &= ~kCcEnable;
    write32(controller, kRegCc, cc);
    return wait_ready(controller, false);
}

// nvme_enable(): admin queues, then the configuration with EN set.
bool enable(Controller& controller) {
    write32(controller, kRegAqa,
        (static_cast<uint32_t>(controller.admin.entries - 1) << 16) |
        static_cast<uint32_t>(controller.admin.entries - 1));
    write64(controller, kRegAsq, controller.admin.sq_memory.physical_address);
    write64(controller, kRegAcq, controller.admin.cq_memory.physical_address);

    uint32_t cc = read32(controller, kRegCc);
    cc &= ~(kCcIocqesMask | kCcIosqesMask | kCcShnMask | kCcAmsMask | kCcMpsMask | kCcCssMask);
    // NVM command set, round-robin arbitration, 4 KiB memory pages (MPS 0).
    cc |= kCcQueueEntrySizes | kCcEnable;
    write32(controller, kRegCc, cc);
    return wait_ready(controller, true);
}

bool allocate_queue(Controller& controller, Queue& queue, uint16_t id, uint16_t entries) {
    queue = {};
    if (!memory::allocate_page(queue.sq_memory)) {
        return false;
    }
    if (!memory::allocate_page(queue.cq_memory)) {
        (void)memory::free_allocation(queue.sq_memory);
        queue.sq_memory = {};
        return false;
    }
    memset(queue.sq_memory.virtual_address, 0, memory::kPageSize);
    memset(queue.cq_memory.virtual_address, 0, memory::kPageSize);
    queue.id = id;
    queue.entries = entries;
    queue.phase = kCqePhase;
    queue.sq_doorbell = kDoorbellBase + (2u * id) * controller.doorbell_stride;
    queue.cq_doorbell = kDoorbellBase + (2u * id + 1u) * controller.doorbell_stride;
    return true;
}

void free_queue(Queue& queue) {
    if (queue.sq_memory.virtual_address != nullptr) {
        (void)memory::free_allocation(queue.sq_memory);
    }
    if (queue.cq_memory.virtual_address != nullptr) {
        (void)memory::free_allocation(queue.cq_memory);
    }
    queue = {};
}

// Submits one command and polls its completion (nvme_poll() and
// nvme_q_complete() collapsed for one command in flight). Returns the status
// field, 0 on success, or -1 if the controller failed.
int execute(Controller& controller, Queue& queue, SubmissionEntry command, uint64_t timeout_ms) {
    if (controller.failed) {
        return -1;
    }
    command.cid = controller.next_cid++;
    auto* ring = static_cast<SubmissionEntry*>(queue.sq_memory.virtual_address);
    ring[queue.sq_tail] = command;
    queue.sq_tail = static_cast<uint16_t>((queue.sq_tail + 1u) % queue.entries);
    arch::memory_barrier();
    write32(controller, queue.sq_doorbell, queue.sq_tail);

    auto* completions = static_cast<volatile CompletionEntry*>(queue.cq_memory.virtual_address);
    Deadline deadline(timeout_ms);
    for (;;) {
        volatile CompletionEntry& entry = completions[queue.cq_head];
        const uint16_t flags = entry.flags;
        if ((flags & kCqePhase) == queue.phase) {
            arch::memory_barrier();
            const uint16_t cid = entry.cid;
            if (++queue.cq_head == queue.entries) {
                queue.cq_head = 0;
                queue.phase ^= kCqePhase;
            }
            write32(controller, queue.cq_doorbell, queue.cq_head);
            if (cid != command.cid) {
                // Nothing else is ever in flight; a stray entry means the
                // queue state is not what we think it is.
                console::printf("nvme: queue %u completed cid %u, expected %u\n",
                    static_cast<unsigned>(queue.id), static_cast<unsigned>(cid),
                    static_cast<unsigned>(command.cid));
                controller.failed = true;
                return -1;
            }
            return static_cast<int>(flags >> 1);
        }
        if (deadline.expired() || (read32(controller, kRegCsts) & kCstsFatal) != 0) {
            console::printf("nvme: opcode %x on queue %u timed out, csts=%x; controller disabled\n",
                static_cast<unsigned>(command.opcode), static_cast<unsigned>(queue.id),
                static_cast<unsigned>(read32(controller, kRegCsts)));
            controller.failed = true;
            return -1;
        }
    }
}

bool admin(Controller& controller, const SubmissionEntry& command) {
    const int status = execute(controller, controller.admin, command, kAdminTimeoutMs);
    if (status > 0) {
        console::printf("nvme: admin opcode %x failed, status %x\n",
            static_cast<unsigned>(command.opcode), static_cast<unsigned>(status));
    }
    return status == 0;
}

bool identify(Controller& controller, uint32_t cns, uint32_t nsid) {
    memset(g_identify.virtual_address, 0, memory::kPageSize);
    SubmissionEntry command = {};
    command.opcode = kAdminIdentify;
    command.nsid = nsid;
    command.prp1 = g_identify.physical_address;
    command.cdw10 = cns;
    return admin(controller, command);
}

// nvme_q_create(): the completion queue first, then the submission queue
// that posts to it. Physically contiguous, interrupts off: requests are
// completed by polling.
bool create_io_queue(Controller& controller) {
    Queue& queue = controller.io;
    SubmissionEntry command = {};
    command.opcode = kAdminCreateCq;
    command.prp1 = queue.cq_memory.physical_address;
    command.cdw10 = (static_cast<uint32_t>(queue.entries - 1) << 16) | queue.id;
    command.cdw11 = 1u; // PC
    if (!admin(controller, command)) {
        return false;
    }
    command = {};
    command.opcode = kAdminCreateSq;
    command.prp1 = queue.sq_memory.physical_address;
    command.cdw10 = (static_cast<uint32_t>(queue.entries - 1) << 16) | queue.id;
    command.cdw11 = (static_cast<uint32_t>(queue.id) << 16) | 1u; // CQID, PC
    return admin(controller, command);
}

// Copies an Identify string (ASCII, space padded, not byte swapped like
// ATA's) without the padding.
void copy_identify_string(const uint8_t* source, size_t length, char* out, size_t capacity) {
    size_t end = length;
    while (end > 0 && (source[end - 1] == ' ' || source[end - 1] == 0)) {
        --end;
    }
    size_t start = 0;
    while (start < end && source[start] == ' ') {
        ++start;
    }
    size_t written = 0;
    for (size_t i = start; i < end && written + 1 < capacity; ++i) {
        const uint8_t c = source[i];
        out[written++] = (c >= 0x20 && c < 0x7f) ? static_cast<char>(c) : '?';
    }
    out[written] = 0;
}

uint64_t load64(const uint8_t* bytes) {
    uint64_t value = 0;
    memcpy(&value, bytes, sizeof(value));
    return value;
}

uint32_t load32(const uint8_t* bytes) {
    uint32_t value = 0;
    memcpy(&value, bytes, sizeof(value));
    return value;
}

// Points PRP1/PRP2 at the first `bytes` of the bounce buffer.
void fill_prps(SubmissionEntry& command, size_t bytes) {
    const uint64_t base = g_bounce.physical_address;
    command.prp1 = base;
    const size_t pages = (bytes + memory::kPageSize - 1) / memory::kPageSize;
    if (pages <= 1) {
        return;
    }
    if (pages == 2) {
        command.prp2 = base + memory::kPageSize;
        return;
    }
    auto* list = static_cast<uint64_t*>(g_prp_list.virtual_address);
    for (size_t page = 1; page < pages; ++page) {
        list[page - 1] = base + page * memory::kPageSize;
    }
    command.prp2 = g_prp_list.physical_address;
}

bool rw_sectors(void* context, uint32_t lba, uint32_t sector_count, void* buffer, bool write) {
    Disk& disk = *static_cast<Disk*>(context);
    Controller& controller = *disk.controller;
    auto* bytes = static_cast<uint8_t*>(buffer);
    uint8_t* bounce = static_cast<uint8_t*>(g_bounce.virtual_address);
    uint64_t sector = lba;
    while (sector_count != 0) {
        const uint32_t chunk = sector_count < controller.max_sectors ? sector_count : controller.max_sectors;
        const size_t chunk_bytes = static_cast<size_t>(chunk) * block::kSectorSize;
        if (write) {
            memcpy(bounce, bytes, chunk_bytes);
        }
        SubmissionEntry command = {};
        command.opcode = write ? kNvmWrite : kNvmRead;
        command.nsid = disk.nsid;
        fill_prps(command, chunk_bytes);
        command.cdw10 = static_cast<uint32_t>(sector);
        command.cdw11 = static_cast<uint32_t>(sector >> 32);
        command.cdw12 = chunk - 1u; // NLB, zero based
        const int status = execute(controller, controller.io, command, kIoTimeoutMs);
        if (status != 0) {
            if (status > 0) {
                console::printf("nvme: %s %s lba %u failed, status %x\n", disk.name,
                    write ? "write" : "read", static_cast<unsigned>(sector), static_cast<unsigned>(status));
            }
            return false;
        }
        if (!write) {
            memcpy(bytes, bounce, chunk_bytes);
        }
        sector += chunk;
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
    SubmissionEntry command = {};
    command.opcode = kNvmFlush;
    command.nsid = disk.nsid;
    return execute(*disk.controller, disk.controller->io, command, kIoTimeoutMs) == 0;
}

const block::DeviceOps kOps = {
    &read_op,
    &write_op,
    &flush_op,
    /*deferred_writes=*/true,
};

void add_namespace(Controller& controller, uint32_t nsid) {
    if (g_disk_count == kMaxDisks || !identify(controller, kIdentifyNamespace, nsid)) {
        return;
    }
    const auto* data = static_cast<const uint8_t*>(g_identify.virtual_address);
    // nvme_scsi_size(): a thin-provisioned namespace may hold less than its size.
    uint64_t sectors = load64(data + kIdNsNsze);
    const uint64_t capacity = load64(data + kIdNsNcap);
    if ((data[kIdNsNsfeat] & kNsfeatThinProvisioning) != 0 && capacity < sectors) {
        sectors = capacity;
    }
    if (sectors == 0) {
        return; // inactive
    }
    const uint8_t* format = data + kIdNsLbaf + 4u * (data[kIdNsFlbas] & 0x0fu);
    const uint16_t metadata_bytes = static_cast<uint16_t>(format[0] | (format[1] << 8));
    const uint8_t block_shift = format[2];
    if (block_shift != 9 || metadata_bytes != 0) {
        // block:: is built on 512-byte sectors; a 4 KiB namespace would need
        // read-modify-write under every write. Reformatting it to 512 bytes
        // (nvme-cli format --lbaf) makes it usable.
        console::printf("nvme: namespace %u has %u-byte blocks with %u bytes of metadata, not supported\n",
            static_cast<unsigned>(nsid), 1u << block_shift, static_cast<unsigned>(metadata_bytes));
        return;
    }

    Disk& disk = g_disks[g_disk_count];
    disk = {};
    disk.controller = &controller;
    disk.nsid = nsid;
    // block:: addresses 32-bit sector counts; a bigger namespace is exposed up
    // to its first 2 TiB, as in ahci.cpp.
    disk.sector_count = sectors > 0xffffffffu ? 0xffffffffu : static_cast<uint32_t>(sectors);
    disk.identity.full_sectors = sectors;
    disk.name[0] = 'n';
    disk.name[1] = 'v';
    disk.name[2] = 'm';
    disk.name[3] = 'e';
    disk.name[4] = static_cast<char>('0' + g_disk_count);
    disk.name[5] = 0;
    ++g_disk_count;
}

void finish_disks(size_t first, const char* model, const char* serial) {
    for (size_t index = first; index < g_disk_count; ++index) {
        Disk& disk = g_disks[index];
        memcpy(disk.identity.model, model, sizeof(disk.identity.model));
        memcpy(disk.identity.serial, serial, sizeof(disk.identity.serial));
        (void)block::register_device(kOps, &disk, disk.sector_count, g_writable, disk.name, &disk.identity);
        console::printf("nvme: %s namespace %u, %u sectors, \"%s\" serial \"%s\"%s\n", disk.name,
            static_cast<unsigned>(disk.nsid), static_cast<unsigned>(disk.sector_count),
            disk.identity.model, disk.identity.serial, g_writable ? "" : ", read-only");
    }
}

// nvme_attach(), without the SCSI bus, the sensors and the interrupt.
void attach(const pci::DeviceInfo& device) {
    pci::BarInfo bar = {};
    if (!pci::bar_info(device, kBarIndex, bar) || bar.io_space || bar.base == 0) {
        console::write_line("nvme: controller without a memory BAR0");
        return;
    }
    void* registers = nullptr;
    if (!vm::map_kernel_mmio(bar.base, static_cast<size_t>(bar.size),
            vm::kPageWriteThrough | vm::kPageCacheDisable, &registers)) {
        console::write_line("nvme: cannot map BAR0");
        return;
    }
    uint16_t pci_command = pci::read_config_u16(device.bus, device.slot, device.function, kPciCommandOffset);
    pci_command = static_cast<uint16_t>(pci_command | kPciCommandMemory | kPciCommandBusMaster |
        kPciCommandInterruptDisable);
    pci::write_config_u16(device.bus, device.slot, device.function, kPciCommandOffset, pci_command);

    Controller& controller = g_controllers[g_controller_count];
    controller = {};
    controller.registers = static_cast<volatile uint8_t*>(registers);

    const uint32_t version = read32(controller, kRegVs);
    if (version == 0xffffffffu) {
        console::write_line("nvme: BAR0 reads all ones");
        return;
    }
    const uint64_t cap = read64(controller, kRegCap);
    controller.doorbell_stride = 4u << ((cap >> 32) & 0xfu);
    controller.ready_timeout_ms = 500u * ((cap >> 24) & 0xffu);
    if (controller.ready_timeout_ms == 0) {
        controller.ready_timeout_ms = 500;
    }
    const uint32_t max_entries = static_cast<uint32_t>(cap & 0xffffu) + 1u;
    if (((cap >> 48) & 0xfu) != 0) {
        console::printf("nvme: minimum page size %u is above 4096\n", 1u << (12 + ((cap >> 48) & 0xfu)));
        return;
    }
    if (((cap >> 37) & 1u) == 0) {
        console::write_line("nvme: controller does not support the NVM command set");
        return;
    }
    console::printf("nvme: controller %x:%x, NVMe %u.%u\n", static_cast<unsigned>(device.vendor_id),
        static_cast<unsigned>(device.device_id), static_cast<unsigned>(version >> 16),
        static_cast<unsigned>((version >> 8) & 0xffu));

    if (!disable(controller)) {
        console::write_line("nvme: controller does not disable");
        return;
    }
    const uint16_t admin_entries = static_cast<uint16_t>(max_entries < kAdminQueueEntries ? max_entries : kAdminQueueEntries);
    const uint16_t io_entries = static_cast<uint16_t>(max_entries < kIoQueueEntries ? max_entries : kIoQueueEntries);
    if (!allocate_queue(controller, controller.admin, 0, admin_entries)) {
        console::write_line("nvme: no memory for the admin queue");
        return;
    }
    if (!enable(controller)) {
        console::printf("nvme: controller does not become ready, csts=%x\n",
            static_cast<unsigned>(read32(controller, kRegCsts)));
        (void)disable(controller);
        free_queue(controller.admin);
        return;
    }
    // Interrupts masked as well as off at the PCI level.
    write32(controller, kRegIntms, 0xffffffffu);

    if (!identify(controller, kIdentifyController, 0)) {
        console::write_line("nvme: Identify Controller failed");
        (void)disable(controller);
        free_queue(controller.admin);
        return;
    }
    const auto* data = static_cast<const uint8_t*>(g_identify.virtual_address);
    char model[sizeof(block::DeviceIdentity::model)] = {};
    char serial[sizeof(block::DeviceIdentity::serial)] = {};
    char firmware[9] = {};
    copy_identify_string(data + kIdCtrlModel, 40, model, sizeof(model));
    copy_identify_string(data + kIdCtrlSerial, 20, serial, sizeof(serial));
    copy_identify_string(data + kIdCtrlFirmware, 8, firmware, sizeof(firmware));
    const uint8_t mdts = data[kIdCtrlMdts];
    uint32_t namespaces = load32(data + kIdCtrlNn);
    console::printf("nvme: \"%s\", firmware %s, %u namespace(s), mdts %u\n", model, firmware,
        static_cast<unsigned>(namespaces), static_cast<unsigned>(mdts));

    // MDTS is a power of two in units of the minimum page size (4 KiB here);
    // 0 means no limit.
    controller.max_sectors = kMaxSectorsPerCommand;
    if (mdts != 0 && mdts < 20) {
        const uint64_t limit = (static_cast<uint64_t>(memory::kPageSize) << mdts) / block::kSectorSize;
        if (limit < controller.max_sectors) {
            controller.max_sectors = static_cast<uint32_t>(limit);
        }
    }

    if (!allocate_queue(controller, controller.io, kIoQueueId, io_entries) || !create_io_queue(controller)) {
        console::write_line("nvme: cannot create the I/O queue");
        free_queue(controller.io);
        (void)disable(controller);
        free_queue(controller.admin);
        return;
    }
    controller.io_queue_created = true;
    ++g_controller_count;

    const size_t first_disk = g_disk_count;
    if (namespaces > kMaxNamespacesScanned) {
        namespaces = kMaxNamespacesScanned;
    }
    for (uint32_t nsid = 1; nsid <= namespaces; ++nsid) {
        add_namespace(controller, nsid);
    }
    finish_disks(first_disk, model, serial);
}

void enumerate() {
    g_controller_count = 0;
    g_disk_count = 0;
    if (!pci::ready()) {
        return;
    }
    const size_t count = pci::device_count();
    for (size_t index = 0; index < count && g_controller_count < kMaxControllers; ++index) {
        pci::DeviceInfo info = {};
        if (!pci::device_info(index, info) || info.class_code != kClassMassStorage ||
            info.subclass != kSubclassNvm || info.prog_if != kProgIfNvme) {
            continue;
        }
        if (g_bounce.virtual_address == nullptr) {
            if (!memory::allocate_contiguous_pages(kBouncePages, g_bounce) ||
                !memory::allocate_page(g_prp_list) || !memory::allocate_page(g_identify)) {
                console::write_line("nvme: no memory for the DMA buffers");
                return;
            }
        }
        attach(info);
    }
}

// nvme_shutdown(): delete the I/O queues, then a normal shutdown notification,
// so the drive commits its cache and does not count an unsafe shutdown.
void shutdown() {
    for (size_t index = 0; index < g_controller_count; ++index) {
        Controller& controller = g_controllers[index];
        if (controller.registers == nullptr) {
            continue;
        }
        if (!controller.failed && controller.io_queue_created) {
            SubmissionEntry command = {};
            command.opcode = kAdminDeleteSq;
            command.cdw10 = kIoQueueId;
            if (admin(controller, command)) {
                command.opcode = kAdminDeleteCq;
                (void)admin(controller, command);
            }
            controller.io_queue_created = false;
        }
        uint32_t cc = read32(controller, kRegCc);
        cc = (cc & ~kCcShnMask) | kCcShnNormal;
        write32(controller, kRegCc, cc);
        Deadline deadline(kShutdownTimeoutMs);
        bool done = false;
        while (!(done = (read32(controller, kRegCsts) & kCstsShstMask) == kCstsShstDone)) {
            if (deadline.expired()) {
                break;
            }
        }
        if (done) {
            console::printf("nvme: controller %u shut down\n", static_cast<unsigned>(index));
        } else {
            console::write_line("nvme: shutdown did not complete, disabling");
            (void)disable(controller);
        }
        controller.failed = true; // nothing is submitted after this
    }
}

// Above ahci (105), below virtio-blk (110). QEMU attaches disk.img through one
// of them, so this only orders devices on a machine that has several.
constexpr int kDriverPriority = 107;

const block::Driver kDriver = {
    "nvme",
    kDriverPriority,
    &enumerate,
    nullptr,
    &shutdown,
};

} // namespace

namespace nvme {

void set_writable(bool writable) { g_writable = writable; }

const block::Driver& driver() { return kDriver; }

} // namespace nvme
