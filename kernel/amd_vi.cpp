#include "kernel/amd_vi.hpp"

#include "arch/cpu.hpp"
#include "kernel/amd_vi_tables.hpp"
#include "kernel/console.hpp"
#include "kernel/physical_memory.hpp"
#include "kernel/string.hpp"
#include "kernel/timer.hpp"
#include "kernel/vmm.hpp"

namespace amd_vi {

namespace {

using iommu_tables::Report;
using iommu_tables::Unit;

// MMIO registers (48882, "MMIO Registers").
constexpr uint32_t kDeviceTableBase = 0x0000;
constexpr uint32_t kCommandBufferBase = 0x0008;
constexpr uint32_t kEventLogBase = 0x0010;
constexpr uint32_t kControl = 0x0018;
constexpr uint32_t kExclusionBase = 0x0020;
constexpr uint32_t kExclusionLimit = 0x0028;
constexpr uint32_t kExtendedFeatures = 0x0030;
constexpr uint32_t kCommandHead = 0x2000;
constexpr uint32_t kCommandTail = 0x2008;
constexpr uint32_t kEventHead = 0x2010;
constexpr uint32_t kEventTail = 0x2018;
constexpr uint32_t kStatus = 0x2020;

constexpr uint64_t kControlIommuEnable = 1ull << 0;
constexpr uint64_t kControlHtTunnel = 1ull << 1;
constexpr uint64_t kControlEventLog = 1ull << 2;
constexpr uint64_t kControlPassPostedWrite = 1ull << 8;
constexpr uint64_t kControlResponsePassPostedWrite = 1ull << 9;
constexpr uint64_t kControlCoherent = 1ull << 10;
constexpr uint64_t kControlIsochronous = 1ull << 11;
constexpr uint64_t kControlCommandBuffer = 1ull << 12;
// InvTimeOut = 5: an invalidation not answered in a second is an event.
constexpr uint64_t kControlInvalidationTimeout = 5ull << 5;

constexpr uint32_t kStatusEventOverflow = 1u << 0;
constexpr uint32_t kStatusEventLogRunning = 1u << 3;
constexpr uint32_t kStatusCommandBufferRunning = 1u << 4;
// The write-1-to-clear bits: event overflow and interrupt, completion wait,
// PPR overflow and interrupt, GA log overflow and interrupt. Only these are
// written: QEMU keeps any other bit a write sets.
constexpr uint32_t kStatusClearable = 0x667u;

// IVHD flags that ask for the matching control bits.
constexpr uint8_t kIvhdHtTunnel = 1u << 0;
constexpr uint8_t kIvhdPassPostedWrite = 1u << 1;
constexpr uint8_t kIvhdResponsePassPostedWrite = 1u << 2;
constexpr uint8_t kIvhdIsochronous = 1u << 3;

constexpr uint64_t kEfrInvalidateAll = 1ull << 6;

// Both rings are one page: 256 entries of 16 bytes, length code 8 (2^8).
constexpr uint64_t kRingBytes = 4096;
constexpr uint64_t kRingLengthCode = 8ull << 56;
constexpr uint32_t kRingPointerMask = 0x7fff0;
// 65536 entries of 32 bytes: every requester ID of the segment. Size field:
// pages minus one.
constexpr uint64_t kDeviceTablePages = 512;
constexpr uint64_t kDeviceTableSize = kDeviceTablePages - 1;
constexpr uint16_t kIdentityDomain = 1;

constexpr uint64_t kWaitMs = 1000;
// Events logged per drain; a device faulting in a loop would otherwise fill
// the log faster than the serial port empties it.
constexpr unsigned kEventsPerDrain = 16;

struct UnitState {
    volatile uint8_t* registers;
    uint64_t* command_ring;
    uint64_t command_physical;
    uint32_t command_tail;
    uint32_t* event_ring;
    uint64_t event_physical;
    uint64_t control;
};

UnitState g_units[iommu_tables::kMaxUnits];
size_t g_unit_count = 0;
bool g_running = false;
uint64_t g_root = 0;
volatile uint64_t* g_completion = nullptr;
uint64_t g_completion_physical = 0;
uint64_t g_completion_sequence = 0;
uint64_t g_table_pages = 0;
uint64_t g_fault_count = 0;
uint64_t g_overflow_count = 0;
uint16_t g_last_fault_device = 0;
uint64_t g_last_fault_address = 0;

uint32_t read32(const UnitState& unit, uint32_t offset) {
    return *reinterpret_cast<const volatile uint32_t*>(unit.registers + offset);
}

void write32(const UnitState& unit, uint32_t offset, uint32_t value) {
    *reinterpret_cast<volatile uint32_t*>(unit.registers + offset) = value;
}

uint64_t read64(const UnitState& unit, uint32_t offset) {
    const uint64_t low = read32(unit, offset);
    return low | (static_cast<uint64_t>(read32(unit, offset + 4)) << 32);
}

// Low half first, as the specification allows for every 64-bit register.
void write64(const UnitState& unit, uint32_t offset, uint64_t value) {
    write32(unit, offset, static_cast<uint32_t>(value));
    write32(unit, offset + 4, static_cast<uint32_t>(value >> 32));
}

void set_control(UnitState& unit, uint64_t control) {
    unit.control = control;
    write64(unit, kControl, control);
}

uint64_t deadline() {
    return timer::monotonic_ms() + kWaitMs;
}

bool past(uint64_t when) {
    return timer::monotonic_ms() > when;
}

// ---- memory ---------------------------------------------------------------

uint64_t allocate_table(void*) {
    memory::PageAllocation page = {};
    if (!memory::allocate_page(page)) {
        return 0;
    }
    memset(page.virtual_address, 0, memory::kPageSize);
    ++g_table_pages;
    return page.physical_address;
}

uint64_t* access_table(uint64_t physical, void*) {
    return vm::physical_to_virtual(physical);
}

const TableMemory kTableMemory = {&allocate_table, &access_table, nullptr};

bool allocate_zeroed(uint64_t pages, memory::PageAllocation& allocation) {
    const bool ok = pages == 1 ? memory::allocate_page(allocation)
                               : memory::allocate_contiguous_pages(pages, allocation);
    if (ok) {
        memset(allocation.virtual_address, 0, pages * memory::kPageSize);
    }
    return ok;
}

/* The memory a device may be handed: what the memory map calls RAM, in any
 * of its uses. Reserved ranges, bad memory and the framebuffer are not. */
bool is_memory(boot::MemoryRegionType type) {
    switch (type) {
    case boot::MemoryRegionType::usable:
    case boot::MemoryRegionType::acpi_reclaimable:
    case boot::MemoryRegionType::acpi_nvs:
    case boot::MemoryRegionType::bootloader_reclaimable:
    case boot::MemoryRegionType::executable_and_modules:
        return true;
    default:
        return false;
    }
}

bool build_identity_domain(const Report& report, const boot::BootInfo& boot_info) {
    g_root = allocate_table(nullptr);
    if (g_root == 0) {
        return false;
    }
    uint64_t mapped = 0;
    for (size_t i = 0; i < boot_info.memory_map_entries; ++i) {
        const boot::MemoryRegion& region = boot_info.memory_map[i];
        if (!is_memory(region.type) || region.length == 0) {
            continue;
        }
        const uint64_t base = region.base & ~(kPageSize - 1);
        const uint64_t end = (region.base + region.length + kPageSize - 1) & ~(kPageSize - 1);
        if (!map_identity(kTableMemory, g_root, base, end)) {
            return false;
        }
        mapped += end - base;
    }
    for (size_t i = 0; i < report.reserved_count; ++i) {
        const iommu_tables::ReservedRange& range = report.reserved[i];
        if (!map_identity(kTableMemory, g_root, range.base, range.end)) {
            return false;
        }
        mapped += range.end - range.base;
    }
    console::printf("iommu: identity domain maps %llu MiB in %llu page tables\n", mapped >> 20, g_table_pages);
    return true;
}

// ---- commands -------------------------------------------------------------

bool submit(UnitState& unit, const Command& command) {
    const uint32_t next = static_cast<uint32_t>((unit.command_tail + sizeof(Command)) % kRingBytes);
    const uint64_t until = deadline();
    while ((read32(unit, kCommandHead) & kRingPointerMask) == next) {
        if (past(until)) {
            return false;
        }
        arch::cpu_relax();
    }
    memcpy(reinterpret_cast<uint8_t*>(unit.command_ring) + unit.command_tail, &command, sizeof(command));
    arch::memory_barrier();
    unit.command_tail = next;
    write32(unit, kCommandTail, next);
    return true;
}

// Every command before this one is done when it returns true.
bool wait_for_commands(UnitState& unit) {
    const uint64_t sequence = ++g_completion_sequence;
    if (!submit(unit, completion_wait(g_completion_physical, sequence))) {
        return false;
    }
    const uint64_t until = deadline();
    while (*g_completion != sequence) {
        if (past(until)) {
            return false;
        }
        arch::cpu_relax();
    }
    return true;
}

bool invalidate_devices(UnitState& unit, uint32_t first, uint32_t last) {
    for (uint32_t device = first; device <= last; ++device) {
        if (!submit(unit, invalidate_device_entry(static_cast<uint16_t>(device)))) {
            return false;
        }
    }
    return true;
}

/* Whatever the unit cached under the firmware, or before translation was on.
 * INVALIDATE_IOMMU_ALL covers it where EFR advertises it, but the device
 * table entries of the devices behind the unit are invalidated one by one as
 * well: QEMU moves a device onto its translated address space only when its
 * own entry is invalidated, and on hardware the extra commands cost nothing
 * that matters. A unit that covers every device, or one without IASup, gets
 * all 65536. */
bool flush_caches(UnitState& unit, const Report& report, const Unit& description, uint64_t extended_features) {
    const bool invalidate_all_supported = (extended_features & kEfrInvalidateAll) != 0;
    if (invalidate_all_supported && !submit(unit, invalidate_all())) {
        return false;
    }
    if (description.all_devices || !invalidate_all_supported) {
        if (!invalidate_devices(unit, 0, 0xffff)) {
            return false;
        }
    } else {
        for (uint32_t i = 0; i < description.scope_count; ++i) {
            const iommu_tables::Scope& scope = report.scopes[description.first_scope + i];
            if (!invalidate_devices(unit, scope.first, scope.last) ||
                (scope.aliased && !invalidate_devices(unit, scope.alias, scope.alias))) {
                return false;
            }
        }
    }
    if (!submit(unit, invalidate_domain_pages(kIdentityDomain))) {
        return false;
    }
    return wait_for_commands(unit);
}

// ---- bring-up -------------------------------------------------------------

void turn_off(UnitState& unit) {
    if (unit.registers != nullptr) {
        set_control(unit, 0);
    }
}

bool start_unit(size_t index, const Report& report, uint64_t device_table_physical) {
    const Unit& description = report.units[index];
    UnitState& unit = g_units[index];

    // Whatever the firmware left running stops first: translation, both
    // rings, every interrupt the unit raises.
    const uint64_t inherited = read64(unit, kControl);
    if ((inherited & kControlIommuEnable) != 0) {
        console::printf("iommu: unit %u was left on by the firmware; taking it over\n", static_cast<unsigned>(index));
    }
    set_control(unit, 0);
    const uint64_t until = deadline();
    while ((read32(unit, kStatus) & (kStatusEventLogRunning | kStatusCommandBufferRunning)) != 0) {
        if (past(until)) {
            console::printf("iommu: unit %u: its rings did not stop\n", static_cast<unsigned>(index));
            return false;
        }
        arch::cpu_relax();
    }

    memory::PageAllocation command = {};
    memory::PageAllocation events = {};
    if (!allocate_zeroed(1, command) || !allocate_zeroed(1, events)) {
        console::printf("iommu: unit %u: no memory for its rings\n", static_cast<unsigned>(index));
        return false;
    }
    unit.command_ring = static_cast<uint64_t*>(command.virtual_address);
    unit.command_physical = command.physical_address;
    unit.command_tail = 0;
    unit.event_ring = static_cast<uint32_t*>(events.virtual_address);
    unit.event_physical = events.physical_address;

    write64(unit, kDeviceTableBase, device_table_physical | kDeviceTableSize);
    write64(unit, kCommandBufferBase, unit.command_physical | kRingLengthCode);
    write64(unit, kCommandHead, 0);
    write64(unit, kCommandTail, 0);
    write64(unit, kEventLogBase, unit.event_physical | kRingLengthCode);
    write64(unit, kEventHead, 0);
    write64(unit, kEventTail, 0);
    // An exclusion range would let devices bypass translation; the IVMD
    // ranges that asked for one are in the identity domain instead.
    write64(unit, kExclusionBase, 0);
    write64(unit, kExclusionLimit, 0);
    write32(unit, kStatus, kStatusClearable);

    uint64_t control = kControlCoherent | kControlInvalidationTimeout;
    if ((description.flags & kIvhdHtTunnel) != 0) control |= kControlHtTunnel;
    if ((description.flags & kIvhdPassPostedWrite) != 0) control |= kControlPassPostedWrite;
    if ((description.flags & kIvhdResponsePassPostedWrite) != 0) control |= kControlResponsePassPostedWrite;
    if ((description.flags & kIvhdIsochronous) != 0) control |= kControlIsochronous;
    set_control(unit, control);
    set_control(unit, control | kControlCommandBuffer | kControlEventLog);
    set_control(unit, unit.control | kControlIommuEnable);

    const uint64_t extended_features = description.has_efr ? description.efr : read64(unit, kExtendedFeatures);
    if (!flush_caches(unit, report, description, extended_features)) {
        console::printf("iommu: unit %u: its command buffer did not complete\n", static_cast<unsigned>(index));
        return false;
    }
    return true;
}

void log_event(size_t unit, const Event& event) {
    char device[8];
    iommu_tables::format_requester_id(event.device, device);
    if (event.code == kEventIoPageFault) {
        console::printf("iommu: I/O page fault from %s at 0x%llx, %s, domain %u\n", device, event.address,
            (event.flags & kFaultWrite) != 0 ? "write" : "read", static_cast<unsigned>(event.domain));
        return;
    }
    console::printf("iommu: unit %u: %s from %s at 0x%llx, flags 0x%x\n", static_cast<unsigned>(unit),
        event_name(event.code), device, event.address, static_cast<unsigned>(event.flags));
}

void drain_unit(size_t index) {
    UnitState& unit = g_units[index];
    uint32_t head = read32(unit, kEventHead) & kRingPointerMask;
    const uint32_t tail = read32(unit, kEventTail) & kRingPointerMask;
    unsigned logged = 0;
    unsigned dropped = 0;
    while (head != tail && head < kRingBytes) {
        uint32_t* slot = unit.event_ring + head / sizeof(uint32_t);
        const uint32_t words[4] = {slot[0], slot[1], slot[2], slot[3]};
        const Event event = decode_event(words);
        if (event.code == 0) {
            break; // the tail moved before the entry was written; next drain
        }
        if (event.code == kEventIoPageFault) {
            ++g_fault_count;
            g_last_fault_device = event.device;
            g_last_fault_address = event.address;
        }
        if (logged < kEventsPerDrain) {
            log_event(index, event);
            ++logged;
        } else {
            ++dropped;
        }
        memset(slot, 0, sizeof(Command));
        head = static_cast<uint32_t>((head + sizeof(Command)) % kRingBytes);
    }
    write32(unit, kEventHead, head);
    if (dropped != 0) {
        console::printf("iommu: unit %u: %u more events not shown\n", static_cast<unsigned>(index), dropped);
    }

    if ((read32(unit, kStatus) & kStatusEventOverflow) != 0) {
        console::printf("iommu: unit %u: event log overflowed, events were lost\n", static_cast<unsigned>(index));
        ++g_overflow_count;
        set_control(unit, unit.control & ~kControlEventLog);
        write64(unit, kEventHead, 0);
        write64(unit, kEventTail, 0);
        memset(unit.event_ring, 0, kRingBytes);
        write32(unit, kStatus, kStatusEventOverflow);
        set_control(unit, unit.control | kControlEventLog);
    }
}

} // namespace

bool start(const Report& report, volatile uint8_t* const* registers, const boot::BootInfo& boot_info) {
    if (g_running || report.kind != iommu_tables::Kind::amd_vi || report.unit_count == 0) {
        return false;
    }
    for (size_t i = 0; i < report.unit_count; ++i) {
        if (registers[i] == nullptr) {
            console::printf("iommu: unit %u: its registers are not mapped; staying off\n", static_cast<unsigned>(i));
            return false;
        }
    }

    memory::PageAllocation device_table = {};
    memory::PageAllocation completion = {};
    if (!allocate_zeroed(kDeviceTablePages, device_table) || !allocate_zeroed(1, completion)) {
        console::printf("iommu: no memory for the device table; staying off\n");
        return false;
    }
    g_completion = static_cast<volatile uint64_t*>(completion.virtual_address);
    g_completion_physical = completion.physical_address;

    if (!build_identity_domain(report, boot_info)) {
        console::printf("iommu: no memory for the identity domain; staying off\n");
        return false;
    }
    uint64_t entry[4];
    device_table_entry(g_root, kIdentityDomain, entry);
    auto* entries = static_cast<uint64_t*>(device_table.virtual_address);
    for (uint64_t device = 0; device <= 0xffff; ++device) {
        memcpy(entries + device * 4, entry, sizeof(entry));
    }

    g_unit_count = report.unit_count;
    for (size_t i = 0; i < g_unit_count; ++i) {
        g_units[i] = {};
        g_units[i].registers = registers[i];
    }
    for (size_t i = 0; i < g_unit_count; ++i) {
        if (!start_unit(i, report, device_table.physical_address)) {
            for (size_t j = 0; j < g_unit_count; ++j) {
                turn_off(g_units[j]);
            }
            console::printf("iommu: staying off\n");
            return false;
        }
    }
    g_running = true;
    console::printf("iommu: AMD-Vi translating, %u unit%s, one identity domain\n",
        static_cast<unsigned>(g_unit_count), g_unit_count == 1 ? "" : "s");
    return true;
}

bool running() {
    return g_running;
}

void drain_events() {
    if (!g_running) {
        return;
    }
    for (size_t i = 0; i < g_unit_count; ++i) {
        drain_unit(i);
    }
}

uint64_t fault_count() {
    return g_fault_count;
}

uint64_t overflow_count() {
    return g_overflow_count;
}

bool last_fault(uint16_t& device, uint64_t& address) {
    if (g_fault_count == 0) {
        return false;
    }
    device = g_last_fault_device;
    address = g_last_fault_address;
    return true;
}

bool identity_maps(uint64_t address) {
    uint64_t physical = 0;
    return g_root != 0 && translate(kTableMemory, g_root, address, physical) && physical == address;
}

} // namespace amd_vi
