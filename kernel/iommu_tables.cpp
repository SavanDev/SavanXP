#include "kernel/iommu_tables.hpp"

#include "kernel/string.hpp"

namespace iommu_tables {

namespace {

using acpi_tables::SdtHeader;

// Both tables carry 12 bytes after the common header before their entries:
// IVinfo and a reserved quadword, or the host address width, the flags and
// ten reserved bytes.
constexpr size_t kFixedLength = sizeof(SdtHeader) + 12;
constexpr uint64_t kPageSize = 4096;
// Neither architecture has a physical address above 52 bits.
constexpr unsigned kPhysicalAddressBits = 52;

constexpr uint8_t kIvhdLegacy = 0x10;
constexpr uint8_t kIvhdEfr = 0x11;
constexpr uint8_t kIvhdAcpiHid = 0x40;
constexpr size_t kIvhdLegacyHeader = 24;
constexpr size_t kIvhdEfrHeader = 40;
constexpr size_t kIvmdLength = 32;
constexpr uint32_t kAmdRegisterSize = 16 * 1024;

constexpr uint16_t kDmarDrhd = 0;
constexpr uint16_t kDmarRmrr = 1;
constexpr size_t kDrhdHeader = 16;
constexpr size_t kRmrrHeader = 24;
constexpr size_t kDeviceScopeHeader = 6;

const char* const kTooManyScopes = "more device entries than the kernel records";
const char* const kTooManyUnits = "more IOMMU units than the kernel records";
const char* const kTooManyReserved = "more reserved memory ranges than the kernel records";
const char* const kRangeOpen = "an IVHD range start is not followed by its end";

uint16_t read16(const uint8_t* bytes) {
    uint16_t value = 0;
    memcpy(&value, bytes, sizeof(value));
    return value;
}

uint32_t read32(const uint8_t* bytes) {
    uint32_t value = 0;
    memcpy(&value, bytes, sizeof(value));
    return value;
}

uint64_t read64(const uint8_t* bytes) {
    uint64_t value = 0;
    memcpy(&value, bytes, sizeof(value));
    return value;
}

void start_report(Report& report, Kind kind) {
    memset(&report, 0, sizeof(report));
    report.kind = kind;
}

// [base, last] widened to whole pages as [out_base, out_end). False when the
// end would wrap.
bool round_to_pages(uint64_t base, uint64_t last, uint64_t& out_base, uint64_t& out_end) {
    if (last > ~0ull - kPageSize) {
        return false;
    }
    out_base = base & ~(kPageSize - 1);
    out_end = (last + kPageSize) & ~(kPageSize - 1);
    return true;
}

const char* add_scope(Report& report, const Scope& scope) {
    if (report.scope_count == kMaxScopes) {
        return kTooManyScopes;
    }
    report.scopes[report.scope_count++] = scope;
    return nullptr;
}

Scope device_scope(uint8_t unit, ScopeKind kind, uint16_t first, uint8_t dte_setting) {
    Scope scope = {};
    scope.unit = unit;
    scope.kind = kind;
    scope.dte_setting = dte_setting;
    scope.first = first;
    scope.last = first;
    scope.alias = first;
    return scope;
}

// ---- IVRS -----------------------------------------------------------------

bool is_ivhd(uint8_t type) {
    return type == kIvhdLegacy || type == kIvhdEfr || type == kIvhdAcpiHid;
}

bool is_ivmd(uint8_t type) {
    return type == 0x20 || type == 0x21 || type == 0x22;
}

size_t ivhd_header_length(uint8_t type) {
    return type == kIvhdLegacy ? kIvhdLegacyHeader : kIvhdEfrHeader;
}

/* Firmware may describe one IOMMU several times, once per IVHD type, so that
 * older software finds the type it knows. The highest type wins, as in every
 * other reader of this table; the others are checked and then ignored. */
bool ivhd_superseded(const uint8_t* table, size_t length, const uint8_t* ivhd) {
    const uint16_t device_id = read16(ivhd + 4);
    const uint16_t segment = read16(ivhd + 16);
    for (size_t at = kFixedLength; at < length; at += read16(table + at + 2)) {
        const uint8_t* other = table + at;
        if (is_ivhd(other[0]) && other[0] > ivhd[0] &&
            read16(other + 4) == device_id && read16(other + 16) == segment) {
            return true;
        }
    }
    return false;
}

/* Checks one IVHD's device entries and, when `record`, files them under the
 * next unit. The entry lengths come from the type: below 0x80 its top two
 * bits give 4 or 8 bytes, 0xf0 declares its own, and any other type cannot
 * be stepped over safely. */
const char* parse_ivhd(Report& report, const uint8_t* ivhd, size_t length, bool record) {
    const uint8_t type = ivhd[0];
    const uint64_t base = read64(ivhd + 8);
    if (base == 0 || (base & (kAmdRegisterSize - 1)) != 0) {
        return "an IVHD's register base is zero or not 16 KiB aligned";
    }
    if ((base >> kPhysicalAddressBits) != 0) {
        return "an IVHD's register base is above the physical address space";
    }
    if (record && report.unit_count == kMaxUnits) {
        return kTooManyUnits;
    }

    const uint8_t unit_index = static_cast<uint8_t>(report.unit_count);
    Unit unit = {};
    unit.register_base = base;
    unit.register_size = kAmdRegisterSize;
    unit.segment = read16(ivhd + 16);
    unit.device_id = read16(ivhd + 4);
    unit.source_type = type;
    unit.flags = ivhd[1];
    unit.has_efr = type != kIvhdLegacy;
    unit.efr = unit.has_efr ? read64(ivhd + 24) : 0;
    unit.first_scope = static_cast<uint32_t>(report.scope_count);

    bool range_open = false;
    Scope range = {};
    bool names_ioapic = false;
    for (size_t at = ivhd_header_length(type); at < length;) {
        const uint8_t* entry = ivhd + at;
        const uint8_t entry_type = entry[0];
        size_t entry_length = 0;
        if (entry_type < 0x80) {
            entry_length = 4u << (entry_type >> 6);
        } else if (entry_type == 0xf0) {
            if (length - at < 22) {
                return "an IVHD ACPI device entry runs past the end of its IVHD";
            }
            entry_length = 22u + entry[21];
        } else {
            return "an IVHD device entry has a type whose length is unknown";
        }
        if (entry_length > length - at) {
            return "an IVHD device entry runs past the end of its IVHD";
        }

        const uint16_t id = read16(entry + 1);
        const uint8_t dte = entry[3];
        const char* refusal = nullptr;
        switch (entry_type) {
        case 0x00: // padding
            break;
        case 0x01: // every device
            if (range_open) {
                return kRangeOpen;
            }
            unit.all_devices = true;
            break;
        case 0x02: // one device
        case 0x42: // one device, aliased
        case 0x46: { // one device, extended setting
            if (range_open) {
                return kRangeOpen;
            }
            Scope scope = device_scope(unit_index, ScopeKind::devices, id, dte);
            if (entry_type == 0x42) {
                scope.alias = read16(entry + 5);
                scope.aliased = true;
            }
            refusal = record ? add_scope(report, scope) : nullptr;
            break;
        }
        case 0x03: // start of a range
        case 0x43: // start of an aliased range
        case 0x47: // start of a range, extended setting
            if (range_open) {
                return kRangeOpen;
            }
            range = device_scope(unit_index, ScopeKind::devices, id, dte);
            if (entry_type == 0x43) {
                range.alias = read16(entry + 5);
                range.aliased = true;
            }
            range_open = true;
            break;
        case 0x04: // end of a range
            if (!range_open) {
                return "an IVHD range end has no start";
            }
            if (id < range.first) {
                return "an IVHD range ends before it starts";
            }
            range.last = id;
            range_open = false;
            refusal = record ? add_scope(report, range) : nullptr;
            break;
        case 0x48: { // I/O APIC or HPET
            if (range_open) {
                return kRangeOpen;
            }
            const uint8_t variety = entry[7];
            if (variety != 1 && variety != 2) {
                break; // a kind of special device this kernel does not know
            }
            Scope scope = device_scope(unit_index, variety == 1 ? ScopeKind::ioapic : ScopeKind::hpet,
                read16(entry + 5), dte);
            scope.handle = entry[4];
            names_ioapic = names_ioapic || variety == 1;
            refusal = record ? add_scope(report, scope) : nullptr;
            break;
        }
        case 0xf0: // ACPI namespace device
            if (range_open) {
                return kRangeOpen;
            }
            refusal = record ? add_scope(report, device_scope(unit_index, ScopeKind::acpi_device, id, dte)) : nullptr;
            break;
        default: // reserved types of a known length
            break;
        }
        if (refusal != nullptr) {
            return refusal;
        }
        at += entry_length;
    }
    if (range_open) {
        return kRangeOpen;
    }

    if (record) {
        unit.scope_count = static_cast<uint32_t>(report.scope_count) - unit.first_scope;
        report.units[report.unit_count++] = unit;
        report.remapping_available = report.remapping_available || names_ioapic;
    }
    return nullptr;
}

const char* parse_ivmd(Report& report, const uint8_t* ivmd) {
    const uint8_t type = ivmd[0];
    const uint64_t start = read64(ivmd + 16);
    const uint64_t size = read64(ivmd + 24);
    if (size == 0) {
        return nullptr; // demands nothing
    }
    ReservedRange range = {};
    if (start > ~0ull - (size - 1) || !round_to_pages(start, start + (size - 1), range.base, range.end)) {
        return "an IVMD range wraps around the address space";
    }
    range.flags = ivmd[1];
    range.all_devices = type == 0x20;
    range.first = read16(ivmd + 4);
    range.last = type == 0x22 ? read16(ivmd + 6) : range.first;
    if (range.all_devices) {
        range.first = 0;
        range.last = 0xffff;
    } else if (range.last < range.first) {
        return "an IVMD device range ends before it starts";
    }
    range.first_scope = static_cast<uint32_t>(report.scope_count);
    if (report.reserved_count == kMaxReserved) {
        return kTooManyReserved;
    }
    report.reserved[report.reserved_count++] = range;
    return nullptr;
}

// ---- DMAR -----------------------------------------------------------------

const char* parse_device_scopes(Report& report, const uint8_t* bytes, size_t length, uint8_t unit,
                                uint32_t& first_scope, uint32_t& scope_count) {
    first_scope = static_cast<uint32_t>(report.scope_count);
    for (size_t at = 0; at < length;) {
        if (length - at < 2) {
            return "a DMAR device scope header runs past the end of its structure";
        }
        const uint8_t* entry = bytes + at;
        const uint8_t type = entry[0];
        const size_t entry_length = entry[1];
        if (entry_length < kDeviceScopeHeader) {
            return "a DMAR device scope is shorter than its header";
        }
        if (entry_length > length - at) {
            return "a DMAR device scope runs past the end of its structure";
        }
        if (((entry_length - kDeviceScopeHeader) & 1u) != 0) {
            return "a DMAR device scope's path is not a whole number of hops";
        }
        const size_t hops = (entry_length - kDeviceScopeHeader) / 2;
        if (hops == 0) {
            return "a DMAR device scope has no path";
        }
        if (hops > kMaxPathHops) {
            return "a DMAR device scope's path is deeper than the kernel records";
        }

        ScopeKind kind = ScopeKind::devices;
        switch (type) {
        case 1: kind = ScopeKind::pci_endpoint; break;
        case 2: kind = ScopeKind::pci_subhierarchy; break;
        case 3: kind = ScopeKind::ioapic; break;
        case 4: kind = ScopeKind::hpet; break;
        case 5: kind = ScopeKind::acpi_device; break;
        default: break; // reserved: stepped over
        }
        if (kind != ScopeKind::devices) {
            Scope scope = {};
            scope.unit = unit;
            scope.kind = kind;
            scope.handle = entry[4];
            scope.path_length = static_cast<uint8_t>(hops);
            for (size_t hop = 0; hop < hops; ++hop) {
                const uint8_t device = entry[kDeviceScopeHeader + hop * 2];
                const uint8_t function = entry[kDeviceScopeHeader + hop * 2 + 1];
                if (device > 31 || function > 7) {
                    return "a DMAR device scope names a device or function out of range";
                }
                scope.path[hop][0] = device;
                scope.path[hop][1] = function;
            }
            scope.first = static_cast<uint16_t>((entry[5] << 8) | (scope.path[0][0] << 3) | scope.path[0][1]);
            scope.last = scope.first;
            scope.alias = scope.first;
            if (const char* refusal = add_scope(report, scope)) {
                return refusal;
            }
        }
        at += entry_length;
    }
    scope_count = static_cast<uint32_t>(report.scope_count) - first_scope;
    return nullptr;
}

const char* parse_drhd(Report& report, const uint8_t* drhd, size_t length) {
    if (length < kDrhdHeader) {
        return "a DRHD is shorter than its header";
    }
    const uint64_t base = read64(drhd + 8);
    if (base == 0 || (base & (kPageSize - 1)) != 0) {
        return "a DRHD's register base is zero or not page aligned";
    }
    if ((base >> kPhysicalAddressBits) != 0) {
        return "a DRHD's register base is above the physical address space";
    }
    if (report.unit_count == kMaxUnits) {
        return kTooManyUnits;
    }
    Unit unit = {};
    unit.register_base = base;
    // Revision 3 and later: 2^N pages of registers; zero means one page.
    unit.register_size = static_cast<uint32_t>(kPageSize << (drhd[5] & 0x0fu));
    unit.segment = read16(drhd + 6);
    unit.flags = drhd[4];
    unit.all_devices = (drhd[4] & 0x01u) != 0;
    const uint8_t unit_index = static_cast<uint8_t>(report.unit_count);
    if (const char* refusal = parse_device_scopes(report, drhd + kDrhdHeader, length - kDrhdHeader, unit_index,
                                                  unit.first_scope, unit.scope_count)) {
        return refusal;
    }
    report.units[report.unit_count++] = unit;
    return nullptr;
}

const char* parse_rmrr(Report& report, const uint8_t* rmrr, size_t length) {
    if (length < kRmrrHeader) {
        return "an RMRR is shorter than its header";
    }
    const uint64_t base = read64(rmrr + 8);
    const uint64_t limit = read64(rmrr + 16);
    if (limit < base) {
        return "an RMRR ends before it starts";
    }
    ReservedRange range = {};
    if (!round_to_pages(base, limit, range.base, range.end)) {
        return "an RMRR wraps around the address space";
    }
    if (report.reserved_count == kMaxReserved) {
        return kTooManyReserved;
    }
    range.segment = read16(rmrr + 6);
    if (const char* refusal = parse_device_scopes(report, rmrr + kRmrrHeader, length - kRmrrHeader, kNoUnit,
                                                  range.first_scope, range.scope_count)) {
        return refusal;
    }
    report.reserved[report.reserved_count++] = range;
    return nullptr;
}

const char kHexDigits[] = "0123456789abcdef";

bool add_group_range(Group& group, uint16_t first, uint16_t last) {
    for (size_t i = 0; i < group.range_count; ++i) {
        if (group.ranges[i].first <= first && last <= group.ranges[i].last) {
            return true;
        }
    }
    if (group.range_count == kMaxGroupRanges) {
        return false;
    }
    group.ranges[group.range_count++] = {first, last};
    return true;
}

// An IVHD device entry with an alias that belongs to a unit.
bool is_alias_scope(const Scope& scope) {
    return scope.kind == ScopeKind::devices && scope.unit != kNoUnit && scope.aliased;
}

} // namespace

bool isolation_group(const Report& report, uint16_t requester, Group& group) {
    group = {};
    group.alias = requester;
    for (size_t i = 0; i < report.scope_count; ++i) {
        const Scope& scope = report.scopes[i];
        if (is_alias_scope(scope) && scope.first <= requester && requester <= scope.last) {
            group.alias = scope.alias;
            break;
        }
    }
    if (!add_group_range(group, requester, requester) || !add_group_range(group, group.alias, group.alias)) {
        return false;
    }
    for (size_t i = 0; i < report.scope_count; ++i) {
        const Scope& scope = report.scopes[i];
        if (is_alias_scope(scope) && scope.alias == group.alias && !add_group_range(group, scope.first, scope.last)) {
            return false;
        }
    }
    return true;
}

void format_requester_id(uint16_t id, char (&out)[8]) {
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

const char* parse_ivrs(const SdtHeader& table, Report& report) {
    start_report(report, Kind::amd_vi);
    const uint8_t* bytes = reinterpret_cast<const uint8_t*>(&table);
    const size_t length = table.length;
    if (length < kFixedLength) {
        return "the IVRS is shorter than its fixed part";
    }
    const uint32_t ivinfo = read32(bytes + sizeof(SdtHeader));
    report.table_flags = ivinfo;
    report.address_width = static_cast<uint8_t>((ivinfo >> 8) & 0x7fu);

    // First every entry's own length, so that the walks below, including the
    // one that looks for a superseding IVHD, can step without checking.
    for (size_t at = kFixedLength; at < length;) {
        if (length - at < 4) {
            return "an IVRS entry header runs past the end of the table";
        }
        const uint8_t* entry = bytes + at;
        const uint16_t entry_length = read16(entry + 2);
        if (entry_length < 4) {
            return "an IVRS entry is shorter than its header";
        }
        if (entry_length > length - at) {
            return "an IVRS entry runs past the end of the table";
        }
        if (is_ivhd(entry[0]) && entry_length < ivhd_header_length(entry[0])) {
            return "an IVHD is shorter than its header";
        }
        if (is_ivmd(entry[0]) && entry_length < kIvmdLength) {
            return "an IVMD is shorter than its 32 bytes";
        }
        at += entry_length;
    }

    for (size_t at = kFixedLength; at < length;) {
        const uint8_t* entry = bytes + at;
        const uint16_t entry_length = read16(entry + 2);
        const char* refusal = nullptr;
        if (is_ivhd(entry[0])) {
            refusal = parse_ivhd(report, entry, entry_length, !ivhd_superseded(bytes, length, entry));
        } else if (is_ivmd(entry[0])) {
            refusal = parse_ivmd(report, entry);
        }
        if (refusal != nullptr) {
            start_report(report, Kind::amd_vi);
            return refusal;
        }
        at += entry_length;
    }

    if (report.unit_count == 0) {
        start_report(report, Kind::amd_vi);
        return "the IVRS describes no IOMMU";
    }
    return nullptr;
}

const char* parse_dmar(const SdtHeader& table, Report& report) {
    start_report(report, Kind::vt_d);
    const uint8_t* bytes = reinterpret_cast<const uint8_t*>(&table);
    const size_t length = table.length;
    if (length < kFixedLength) {
        return "the DMAR is shorter than its fixed part";
    }
    report.address_width = static_cast<uint8_t>(bytes[sizeof(SdtHeader)] + 1u);
    report.table_flags = bytes[sizeof(SdtHeader) + 1];
    report.remapping_available = (report.table_flags & 0x01u) != 0;

    for (size_t at = kFixedLength; at < length;) {
        if (length - at < 4) {
            start_report(report, Kind::vt_d);
            return "a DMAR structure header runs past the end of the table";
        }
        const uint8_t* entry = bytes + at;
        const uint16_t type = read16(entry);
        const uint16_t entry_length = read16(entry + 2);
        const char* refusal = nullptr;
        if (entry_length < 4) {
            refusal = "a DMAR structure is shorter than its header";
        } else if (entry_length > length - at) {
            refusal = "a DMAR structure runs past the end of the table";
        } else if (type == kDmarDrhd) {
            refusal = parse_drhd(report, entry, entry_length);
        } else if (type == kDmarRmrr) {
            refusal = parse_rmrr(report, entry, entry_length);
        }
        if (refusal != nullptr) {
            start_report(report, Kind::vt_d);
            return refusal;
        }
        at += entry_length;
    }

    if (report.unit_count == 0) {
        start_report(report, Kind::vt_d);
        return "the DMAR describes no remapping hardware unit";
    }
    return nullptr;
}

} // namespace iommu_tables
