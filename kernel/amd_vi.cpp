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
constexpr uint32_t kDeviceCount = 65536;
// The first slot a driver can be given; the domain ID is the slot plus one,
// so identity is domain 1 and blocked domain 2.
constexpr int kFirstOwnDomain = 2;

constexpr uint64_t kWaitMs = 1000;
// Events logged per drain; a device faulting in a loop would otherwise fill
// the log faster than the serial port empties it.
constexpr unsigned kEventsPerDrain = 16;

struct UnitState {
    volatile uint8_t* registers;
    uint64_t* command_ring;
    uint64_t command_physical;
    uint32_t command_tail;
    // The tail the unit has been given, and the last head read from it.
    // Commands are queued past the published tail and handed over in one
    // register write; the head is read again only when the ring looks full.
    uint32_t published_tail;
    uint32_t known_head;
    uint32_t* event_ring;
    uint64_t event_physical;
    uint64_t control;
};

struct DomainState {
    bool used;
    uint64_t root;
    FaultHandler handler;
    void* context;
};

// One remapping table per isolation group given interrupts, keyed by the
// ID the unit sees for the group; a page each.
constexpr size_t kMaxInterruptTables = 32;
struct InterruptTable {
    uint16_t alias;
    uint32_t* entries;
    uint64_t physical;
};
// The pass-through bits of a device table entry's third quadword.
constexpr uint64_t kInterruptPassBits = (7ull << 56) | (3ull << 62);
// An entry names a destination in 8 bits, and 255 is the broadcast.
constexpr uint32_t kMaxInterruptDestination = 254;

UnitState g_units[iommu_tables::kMaxUnits];
size_t g_unit_count = 0;
bool g_running = false;
const Report* g_report = nullptr;
DomainState g_domains[kMaxDomains];
uint64_t* g_device_table = nullptr;
// The domain slot each requester ID is in.
uint8_t g_owner[kDeviceCount];
bool g_remapping = false;
uint64_t g_empty_interrupt_table = 0;
InterruptTable g_interrupt_tables[kMaxInterruptTables];
size_t g_interrupt_table_count = 0;
InterruptSources g_interrupt_sources = {};
uint64_t g_refused_interrupts = 0;
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

void release_table(uint64_t physical, void*) {
    memory::free_pages(physical, 1);
    --g_table_pages;
}

const TableMemory kTableMemory = {&allocate_table, &access_table, &release_table, nullptr};

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
    const uint64_t root = allocate_table(nullptr);
    if (root == 0) {
        return false;
    }
    g_domains[kIdentityDomain] = {true, root, nullptr, nullptr};
    uint64_t mapped = 0;
    for (size_t i = 0; i < boot_info.memory_map_entries; ++i) {
        const boot::MemoryRegion& region = boot_info.memory_map[i];
        if (!is_memory(region.type) || region.length == 0) {
            continue;
        }
        const uint64_t base = region.base & ~(kPageSize - 1);
        const uint64_t end = (region.base + region.length + kPageSize - 1) & ~(kPageSize - 1);
        if (!map_identity(kTableMemory, root, base, end)) {
            return false;
        }
        mapped += end - base;
    }
    for (size_t i = 0; i < report.reserved_count; ++i) {
        const iommu_tables::ReservedRange& range = report.reserved[i];
        if (!map_identity(kTableMemory, root, range.base, range.end)) {
            return false;
        }
        mapped += range.end - range.base;
    }
    console::printf("iommu: identity domain maps %llu MiB in %llu page tables\n", mapped >> 20, g_table_pages);
    return true;
}

// ---- commands -------------------------------------------------------------

// Hands the unit every command queued since the last time.
void publish(UnitState& unit) {
    if (unit.published_tail != unit.command_tail) {
        arch::memory_barrier();
        write32(unit, kCommandTail, unit.command_tail);
        unit.published_tail = unit.command_tail;
    }
}

/* Writes a command into the ring without telling the unit. Two register
 * accesses per command made a long run of invalidations cost about 5 us
 * each (330 ms for the 65536 requester IDs of a Ryzen's IVHD); queued, a
 * run costs one tail write per call and one head read per ring's worth. */
