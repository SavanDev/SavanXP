#include "kernel/iommu.hpp"

#include "kernel/acpi.hpp"
#include "kernel/acpi_tables.hpp"
#include "kernel/amd_vi.hpp"
#include "kernel/amd_vi_tables.hpp"
#include "kernel/console.hpp"
#include "kernel/pci.hpp"
#include "kernel/vmm.hpp"

namespace iommu {

namespace {

using iommu_tables::Kind;
using iommu_tables::Report;
using iommu_tables::Unit;

// AMD-Vi registers (48882, "MMIO Registers").
constexpr uint32_t kAmdControl = 0x18;
constexpr uint64_t kAmdControlIommuEnable = 1ull << 0;
// VT-d registers (chapter 11).
constexpr uint32_t kVtdExtendedCapability = 0x10;
constexpr uint64_t kVtdEcapInterruptRemapping = 1ull << 3;
constexpr uint32_t kVtdGlobalStatus = 0x1c;
constexpr uint32_t kVtdStatusTranslation = 1u << 31;
constexpr uint32_t kVtdStatusRemapping = 1u << 25;

// The AMD-Vi capability block in the IOMMU's PCI configuration space
// (48882, "IOMMU Capability Block"): a Secure Device capability of type 3,
// whose base address low register at +4 holds the enable bit and bits 31:14.
constexpr uint8_t kPciCapabilitySecureDevice = 0x0f;
constexpr uint32_t kAmdCapabilityTypeIommu = 3;
constexpr uint32_t kAmdBaseEnable = 1u << 0;
constexpr uint32_t kAmdBaseLowMask = 0xffffc000u;
// Where capabilities may sit in the legacy 256-byte configuration space.
constexpr uint16_t kPciCapabilityFirst = 0x40;
constexpr uint16_t kPciCapabilityLast = 0xf0;

constexpr uint64_t kPageSize = 4096;
// What is mapped of each unit's registers: AMD-Vi's go up to the status
// register at 0x2020; VT-d's first page holds every one read here.
constexpr uint64_t kAmdRegisterWindow = 16 * 1024;
// How often the BSP's tick drains the event logs.
constexpr uint64_t kDrainIntervalMs = 100;

Report g_report;
bool g_present = false;
volatile uint8_t* g_registers[iommu_tables::kMaxUnits] = {};
// Every window mapped, answering or not, so a second look reuses it.
void* g_windows[iommu_tables::kMaxUnits] = {};
uint64_t g_next_drain_ms = 0;

/* M4: the I/O APICs the MADT lists, each matched with the IVRS entry that
 * names it. Null, with the reason logged, when interrupts cannot all be
 * remapped: an I/O APIC the IVRS does not name would have its interrupts
 * looked up under some other device's entry. */
const amd_vi::InterruptSources* interrupt_sources(const boot::BootInfo& boot_info,
    amd_vi::InterruptSources& sources) {
    if (boot::has_option(boot_info.cmdline, "nointremap")) {
        console::printf("iommu: interrupt remapping left off by the nointremap boot option\n");
        return nullptr;
    }
    if (!g_report.remapping_available) {
        console::printf("iommu: interrupt remapping stays off: the IVRS names no I/O APIC\n");
        return nullptr;
    }
    const acpi_tables::SdtHeader* madt = acpi::find_table("APIC");
    if (madt == nullptr) {
        console::printf("iommu: interrupt remapping stays off: no MADT\n");
        return nullptr;
    }
    sources = {};
    constexpr uint8_t kMadtIoApic = 1;
    constexpr uint8_t kMadtIoApicLength = 12;
    for (const acpi_tables::MadtEntryHeader* entry = acpi_tables::madt_next_entry(*madt, nullptr); entry != nullptr;
         entry = acpi_tables::madt_next_entry(*madt, entry)) {
        if (entry->type != kMadtIoApic || entry->length < kMadtIoApicLength) {
            continue;
        }
        const uint8_t id = reinterpret_cast<const uint8_t*>(entry)[2];
        bool named = false;
        for (size_t i = 0; i < g_report.scope_count && !named; ++i) {
            const iommu_tables::Scope& scope = g_report.scopes[i];
            if (scope.kind == iommu_tables::ScopeKind::ioapic && scope.unit != iommu_tables::kNoUnit &&
                scope.handle == id) {
                if (sources.ioapic_count == amd_vi::kMaxIoApics) {
                    console::printf("iommu: interrupt remapping stays off: more than %u I/O APICs\n",
                        static_cast<unsigned>(amd_vi::kMaxIoApics));
                    return nullptr;
                }
                sources.ioapic_id[sources.ioapic_count] = id;
                sources.ioapic_requester[sources.ioapic_count] = scope.first;
                ++sources.ioapic_count;
                named = true;
            }
        }
        if (!named) {
            console::printf("iommu: interrupt remapping stays off: I/O APIC %u is not in the IVRS\n",
                static_cast<unsigned>(id));
            return nullptr;
        }
    }
    for (size_t i = 0; i < sources.ioapic_count; ++i) {
        char device[8];
        iommu_tables::format_requester_id(sources.ioapic_requester[i], device);
        console::printf("iommu: I/O APIC %u reaches the unit as %s\n", static_cast<unsigned>(sources.ioapic_id[i]),
            device);
    }
    return &sources;
}

const char* kind_name(Kind kind) {
    return kind == Kind::amd_vi ? "AMD-Vi" : "VT-d";
}

/* The register base came from firmware. Before it is mapped and read it must
 * not lie in memory the kernel uses as memory: a read of RAM is harmless, but
 * a base that points at it is a table that lies, and nothing read there would
 * mean anything. Reserved ranges are where device registers live. */
bool window_outside_memory(const boot::BootInfo& boot_info, uint64_t base, uint64_t size) {
    const uint64_t end = base + size;
    for (size_t i = 0; i < boot_info.memory_map_entries; ++i) {
        const boot::MemoryRegion& region = boot_info.memory_map[i];
        if (region.type == boot::MemoryRegionType::reserved ||
            region.type == boot::MemoryRegionType::reserved_mapped ||
            region.type == boot::MemoryRegionType::bad_memory) {
            continue;
        }
        const uint64_t region_end = region.base + region.length;
        if (base < region_end && region.base < end) {
            return false;
        }
    }
    return true;
}

uint32_t read32(const volatile uint8_t* registers, uint32_t offset) {
    return *reinterpret_cast<const volatile uint32_t*>(registers + offset);
}

// Two 32-bit halves, low first: the order both specifications allow.
uint64_t read64(const volatile uint8_t* registers, uint32_t offset) {
    const uint64_t low = read32(registers, offset);
    const uint64_t high = read32(registers, offset + 4);
    return low | (high << 32);
}

// Reads, never writes: discovery leaves every unit exactly as firmware left
// it. The window stays mapped for start().
void read_unit_state(const boot::BootInfo& boot_info, Kind kind, size_t index, Unit& unit) {
    const uint64_t window = kind == Kind::amd_vi ? kAmdRegisterWindow : kPageSize;
    if (!window_outside_memory(boot_info, unit.register_base, window)) {
        return;
    }
    void* mapping = g_windows[index];
    if (mapping == nullptr) {
        if (!vm::map_kernel_mmio(unit.register_base, window, vm::kPagePresent, &mapping)) {
            return;
        }
        g_windows[index] = mapping;
    }
    const auto* registers = static_cast<const volatile uint8_t*>(mapping);
    if (kind == Kind::amd_vi) {
        const uint64_t control = read64(registers, kAmdControl);
        if (control == ~0ull) {
            return; // nothing answers there
        }
        unit.registers_read = true;
        g_registers[index] = static_cast<volatile uint8_t*>(mapping);
        // Interrupt remapping is part of every AMD-Vi unit; what it needs
        // from the table is an I/O APIC in some IVHD (remapping_available).
        unit.remapping_supported = true;
        unit.translation_enabled = (control & kAmdControlIommuEnable) != 0;
    } else {
        const uint64_t extended = read64(registers, kVtdExtendedCapability);
        const uint32_t status = read32(registers, kVtdGlobalStatus);
        if (extended == ~0ull || status == ~0u) {
            return;
        }
        unit.registers_read = true;
        g_registers[index] = static_cast<volatile uint8_t*>(mapping);
        unit.remapping_supported = (extended & kVtdEcapInterruptRemapping) != 0;
        unit.translation_enabled = (status & kVtdStatusTranslation) != 0;
        unit.remapping_enabled = (status & kVtdStatusRemapping) != 0;
    }
}

/* An AMD-Vi unit's registers answer only once its PCI capability's base
 * address register holds the enable bit. Firmware sets it on every board this
 * was written for; VirtualBox's EFI does not, and there the window reads as
 * all ones. With the enable bit clear the base is still writable, so it is
 * written with the address the IVRS gives and turned on. A bit already set,
 * a base that names another address, or a capability that is not an IOMMU's
 * leaves the unit alone. Only the iommu boot option gets here: discovery
 * never writes. */
bool turn_on_register_window(const boot::BootInfo& boot_info, size_t index, Unit& unit) {
    if (unit.segment != 0 || unit.capability_offset < kPciCapabilityFirst ||
        unit.capability_offset > kPciCapabilityLast ||
        !window_outside_memory(boot_info, unit.register_base, kAmdRegisterWindow)) {
        return false;
    }
    const uint8_t bus = static_cast<uint8_t>(unit.device_id >> 8);
    const uint8_t slot = static_cast<uint8_t>((unit.device_id >> 3) & 0x1fu);
    const uint8_t function = static_cast<uint8_t>(unit.device_id & 0x7u);
    const auto offset = static_cast<uint8_t>(unit.capability_offset);
    const uint32_t header = pci::read_config_u32(bus, slot, function, offset);
    if ((header & 0xffu) != kPciCapabilitySecureDevice ||
        ((header >> 16) & 0x7u) != kAmdCapabilityTypeIommu) {
        return false;
    }
    const uint32_t low = pci::read_config_u32(bus, slot, function, static_cast<uint8_t>(offset + 4));
    const uint32_t high = pci::read_config_u32(bus, slot, function, static_cast<uint8_t>(offset + 8));
    if ((low & kAmdBaseEnable) != 0) {
        return false;
    }
    const uint64_t current = (static_cast<uint64_t>(high) << 32) | (low & kAmdBaseLowMask);
    if (current != 0 && current != unit.register_base) {
        return false;
    }
    pci::write_config_u32(bus, slot, function, static_cast<uint8_t>(offset + 8),
        static_cast<uint32_t>(unit.register_base >> 32));
    pci::write_config_u32(bus, slot, function, static_cast<uint8_t>(offset + 4),
        (static_cast<uint32_t>(unit.register_base) & kAmdBaseLowMask) | kAmdBaseEnable);
    read_unit_state(boot_info, Kind::amd_vi, index, unit);
    console::printf("iommu: unit %u: firmware left its register window off; turned on at 0x%llx, %s\n",
        static_cast<unsigned>(index), unit.register_base,
        unit.registers_read ? "registers readable" : "still not answering");
    return unit.registers_read;
}

void log_unit(size_t index, const Unit& unit) {
    console::printf("iommu: unit %u at 0x%llx, segment %u, ",
        static_cast<unsigned>(index), unit.register_base, static_cast<unsigned>(unit.segment));
    if (g_report.kind == Kind::amd_vi) {
        char device[8];
        iommu_tables::format_requester_id(unit.device_id, device);
        console::printf("device %s, IVHD 0x%x, ", device, static_cast<unsigned>(unit.source_type));
    }
    console::printf("%s%u device entries, ", unit.all_devices ? "all devices, " : "",
        static_cast<unsigned>(unit.scope_count));
    if (!unit.registers_read) {
        console::printf("registers not readable\n");
        return;
    }
    console::printf("translation %s", unit.translation_enabled ? "left on by the firmware" : "off");
    if (g_report.kind == Kind::vt_d) {
        console::printf(", interrupt remapping %s",
            !unit.remapping_supported ? "not supported"
            : unit.remapping_enabled ? "left on by the firmware" : "off");
    }
    console::printf("\n");
}

bool try_table(const boot::BootInfo& boot_info, const char (&signature)[5], Kind kind) {
    const acpi_tables::SdtHeader* table = acpi::find_table(signature);
    if (table == nullptr) {
        return false;
    }
    const char* refusal = kind == Kind::amd_vi
        ? iommu_tables::parse_ivrs(*table, g_report)
        : iommu_tables::parse_dmar(*table, g_report);
    if (refusal != nullptr) {
        console::printf("iommu: %s refused: %s\n", signature, refusal);
        return false;
    }

    for (size_t i = 0; i < g_report.unit_count; ++i) {
        Unit& unit = g_report.units[i];
        read_unit_state(boot_info, kind, i, unit);
        // VT-d remapping needs every unit to support it, not just the table
        // to say the platform does.
        if (kind == Kind::vt_d && !(unit.registers_read && unit.remapping_supported)) {
            g_report.remapping_available = false;
        }
    }
    return true;
}

} // namespace

void discover(const boot::BootInfo& boot_info) {
    g_present = false;
    bool found = try_table(boot_info, "IVRS", Kind::amd_vi);
    if (found) {
        if (acpi::find_table("DMAR") != nullptr) {
            console::printf("iommu: a DMAR is present too; using the IVRS\n");
        }
    } else {
        found = try_table(boot_info, "DMAR", Kind::vt_d);
    }
    if (!found) {
        console::printf("iommu: none\n");
        return;
    }
    g_present = true;

    console::printf("iommu: %s, %u unit%s, interrupt remapping %s, address width %u\n",
        kind_name(g_report.kind), static_cast<unsigned>(g_report.unit_count),
        g_report.unit_count == 1 ? "" : "s",
        g_report.remapping_available ? "available" : "unavailable",
        static_cast<unsigned>(g_report.address_width));
    for (size_t i = 0; i < g_report.unit_count; ++i) {
        log_unit(i, g_report.units[i]);
    }
    for (size_t i = 0; i < g_report.reserved_count; ++i) {
        const iommu_tables::ReservedRange& range = g_report.reserved[i];
        console::printf("iommu: reserved 0x%llx-0x%llx for %s\n", range.base, range.end - 1,
            range.all_devices ? "all devices" : "listed devices");
    }
}

const iommu_tables::Report* report() {
    return g_present ? &g_report : nullptr;
}

void start(const boot::BootInfo& boot_info) {
    if (!g_present) {
        return;
    }
    const bool asked = boot::has_option(boot_info.cmdline, "iommu");
    if (boot::has_option(boot_info.cmdline, "noiommu")) {
        console::printf("iommu: left off by the noiommu boot option\n");
        return;
    }
    if (!asked) {
        console::printf("iommu: translation stays off; boot with the iommu option to turn it on\n");
        return;
    }
    if (g_report.kind != Kind::amd_vi) {
        console::printf("iommu: VT-d is not driven yet; translation stays off\n");
        return;
    }
    for (size_t i = 0; i < g_report.unit_count; ++i) {
        if (!g_report.units[i].registers_read) {
            (void)turn_on_register_window(boot_info, i, g_report.units[i]);
        }
    }
    amd_vi::InterruptSources sources = {};
    amd_vi::start(g_report, g_registers, boot_info, interrupt_sources(boot_info, sources));
}

bool translating() {
    return amd_vi::running();
}

bool create_domain(FaultHandler handler, void* context, Domain& domain) {
    domain.index = amd_vi::create_domain(handler, context);
    return domain.valid();
}

void destroy_domain(Domain& domain) {
    if (domain.valid()) {
        amd_vi::destroy_domain(domain.index);
    }
    domain.index = -1;
}

uint16_t requester_id(const pci::DeviceInfo& device) {
    return static_cast<uint16_t>((device.bus << 8) | ((device.slot & 0x1f) << 3) | (device.function & 0x7));
}

bool attach(const Domain& domain, const pci::DeviceInfo& device) {
    return domain.valid() && amd_vi::attach(domain.index, requester_id(device));
}

void block(const pci::DeviceInfo& device) {
    amd_vi::block(requester_id(device));
}

namespace {

uint64_t unit_access(unsigned access) {
    return ((access & kDeviceReads) != 0 ? amd_vi::kReadable : 0) |
        ((access & kDeviceWrites) != 0 ? amd_vi::kWritable : 0);
}

} // namespace

bool map(const Domain& domain, uint64_t device_address, uint64_t physical, uint64_t bytes, unsigned access) {
    return domain.valid() && amd_vi::map(domain.index, device_address, physical, bytes, unit_access(access));
}

bool unmap(const Domain& domain, uint64_t device_address, uint64_t bytes) {
    return domain.valid() && amd_vi::unmap(domain.index, device_address, bytes);
}

bool remapping_interrupts() {
    return amd_vi::remapping_interrupts();
}

bool allow_interrupt(const pci::DeviceInfo& device, uint8_t vector, uint32_t apic_id) {
    return amd_vi::allow_interrupt(requester_id(device), vector, apic_id);
}

bool allow_ioapic_interrupt(uint8_t ioapic_id, uint8_t vector, uint32_t apic_id) {
    return amd_vi::allow_ioapic_interrupt(ioapic_id, vector, apic_id);
}

void tick(uint64_t now_ms) {
    if (!amd_vi::running() || now_ms < g_next_drain_ms) {
        return;
    }
    g_next_drain_ms = now_ms + kDrainIntervalMs;
    amd_vi::drain_events();
}

} // namespace iommu
