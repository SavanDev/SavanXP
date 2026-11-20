#include "kernel/iommu.hpp"

#include "kernel/acpi.hpp"
#include "kernel/console.hpp"
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

constexpr uint64_t kPageSize = 4096;

Report g_report;
bool g_present = false;

const char* kind_name(Kind kind) {
    return kind == Kind::amd_vi ? "AMD-Vi" : "VT-d";
}

/* The register base came from firmware. Before it is mapped and read it must
 * not lie in memory the kernel uses as memory: a read of RAM is harmless, but
 * a base that points at it is a table that lies, and nothing read there would
 * mean anything. Reserved ranges are where device registers live. */
bool base_outside_memory(const boot::BootInfo& boot_info, uint64_t base) {
    const uint64_t end = base + kPageSize;
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

// Reads, never writes: M1 leaves every unit exactly as firmware left it.
void read_unit_state(const boot::BootInfo& boot_info, Kind kind, Unit& unit) {
    if (!base_outside_memory(boot_info, unit.register_base)) {
        return;
    }
    void* mapping = nullptr;
    if (!vm::map_kernel_mmio(unit.register_base, kPageSize, vm::kPagePresent, &mapping)) {
        return;
    }
    const auto* registers = static_cast<const volatile uint8_t*>(mapping);
    if (kind == Kind::amd_vi) {
        const uint64_t control = read64(registers, kAmdControl);
        if (control == ~0ull) {
            return; // nothing answers there
        }
        unit.registers_read = true;
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
        unit.remapping_supported = (extended & kVtdEcapInterruptRemapping) != 0;
        unit.translation_enabled = (status & kVtdStatusTranslation) != 0;
        unit.remapping_enabled = (status & kVtdStatusRemapping) != 0;
    }
}

const char kHexDigits[] = "0123456789abcdef";

// "bb:dd.f" for a requester ID.
void format_device(uint16_t id, char (&out)[8]) {
    const unsigned bus = id >> 8;
    const unsigned device = (id >> 3) & 0x1fu;
    out[0] = kHexDigits[bus >> 4];
    out[1] = kHexDigits[bus & 0xfu];
    out[2] = ':';
    out[3] = kHexDigits[device >> 4];
    out[4] = kHexDigits[device & 0xfu];
    out[5] = '.';
    out[6] = kHexDigits[id & 0x7u];
    out[7] = '\0';
}

void log_unit(size_t index, const Unit& unit) {
    console::printf("iommu: unit %u at 0x%llx, segment %u, ",
        static_cast<unsigned>(index), unit.register_base, static_cast<unsigned>(unit.segment));
    if (g_report.kind == Kind::amd_vi) {
        char device[8];
        format_device(unit.device_id, device);
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
        read_unit_state(boot_info, kind, unit);
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

} // namespace iommu