bool queue(UnitState& unit, const Command& command) {
    const uint32_t next = static_cast<uint32_t>((unit.command_tail + sizeof(Command)) % kRingBytes);
    if (next == unit.known_head) {
        unit.known_head = read32(unit, kCommandHead) & kRingPointerMask;
        if (next == unit.known_head) {
            publish(unit);
            const uint64_t until = deadline();
            while (next == unit.known_head) {
                if (past(until)) {
                    return false;
                }
                arch::cpu_relax();
                unit.known_head = read32(unit, kCommandHead) & kRingPointerMask;
            }
        }
    }
    memcpy(reinterpret_cast<uint8_t*>(unit.command_ring) + unit.command_tail, &command, sizeof(command));
    unit.command_tail = next;
    return true;
}

bool submit(UnitState& unit, const Command& command) {
    if (!queue(unit, command)) {
        return false;
    }
    publish(unit);
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

// `interrupt_tables` adds INVALIDATE_INTERRUPT_TABLE for each device too.
bool invalidate_devices(UnitState& unit, uint32_t first, uint32_t last, bool interrupt_tables) {
    for (uint32_t device = first; device <= last; ++device) {
        if (!queue(unit, invalidate_device_entry(static_cast<uint16_t>(device))) ||
            (interrupt_tables && !queue(unit, invalidate_interrupt_table(static_cast<uint16_t>(device))))) {
            return false;
        }
    }
    publish(unit);
    return true;
}

bool invalidate_devices(UnitState& unit, uint32_t first, uint32_t last) {
    return invalidate_devices(unit, first, last, g_remapping);
}

/* Whatever the unit cached under the firmware, or before translation was on.
 * INVALIDATE_IOMMU_ALL covers it where EFR advertises it, interrupt tables
 * included, but the device table entries of the devices behind the unit are
 * invalidated one by one as well: QEMU moves a device onto its translated
 * address space only when its own entry is invalidated (its INVALIDATE_ALL
 * does not). Queued, that costs a few milliseconds on hardware. A unit that
 * covers every device, or one without IASup, gets all 65536, and without
 * IASup their interrupt tables too. */
bool flush_caches(UnitState& unit, const Report& report, const Unit& description, uint64_t extended_features) {
    const bool invalidate_all_supported = (extended_features & kEfrInvalidateAll) != 0;
    if (invalidate_all_supported && !submit(unit, invalidate_all())) {
        return false;
    }
    const bool interrupt_tables = g_remapping && !invalidate_all_supported;
    if (description.all_devices || !invalidate_all_supported) {
        if (!invalidate_devices(unit, 0, 0xffff, interrupt_tables)) {
            return false;
        }
    } else {
        for (uint32_t i = 0; i < description.scope_count; ++i) {
            const iommu_tables::Scope& scope = report.scopes[description.first_scope + i];
            if (!invalidate_devices(unit, scope.first, scope.last, interrupt_tables) ||
                (scope.aliased && !invalidate_devices(unit, scope.alias, scope.alias, interrupt_tables))) {
                return false;
            }
        }
    }
    if (!submit(unit, invalidate_domain_pages(domain_id(kIdentityDomain)))) {
        return false;
    }
    return wait_for_commands(unit);
}

// ---- domains --------------------------------------------------------------

bool own_domain(int domain) {
    return domain >= kFirstOwnDomain && static_cast<size_t>(domain) < kMaxDomains && g_domains[domain].used;
}

void write_entry(uint32_t device, int domain) {
    uint64_t entry[4];
    device_table_entry(g_domains[domain].root, domain_id(domain), entry);
    uint64_t* slot = g_device_table + device * 4;
    // The root and domain ID in the first two quadwords are all that differ
    // between domains; the interrupt half after them is left as it is. The
    // invalidation that follows makes the unit reread the entry.
    slot[1] = entry[1];
    slot[0] = entry[0];
    g_owner[device] = static_cast<uint8_t>(domain);
}

/* After device table entries changed: every unit rereads them, and drops
 * what it cached for the domains the devices were in and are in now. */
bool flush_devices(const iommu_tables::Group& group, const bool (&domains)[kMaxDomains]) {
    for (size_t index = 0; index < g_unit_count; ++index) {
        UnitState& unit = g_units[index];
        for (size_t i = 0; i < group.range_count; ++i) {
            if (!invalidate_devices(unit, group.ranges[i].first, group.ranges[i].last)) {
                return false;
            }
        }
        for (size_t domain = 0; domain < kMaxDomains; ++domain) {
            if (domains[domain] && !submit(unit, invalidate_domain_pages(domain_id(static_cast<int>(domain))))) {
                return false;
            }
        }
        if (!wait_for_commands(unit)) {
            return false;
        }
    }
    return true;
}

bool flush_domain(int domain) {
    for (size_t index = 0; index < g_unit_count; ++index) {
        if (!submit(g_units[index], invalidate_domain_pages(domain_id(domain))) || !wait_for_commands(g_units[index])) {
            return false;
        }
    }
    return true;
}

bool move_group(const iommu_tables::Group& group, int domain) {
    bool domains[kMaxDomains] = {};
    domains[domain] = true;
    for (size_t i = 0; i < group.range_count; ++i) {
        for (uint32_t device = group.ranges[i].first; device <= group.ranges[i].last; ++device) {
            domains[g_owner[device]] = true;
            write_entry(device, domain);
        }
    }
    if (!flush_devices(group, domains)) {
        console::printf("iommu: a unit did not complete the invalidation\n");
        return false;
    }
    return true;
}

uint32_t group_size(const iommu_tables::Group& group) {
    uint32_t size = 0;
    for (size_t i = 0; i < group.range_count; ++i) {
        size += static_cast<uint32_t>(group.ranges[i].last - group.ranges[i].first) + 1;
    }
    return size;
}

/* I/O page faults read in one drain, handed to the domains' owners once the
 * whole drain is logged: an owner that blocks its device must not change the
 * domain the rest of the drain is logged with. One ring's worth. */
struct PendingFault {
    uint16_t device;
    uint8_t domain;
    bool write;
    uint64_t address;
};

PendingFault g_pending[kRingBytes / sizeof(Command)];
size_t g_pending_count = 0;

/* A fault goes to the owner of the domain the device was in when it was
 * read, if the device is still there: one already moved to the blocked
 * domain reaches nobody. */
void deliver_faults() {
    const size_t count = g_pending_count;
    g_pending_count = 0;
    for (size_t i = 0; i < count; ++i) {
        const PendingFault& fault = g_pending[i];
        const int domain = fault.domain;
        if (g_owner[fault.device] != domain || !own_domain(domain) || g_domains[domain].handler == nullptr) {
            continue;
        }
        g_domains[domain].handler(g_domains[domain].context, fault.device, fault.address, fault.write);
    }
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
    unit.published_tail = 0;
    unit.known_head = 0;
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
    if (is_refused_interrupt(event)) {
        console::printf("iommu: interrupt from %s refused, address 0x%llx\n", device, event.address);
        return;
    }
    if (event.code == kEventIoPageFault) {
        // The domain the kernel put the device in: QEMU leaves the event's
        // own field zero.
        console::printf("iommu: I/O page fault from %s at 0x%llx, %s, domain %u\n", device, event.address,
            (event.flags & kFaultWrite) != 0 ? "write" : "read", static_cast<unsigned>(domain_id(g_owner[event.device])));
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
        if (is_refused_interrupt(event)) {
            ++g_refused_interrupts;
        } else if (event.code == kEventIoPageFault) {
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
        if (event.code == kEventIoPageFault && !is_refused_interrupt(event) && g_pending_count < sizeof(g_pending) / sizeof(g_pending[0])) {
            g_pending[g_pending_count++] = {event.device, g_owner[event.device], (event.flags & kFaultWrite) != 0,
                event.address};
        }
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

// ---- interrupt remapping --------------------------------------------------

void set_interrupt_half(uint32_t device, uint64_t table, uint8_t dte_setting) {
    g_device_table[device * 4 + 2] = interrupt_remapping(table, dte_setting);
}

// Every device gets the empty table, with the pass-through bits the IVRS
// asks for it. Before any unit runs.
// A zeroed page for a remapping table: not counted with the page tables.
uint64_t allocate_interrupt_table() {
    memory::PageAllocation page = {};
    return allocate_zeroed(1, page) ? page.physical_address : 0;
}

bool prepare_remapping(const Report& report) {
    g_empty_interrupt_table = allocate_interrupt_table();
    if (g_empty_interrupt_table == 0) {
        return false;
    }
    for (uint32_t device = 0; device < kDeviceCount; ++device) {
        set_interrupt_half(device, g_empty_interrupt_table, 0);
    }
    for (size_t i = 0; i < report.scope_count; ++i) {
        const iommu_tables::Scope& scope = report.scopes[i];
        if (scope.unit == iommu_tables::kNoUnit || scope.dte_setting == 0 ||
            scope.kind == iommu_tables::ScopeKind::acpi_device) {
            continue;
        }
        for (uint32_t device = scope.first; device <= scope.last; ++device) {
            set_interrupt_half(device, g_empty_interrupt_table, scope.dte_setting);
        }
        if (scope.aliased) {
            set_interrupt_half(scope.alias, g_empty_interrupt_table, scope.dte_setting);
        }
    }
    g_remapping = true;
    return true;
}

bool flush_interrupt_tables(const iommu_tables::Group& group, bool entries_changed) {
    if (!g_running) {
        return true; // the units read everything fresh when they start
    }
    for (size_t index = 0; index < g_unit_count; ++index) {
        UnitState& unit = g_units[index];
        for (size_t i = 0; i < group.range_count; ++i) {
            for (uint32_t device = group.ranges[i].first; device <= group.ranges[i].last; ++device) {
                if ((entries_changed && !submit(unit, invalidate_device_entry(static_cast<uint16_t>(device)))) ||
                    !submit(unit, invalidate_interrupt_table(static_cast<uint16_t>(device)))) {
                    return false;
                }
            }
        }
        if (!wait_for_commands(unit)) {
            return false;
        }
    }
    return true;
}

/* The group's own table, made on first use: every member's entry then
 * points at it, keeping its pass-through bits. */
InterruptTable* interrupt_table_for(uint16_t requester, iommu_tables::Group& group) {
    if (!iommu_tables::isolation_group(*g_report, requester, group)) {
        return nullptr;
    }
    for (size_t i = 0; i < g_interrupt_table_count; ++i) {
        if (g_interrupt_tables[i].alias == group.alias) {
            return &g_interrupt_tables[i];
        }
    }
    if (g_interrupt_table_count == kMaxInterruptTables) {
        console::printf("iommu: every interrupt remapping table is in use\n");
        return nullptr;
    }
    const uint64_t physical = allocate_interrupt_table();
    if (physical == 0) {
        return nullptr;
    }
    InterruptTable& table = g_interrupt_tables[g_interrupt_table_count++];
    table = {group.alias, reinterpret_cast<uint32_t*>(vm::physical_to_virtual(physical)), physical};
    for (size_t i = 0; i < group.range_count; ++i) {
        for (uint32_t device = group.ranges[i].first; device <= group.ranges[i].last; ++device) {
            uint64_t& half = g_device_table[device * 4 + 2];
            half = interrupt_remapping(physical, 0) | (half & kInterruptPassBits);
        }
    }
    if (!flush_interrupt_tables(group, true)) {
        console::printf("iommu: a unit did not complete the invalidation\n");
        return nullptr;
    }
    return &table;
}

} // namespace

bool start(const Report& report, volatile uint8_t* const* registers, const boot::BootInfo& boot_info,
    const InterruptSources* interrupts) {
    if (g_running || report.kind != iommu_tables::Kind::amd_vi || report.unit_count == 0) {
        return false;
    }
    for (size_t i = 0; i < report.unit_count; ++i) {
        if (registers[i] == nullptr) {
            console::printf("iommu: unit %u: its registers are not mapped; staying off\n", static_cast<unsigned>(i));
            return false;
        }
    }

    g_report = &report;
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
    const uint64_t blocked_root = allocate_table(nullptr);
    if (blocked_root == 0) {
        console::printf("iommu: no memory for the blocked domain; staying off\n");
        return false;
    }
    g_domains[kBlockedDomain] = {true, blocked_root, nullptr, nullptr};
    g_device_table = static_cast<uint64_t*>(device_table.virtual_address);
    for (uint32_t device = 0; device < kDeviceCount; ++device) {
        write_entry(device, kIdentityDomain);
    }
    if (interrupts != nullptr) {
        if (!prepare_remapping(report)) {
            console::printf("iommu: no memory for interrupt remapping; staying off\n");
            return false;
        }
        g_interrupt_sources = *interrupts;
        for (size_t i = 0; i < interrupts->ioapic_count; ++i) {
            iommu_tables::Group group = {};
            if (interrupt_table_for(interrupts->ioapic_requester[i], group) == nullptr) {
                console::printf("iommu: no remapping table for I/O APIC %u; staying off\n",
                    static_cast<unsigned>(interrupts->ioapic_id[i]));
                return false;
            }
        }
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
            g_remapping = false;
            console::printf("iommu: staying off\n");
            return false;
        }
    }
    g_running = true;
    console::printf("iommu: AMD-Vi translating, %u unit%s, one identity domain\n",
        static_cast<unsigned>(g_unit_count), g_unit_count == 1 ? "" : "s");
    if (g_remapping) {
        console::printf("iommu: interrupts remapped; a device raises only the vectors it was given\n");
    }
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
        deliver_faults();
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
    const uint64_t root = g_domains[kIdentityDomain].root;
    return root != 0 && translate(kTableMemory, root, address, physical) && physical == address;
}

int create_domain(FaultHandler handler, void* context) {
    if (!g_running) {
        return -1;
    }
    for (size_t domain = kFirstOwnDomain; domain < kMaxDomains; ++domain) {
        if (g_domains[domain].used) {
            continue;
        }
        const uint64_t root = allocate_table(nullptr);
        if (root == 0) {
            console::printf("iommu: no memory for a new domain\n");
            return -1;
        }
        g_domains[domain] = {true, root, handler, context};
        return static_cast<int>(domain);
    }
    console::printf("iommu: every domain is in use\n");
    return -1;
}

void destroy_domain(int domain) {
    if (!own_domain(domain)) {
        return;
    }
    unsigned moved = 0;
    for (uint32_t device = 0; device < kDeviceCount; ++device) {
        if (g_owner[device] != domain) {
            continue;
        }
        write_entry(device, kBlockedDomain);
        iommu_tables::Group group = {};
        group.alias = static_cast<uint16_t>(device);
        group.range_count = 1;
        group.ranges[0] = {static_cast<uint16_t>(device), static_cast<uint16_t>(device)};
        bool domains[kMaxDomains] = {};
        domains[domain] = true;
        if (!flush_devices(group, domains)) {
            // The tables stay, unused, rather than freed under a unit that
            // may still walk them.
            console::printf("iommu: a unit did not complete the invalidation; domain %u leaks its tables\n",
                static_cast<unsigned>(domain_id(domain)));
            g_domains[domain].handler = nullptr;
            return;
        }
        ++moved;
    }
    if (!flush_domain(domain)) {
        console::printf("iommu: a unit did not complete the invalidation; domain %u leaks its tables\n",
            static_cast<unsigned>(domain_id(domain)));
        g_domains[domain].handler = nullptr;
        return;
    }
    free_tables(kTableMemory, g_domains[domain].root);
    g_domains[domain] = {};
    if (moved != 0) {
        console::printf("iommu: domain %u destroyed; %u device%s moved to the blocked domain\n",
            static_cast<unsigned>(domain_id(domain)), moved, moved == 1 ? "" : "s");
    }
}

bool attach(int domain, uint16_t requester) {
    char name[8];
    iommu_tables::format_requester_id(requester, name);
    if (!own_domain(domain)) {
        return false;
    }
    iommu_tables::Group group = {};
    if (!iommu_tables::isolation_group(*g_report, requester, group)) {
        console::printf("iommu: %s: its isolation group has too many ranges; not attached\n", name);
        return false;
    }
    for (size_t i = 0; i < group.range_count; ++i) {
        for (uint32_t device = group.ranges[i].first; device <= group.ranges[i].last; ++device) {
            const int owner = g_owner[device];
            if (owner >= kFirstOwnDomain && owner != domain) {
                char other[8];
                iommu_tables::format_requester_id(static_cast<uint16_t>(device), other);
                console::printf("iommu: %s: %s, in its isolation group, belongs to domain %u; not attached\n",
                    name, other, static_cast<unsigned>(domain_id(owner)));
                return false;
            }
        }
    }
    if (!move_group(group, domain)) {
        return false;
    }
    const uint32_t size = group_size(group);
    if (size > 1) {
        console::printf("iommu: %s attached to domain %u with its isolation group, %u requester IDs\n", name,
            static_cast<unsigned>(domain_id(domain)), size);
    } else {
        console::printf("iommu: %s attached to domain %u\n", name, static_cast<unsigned>(domain_id(domain)));
    }
    return true;
}

void block(uint16_t requester) {
    if (!g_running) {
        return;
    }
    char name[8];
    iommu_tables::format_requester_id(requester, name);
    iommu_tables::Group group = {};
    if (!iommu_tables::isolation_group(*g_report, requester, group)) {
        // At least the function and the ID the unit sees for it.
        group.range_count = 0;
        group.ranges[group.range_count++] = {requester, requester};
        if (group.alias != requester) {
            group.ranges[group.range_count++] = {group.alias, group.alias};
        }
    }
    move_group(group, kBlockedDomain);
    console::printf("iommu: %s moved to the blocked domain\n", name);
}

int domain_of(uint16_t requester) {
    return g_owner[requester];
}

uint16_t domain_id(int domain) {
    return static_cast<uint16_t>(domain + 1);
}

bool map(int domain, uint64_t device_address, uint64_t physical, uint64_t bytes, uint64_t access) {
    if (!own_domain(domain) ||
        !map_pages(kTableMemory, g_domains[domain].root, device_address, physical, bytes, access)) {
        return false;
    }
    // A unit may cache what it found not present (NpCache).
    return flush_domain(domain);
}

bool unmap(int domain, uint64_t device_address, uint64_t bytes) {
    if (!own_domain(domain)) {
        return false;
    }
    unmap_pages(kTableMemory, g_domains[domain].root, device_address, bytes);
    return flush_domain(domain);
}

uint64_t table_pages() {
    return g_table_pages;
}

bool remapping_interrupts() {
    return g_running && g_remapping;
}

bool allow_interrupt(uint16_t requester, uint8_t vector, uint32_t apic_id) {
    if (!remapping_interrupts()) {
        return true;
    }
    char name[8];
    iommu_tables::format_requester_id(requester, name);
    if (apic_id > kMaxInterruptDestination) {
        console::printf("iommu: %s: local APIC %u cannot be named by a remapping entry\n", name,
            static_cast<unsigned>(apic_id));
        return false;
    }
    iommu_tables::Group group = {};
    InterruptTable* table = interrupt_table_for(requester, group);
    if (table == nullptr) {
        console::printf("iommu: %s: no remapping table; vector %u not allowed\n", name, static_cast<unsigned>(vector));
        return false;
    }
    table->entries[vector] = interrupt_table_entry(vector, static_cast<uint8_t>(apic_id));
    arch::memory_barrier();
    if (!flush_interrupt_tables(group, false)) {
        console::printf("iommu: a unit did not complete the invalidation\n");
        return false;
    }
    return true;
}

bool allow_ioapic_interrupt(uint8_t ioapic_id, uint8_t vector, uint32_t apic_id) {
    if (!remapping_interrupts()) {
        return true;
    }
    for (size_t i = 0; i < g_interrupt_sources.ioapic_count; ++i) {
        if (g_interrupt_sources.ioapic_id[i] == ioapic_id) {
            return allow_interrupt(g_interrupt_sources.ioapic_requester[i], vector, apic_id);
        }
    }
    console::printf("iommu: I/O APIC %u is not one the units remap\n", static_cast<unsigned>(ioapic_id));
    return false;
}

uint64_t refused_interrupt_count() {
    return g_refused_interrupts;
}

} // namespace amd_vi
