#pragma once

#include <stddef.h>
#include <stdint.h>

#include "kernel/acpi_tables.hpp"

/* Bounded parsers for the two ACPI tables that describe IOMMUs: AMD's IVRS
 * (AMD I/O Virtualization Technology specification, 48882) and Intel's DMAR
 * (VT-d specification, chapter 8). Phase M1 of
 * docs/roadmaps/open/DRIVER_FRAMEWORK_ROADMAP.md; the rules are in
 * docs/kernel/IOMMU.md.
 *
 * Firmware is a hostile input: every length, device range and address is
 * checked, and a table that breaks a rule is refused whole, with a reason
 * that names the rule. Nothing here touches the machine, so the same code
 * runs in the host tests against tables built to be wrong. */
namespace iommu_tables {

enum class Kind : uint8_t {
    amd_vi,
    vt_d,
};

// What a device scope names. AMD's IVHD device entries all become `devices`
// (a range of requester IDs, maybe aliased) or one of the special kinds;
// VT-d's device scopes keep their own kind.
enum class ScopeKind : uint8_t {
    devices,          // requester IDs first..last
    pci_endpoint,     // VT-d: one function, reached through `path`
    pci_subhierarchy, // VT-d: a bridge and everything below it
    ioapic,           // `handle` is the I/O APIC ID
    hpet,             // `handle` is the HPET number
    acpi_device,      // an ACPI namespace device
};

constexpr size_t kMaxUnits = 16;
// Every IVHD device entry or DMAR device scope the kernel keeps. A large
// server's tables list a few hundred.
constexpr size_t kMaxScopes = 512;
constexpr size_t kMaxReserved = 64;
// A VT-d path is one hop per bridge between the start bus and the device.
constexpr size_t kMaxPathHops = 8;
// Scope::unit for a scope that belongs to a reserved range, not to a unit.
constexpr uint8_t kNoUnit = 0xff;

struct Scope {
    uint8_t unit;
    ScopeKind kind;
    // The I/O APIC ID or HPET number; VT-d's enumeration ID for any kind.
    uint8_t handle;
    // AMD only: the DTE setting the firmware asks for these devices.
    uint8_t dte_setting;
    // Requester IDs (bus << 8 | device << 3 | function). AMD: the range, or
    // the one ID of a special device as the IOMMU sees it. VT-d: the start
    // bus and the first hop; the rest of the path needs the bus walk.
    uint16_t first;
    uint16_t last;
    // AMD alias entries: the ID the IOMMU sees for every device in the range.
    // Equal to `first` when there is no alias.
    uint16_t alias;
    bool aliased;
    // VT-d: every hop of the path from the start bus, each (device,
    // function); path[0] is the one in `first`.
    uint8_t path_length;
    uint8_t path[kMaxPathHops][2];
};

struct Unit {
    uint64_t register_base;
    uint32_t register_size;
    uint16_t segment;
    // AMD: the IOMMU's own requester ID. VT-d: zero.
    uint16_t device_id;
    // AMD: the IVHD type that described it (0x10, 0x11 or 0x40). VT-d: zero.
    uint8_t source_type;
    // The IVHD or DRHD flags as firmware wrote them.
    uint8_t flags;
    // VT-d INCLUDE_PCI_ALL, or an AMD "all devices" entry.
    bool all_devices;
    // AMD types 0x11 and 0x40 carry an image of the Extended Feature Register.
    bool has_efr;
    uint64_t efr;
    uint32_t first_scope;
    uint32_t scope_count;

    // Filled from the unit's own registers by iommu::discover; the parser
    // leaves them false.
    bool registers_read;
    bool remapping_supported;
    bool translation_enabled;
    bool remapping_enabled;
};

// Memory firmware demands stay mapped for some devices: AMD's IVMD, Intel's
// RMRR. Rounded outward to whole pages.
struct ReservedRange {
    uint64_t base;
    uint64_t end; // exclusive
    uint16_t segment;
    bool all_devices;
    // AMD: requester IDs first..last. VT-d: the devices are its scopes.
    uint16_t first;
    uint16_t last;
    // AMD: the IVMD flags (unity, read, write, exclusion range).
    uint8_t flags;
    uint32_t first_scope;
    uint32_t scope_count;
};

struct Report {
    Kind kind;
    // AMD: IVinfo's physical address size. VT-d: the host address width.
    uint8_t address_width;
    // IVinfo or the DMAR flags, as firmware wrote them.
    uint32_t table_flags;
    // AMD: an IVHD names an I/O APIC, which remapping its interrupts needs.
    // VT-d: the DMAR's INTR_REMAP flag. iommu::discover narrows this with
    // each unit's own capability.
    bool remapping_available;

    size_t unit_count;
    Unit units[kMaxUnits];
    size_t scope_count;
    Scope scopes[kMaxScopes];
    size_t reserved_count;
    ReservedRange reserved[kMaxReserved];
};

/* Fills `report` from a table readable for its whole `length` (what
 * acpi_tables::map_table returns). Returns nullptr on success, or the reason
 * the table was refused; on refusal `report` holds nothing to use. */
const char* parse_ivrs(const acpi_tables::SdtHeader& table, Report& report);
const char* parse_dmar(const acpi_tables::SdtHeader& table, Report& report);

/* An isolation group (phase M3): the requester IDs the unit cannot tell
 * apart from one function's, so they share whatever domain it is in. AMD's
 * alias entries are where the IVRS says so: a device in an aliased range
 * reaches the unit as the alias, and so does every other device aliased to
 * it. Without an alias the group is the function alone. */
constexpr size_t kMaxGroupRanges = 8;

struct GroupRange {
    uint16_t first;
    uint16_t last;
};

struct Group {
    // The ID the unit sees for the function.
    uint16_t alias;
    size_t range_count;
    GroupRange ranges[kMaxGroupRanges];
};

/* Fills `group` with every requester ID that shares `requester`'s, the
 * function itself first. False when that takes more than kMaxGroupRanges
 * ranges; the group is then not to be attached anywhere. */
bool isolation_group(const Report& report, uint16_t requester, Group& group);

// "bb:dd.f" for a requester ID, as the boot log prints devices.
void format_requester_id(uint16_t id, char (&out)[8]);

} // namespace iommu_tables
