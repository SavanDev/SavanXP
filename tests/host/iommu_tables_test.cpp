/*
 * iommu_tables_test.cpp -- the IVRS and DMAR parsers against firmware that
 * lies (docs/kernel/IOMMU.md).
 *
 * Every table is copied so that it ends exactly where a page with no access
 * begins: a parser that follows a bad length one byte too far dies with
 * SIGSEGV instead of passing by luck. Each malformed table must be refused
 * with the reason that names what is wrong with it.
 */

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include <sys/mman.h>
#include <unistd.h>

#include "kernel/iommu_tables.hpp"

namespace {

using acpi_tables::SdtHeader;
using iommu_tables::Kind;
using iommu_tables::Report;
using iommu_tables::ScopeKind;

int g_failures = 0;

#define CHECK(condition) \
    do { \
        if (!(condition)) { \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
            ++g_failures; \
        } \
    } while (0)

uint8_t* g_memory = nullptr;
size_t g_size = 0;
Report g_report;

using Bytes = std::vector<uint8_t>;

void put16(Bytes& bytes, uint16_t value) {
    bytes.push_back(static_cast<uint8_t>(value));
    bytes.push_back(static_cast<uint8_t>(value >> 8));
}

void put32(Bytes& bytes, uint32_t value) {
    put16(bytes, static_cast<uint16_t>(value));
    put16(bytes, static_cast<uint16_t>(value >> 16));
}

void put64(Bytes& bytes, uint64_t value) {
    put32(bytes, static_cast<uint32_t>(value));
    put32(bytes, static_cast<uint32_t>(value >> 32));
}

void set16(Bytes& bytes, size_t at, uint16_t value) {
    bytes[at] = static_cast<uint8_t>(value);
    bytes[at + 1] = static_cast<uint8_t>(value >> 8);
}

// A table header followed by 12 bytes of fixed part, length filled in later.
Bytes table_start(const char* signature, const uint8_t (&fixed)[12]) {
    Bytes bytes(sizeof(SdtHeader), 0);
    std::memcpy(bytes.data(), signature, 4);
    bytes.insert(bytes.end(), fixed, fixed + 12);
    return bytes;
}

// Copies `bytes` to end at the guard page, with `length` in its header (the
// real size unless a test says otherwise).
const SdtHeader& place(const Bytes& bytes, size_t length) {
    uint8_t* at = g_memory + g_size - length;
    std::memset(g_memory, 0xcc, g_size);
    std::memcpy(at, bytes.data(), length);
    auto* header = reinterpret_cast<SdtHeader*>(at);
    header->length = static_cast<uint32_t>(length);
    return *header;
}

const SdtHeader& place(const Bytes& bytes) {
    return place(bytes, bytes.size());
}

bool refused_with(const char* reason, const char* expected) {
    if (reason == nullptr || std::strcmp(reason, expected) != 0) {
        std::fprintf(stderr, "  expected \"%s\", got \"%s\"\n", expected, reason != nullptr ? reason : "(accepted)");
        return false;
    }
    return g_report.unit_count == 0 && g_report.scope_count == 0 && g_report.reserved_count == 0;
}

// ---- IVRS -----------------------------------------------------------------

constexpr uint8_t kIvinfo[12] = {0x00, 0x30, 0x00, 0x00}; // PAsize 48

// An IVHD header of `type` for the IOMMU at `device_id`, its length patched
// by ivhd_end.
size_t ivhd_begin(Bytes& bytes, uint8_t type, uint16_t device_id, uint64_t base) {
    const size_t start = bytes.size();
    bytes.push_back(type);
    bytes.push_back(0xb0); // flags
    put16(bytes, 0);
    put16(bytes, device_id);
    put16(bytes, 0x40); // capability offset
    put64(bytes, base);
    put16(bytes, 0); // segment
    put16(bytes, 0); // IOMMU info
    put32(bytes, 0); // feature reporting or attributes
    if (type != 0x10) {
        put64(bytes, 0x0000004aull); // EFR image
        put64(bytes, 0);
    }
    return start;
}

void ivhd_end(Bytes& bytes, size_t start) {
    set16(bytes, start + 2, static_cast<uint16_t>(bytes.size() - start));
}

void entry4(Bytes& bytes, uint8_t type, uint16_t id, uint8_t dte = 0) {
    bytes.push_back(type);
    put16(bytes, id);
    bytes.push_back(dte);
}

void entry8(Bytes& bytes, uint8_t type, uint16_t id, uint8_t byte4, uint16_t word5, uint8_t byte7) {
    entry4(bytes, type, id);
    bytes.push_back(byte4);
    put16(bytes, word5);
    bytes.push_back(byte7);
}

void ivmd(Bytes& bytes, uint8_t type, uint16_t device, uint16_t aux, uint64_t start, uint64_t size) {
    bytes.push_back(type);
    bytes.push_back(0x01); // unity mapping
    put16(bytes, 32);
    put16(bytes, device);
    put16(bytes, aux);
    put64(bytes, 0);
    put64(bytes, start);
    put64(bytes, size);
}

// What QEMU's amd-iommu describes, plus one of every entry kind: the IOMMU
// once as type 0x10 and once as 0x11, and a reserved range.
Bytes good_ivrs() {
    Bytes bytes = table_start("IVRS", kIvinfo);

    size_t ivhd = ivhd_begin(bytes, 0x10, 0x0010, 0xfeb80000);
    entry4(bytes, 0x01, 0);
    ivhd_end(bytes, ivhd);

    ivhd = ivhd_begin(bytes, 0x11, 0x0010, 0xfeb80000);
    entry4(bytes, 0x00, 0);                          // padding
    entry4(bytes, 0x02, 0x0008, 0x01);               // one device
    entry4(bytes, 0x03, 0x0100);                     // range 01:00.0 -
    entry4(bytes, 0x04, 0x01ff);                     //       01:1f.7
    entry8(bytes, 0x42, 0x0200, 0, 0x0300, 0);       // aliased device
    entry8(bytes, 0x48, 0, 0x21, 0x00a0, 1);         // I/O APIC 0x21 as 00:14.0
    entry8(bytes, 0x48, 0, 0x00, 0x00a0, 2);         // HPET 0
    entry8(bytes, 0x48, 0, 0x00, 0x00a0, 9);         // a kind not known: skipped
    // ACPI namespace device with a 2-byte UID.
    entry4(bytes, 0xf0, 0x00a5);
    for (int i = 0; i < 16; ++i) {
        bytes.push_back('A');
    }
    bytes.push_back(2);
    bytes.push_back(2);
    bytes.push_back('0');
    bytes.push_back('1');
    ivhd_end(bytes, ivhd);

    ivmd(bytes, 0x21, 0x0008, 0, 0x7f000010, 0x2000);
    return bytes;
}

void test_ivrs_good() {
    const Bytes bytes = good_ivrs();
    CHECK(iommu_tables::parse_ivrs(place(bytes), g_report) == nullptr);
    CHECK(g_report.kind == Kind::amd_vi);
    CHECK(g_report.address_width == 0x30);
    CHECK(g_report.remapping_available);
    // One unit: the 0x11 description supersedes the 0x10 one.
    CHECK(g_report.unit_count == 1);
    const auto& unit = g_report.units[0];
    CHECK(unit.source_type == 0x11);
    CHECK(unit.register_base == 0xfeb80000);
    CHECK(unit.device_id == 0x0010);
    CHECK(unit.has_efr && unit.efr == 0x4a);
    CHECK(!unit.all_devices); // the "all" entry was in the superseded IVHD
    CHECK(!unit.registers_read);
    CHECK(unit.first_scope == 0 && unit.scope_count == 6);
    CHECK(g_report.scope_count == 6);

    const auto* scope = g_report.scopes;
    CHECK(scope[0].kind == ScopeKind::devices && scope[0].first == 0x0008 && scope[0].last == 0x0008 &&
          scope[0].dte_setting == 1 && !scope[0].aliased);
    CHECK(scope[1].kind == ScopeKind::devices && scope[1].first == 0x0100 && scope[1].last == 0x01ff);
    CHECK(scope[2].first == 0x0200 && scope[2].aliased && scope[2].alias == 0x0300);
    CHECK(scope[3].kind == ScopeKind::ioapic && scope[3].handle == 0x21 && scope[3].first == 0x00a0);
    CHECK(scope[4].kind == ScopeKind::hpet && scope[4].handle == 0);
    CHECK(scope[5].kind == ScopeKind::acpi_device && scope[5].first == 0x00a5);

    CHECK(g_report.reserved_count == 1);
    const auto& range = g_report.reserved[0];
    CHECK(range.base == 0x7f000000 && range.end == 0x7f003000); // widened to pages
    CHECK(!range.all_devices && range.first == 0x0008 && range.last == 0x0008 && range.flags == 1);
}

void test_ivrs_legacy_only() {
    Bytes bytes = table_start("IVRS", kIvinfo);
    const size_t ivhd = ivhd_begin(bytes, 0x10, 0x0002, 0xfeb80000);
    entry4(bytes, 0x01, 0);
    ivhd_end(bytes, ivhd);
    ivmd(bytes, 0x20, 0, 0, 0x1000, 0x1000);
    CHECK(iommu_tables::parse_ivrs(place(bytes), g_report) == nullptr);
    CHECK(g_report.unit_count == 1 && g_report.units[0].all_devices && !g_report.units[0].has_efr);
    CHECK(!g_report.remapping_available); // no I/O APIC named
    CHECK(g_report.reserved_count == 1 && g_report.reserved[0].all_devices);
}

bool group_has(const iommu_tables::Group& group, uint16_t id) {
    for (size_t i = 0; i < group.range_count; ++i) {
        if (group.ranges[i].first <= id && id <= group.ranges[i].last) {
            return true;
        }
    }
    return false;
}

// Isolation groups from the IVRS's alias entries (phase M3): a PCIe-to-PCI
// bridge at 03:00.0 whose secondary bus 04 and one more device reach the
// unit under the bridge's ID.
void test_isolation_groups() {
    Bytes bytes = table_start("IVRS", kIvinfo);
    size_t ivhd = ivhd_begin(bytes, 0x11, 0x0002, 0xfeb80000);
    entry4(bytes, 0x02, 0x0008);                      // a device of its own
    entry8(bytes, 0x42, 0x0200, 0, 0x0300, 0);        // 02:00.0 as 03:00.0
    entry8(bytes, 0x43, 0x0400, 0, 0x0300, 0);        // 04:00.0 -
    entry4(bytes, 0x04, 0x04ff);                      // 04:1f.7 as 03:00.0
    ivhd_end(bytes, ivhd);
    CHECK(iommu_tables::parse_ivrs(place(bytes), g_report) == nullptr);

    iommu_tables::Group group = {};
    CHECK(iommu_tables::isolation_group(g_report, 0x0008, group));
    CHECK(group.alias == 0x0008 && group.range_count == 1 && group.ranges[0].first == 0x0008 &&
          group.ranges[0].last == 0x0008);
    // A device the table does not name is a group of its own as well.
    CHECK(iommu_tables::isolation_group(g_report, 0x0010, group));
    CHECK(group.alias == 0x0010 && group.range_count == 1);

    // From any member, the same set: the bridge, its bus, the other device.
    for (uint16_t member : {uint16_t{0x0200}, uint16_t{0x0300}, uint16_t{0x0400}, uint16_t{0x0450}, uint16_t{0x04ff}}) {
        CHECK(iommu_tables::isolation_group(g_report, member, group));
        CHECK(group.alias == 0x0300);
        CHECK(group.ranges[0].first == member && group.ranges[0].last == member);
        CHECK(group_has(group, 0x0200) && group_has(group, 0x0300) && group_has(group, 0x0400) &&
              group_has(group, 0x04ff));
        CHECK(!group_has(group, 0x0008) && !group_has(group, 0x0500) && !group_has(group, 0x03ff));
    }

    // More ranges than a group holds: refused, not cut short.
    bytes = table_start("IVRS", kIvinfo);
    ivhd = ivhd_begin(bytes, 0x11, 0x0002, 0xfeb80000);
    for (uint16_t i = 0; i < iommu_tables::kMaxGroupRanges; ++i) {
        entry8(bytes, 0x42, static_cast<uint16_t>(0x0500 + 0x10 * i), 0, 0x0300, 0);
    }
    ivhd_end(bytes, ivhd);
    CHECK(iommu_tables::parse_ivrs(place(bytes), g_report) == nullptr);
    CHECK(!iommu_tables::isolation_group(g_report, 0x0500, group));
    CHECK(!iommu_tables::isolation_group(g_report, 0x0300, group));
}

// Every prefix of a good table, each ending at the guard page. A prefix that
// ends between entries is a smaller valid table; any other must be refused
// and leave nothing behind. Neither may read past its length.
void test_ivrs_truncated() {
    const Bytes bytes = good_ivrs();
    for (size_t cut = 0; cut < bytes.size(); ++cut) {
        const size_t length = cut < sizeof(SdtHeader) ? sizeof(SdtHeader) : cut;
        const char* reason = iommu_tables::parse_ivrs(place(bytes, length), g_report);
        CHECK(reason != nullptr || g_report.unit_count == 1);
        CHECK(reason == nullptr || g_report.unit_count == 0);
    }
}

void test_ivrs_refused() {
    Bytes bytes = table_start("IVRS", kIvinfo);
    CHECK(refused_with(iommu_tables::parse_ivrs(place(bytes, 40), g_report),
        "the IVRS is shorter than its fixed part"));
    CHECK(refused_with(iommu_tables::parse_ivrs(place(bytes), g_report), "the IVRS describes no IOMMU"));

    // A reserved range alone describes no IOMMU either.
    Bytes only_ivmd = bytes;
    ivmd(only_ivmd, 0x20, 0, 0, 0x1000, 0x1000);
    CHECK(refused_with(iommu_tables::parse_ivrs(place(only_ivmd), g_report), "the IVRS describes no IOMMU"));

    Bytes partial = bytes;
    partial.insert(partial.end(), {0x10, 0x00});
    CHECK(refused_with(iommu_tables::parse_ivrs(place(partial), g_report),
        "an IVRS entry header runs past the end of the table"));

    // A zero length used to be the classic infinite loop.
    Bytes zero = bytes;
    zero.insert(zero.end(), {0x10, 0x00, 0x00, 0x00});
    CHECK(refused_with(iommu_tables::parse_ivrs(place(zero), g_report), "an IVRS entry is shorter than its header"));

    Bytes long_entry = good_ivrs();
    set16(long_entry, 48 + 2, 0x4000);
    CHECK(refused_with(iommu_tables::parse_ivrs(place(long_entry), g_report),
        "an IVRS entry runs past the end of the table"));

    Bytes short_ivhd = bytes;
    size_t ivhd = ivhd_begin(short_ivhd, 0x11, 0x10, 0xfeb80000);
    set16(short_ivhd, ivhd + 2, 24); // type 0x11 needs 40
    short_ivhd.resize(ivhd + 24);
    CHECK(refused_with(iommu_tables::parse_ivrs(place(short_ivhd), g_report), "an IVHD is shorter than its header"));

    Bytes short_ivmd = bytes;
    ivmd(short_ivmd, 0x22, 0, 0, 0x1000, 0x1000);
    set16(short_ivmd, 48 + 2, 24);
    short_ivmd.resize(48 + 24);
    CHECK(refused_with(iommu_tables::parse_ivrs(place(short_ivmd), g_report), "an IVMD is shorter than its 32 bytes"));

    const struct {
        uint64_t base;
        const char* reason;
    } bases[] = {
        {0, "an IVHD's register base is zero or not 16 KiB aligned"},
        {0xfeb81000, "an IVHD's register base is zero or not 16 KiB aligned"},
        {1ull << 52, "an IVHD's register base is above the physical address space"},
    };
    for (const auto& item : bases) {
        Bytes table = bytes;
        ivhd = ivhd_begin(table, 0x10, 0x10, item.base);
        ivhd_end(table, ivhd);
        CHECK(refused_with(iommu_tables::parse_ivrs(place(table), g_report), item.reason));
    }

    // Device entries, each inside an otherwise good IVHD.
    const auto with_entries = [&](const Bytes& entries) {
        Bytes table = bytes;
        const size_t start = ivhd_begin(table, 0x10, 0x10, 0xfeb80000);
        table.insert(table.end(), entries.begin(), entries.end());
        ivhd_end(table, start);
        return table;
    };
    CHECK(refused_with(iommu_tables::parse_ivrs(place(with_entries({0x81, 0, 0, 0})), g_report),
        "an IVHD device entry has a type whose length is unknown"));
    CHECK(refused_with(iommu_tables::parse_ivrs(place(with_entries({0x42, 0, 0, 0})), g_report),
        "an IVHD device entry runs past the end of its IVHD"));
    CHECK(refused_with(iommu_tables::parse_ivrs(place(with_entries({0xf0, 0, 0, 0})), g_report),
        "an IVHD ACPI device entry runs past the end of its IVHD"));
    Bytes acpi_device(22, 0);
    acpi_device[0] = 0xf0;
    acpi_device[21] = 8; // UID length with no UID after it
    CHECK(refused_with(iommu_tables::parse_ivrs(place(with_entries(acpi_device)), g_report),
        "an IVHD device entry runs past the end of its IVHD"));
    CHECK(refused_with(iommu_tables::parse_ivrs(place(with_entries({0x04, 0x10, 0, 0})), g_report),
        "an IVHD range end has no start"));
    CHECK(refused_with(iommu_tables::parse_ivrs(place(with_entries({0x03, 0x10, 0, 0})), g_report),
        "an IVHD range start is not followed by its end"));
    CHECK(refused_with(iommu_tables::parse_ivrs(place(with_entries({0x03, 0x10, 0, 0, 0x02, 0x11, 0, 0})), g_report),
        "an IVHD range start is not followed by its end"));
    CHECK(refused_with(iommu_tables::parse_ivrs(place(with_entries({0x03, 0x10, 0, 0, 0x04, 0x0f, 0, 0})), g_report),
        "an IVHD range ends before it starts"));

    // A superseded IVHD is still checked.
    Bytes superseded = good_ivrs();
    superseded[48 + 24] = 0x81;
    CHECK(refused_with(iommu_tables::parse_ivrs(place(superseded), g_report),
        "an IVHD device entry has a type whose length is unknown"));

    Bytes wraps = bytes;
    ivhd = ivhd_begin(wraps, 0x10, 0x10, 0xfeb80000);
    ivhd_end(wraps, ivhd);
    ivmd(wraps, 0x20, 0, 0, 0xfffffffffffff000ull, 0x2000);
    CHECK(refused_with(iommu_tables::parse_ivrs(place(wraps), g_report), "an IVMD range wraps around the address space"));
    Bytes last_page = bytes;
    ivhd = ivhd_begin(last_page, 0x10, 0x10, 0xfeb80000);
    ivhd_end(last_page, ivhd);
    ivmd(last_page, 0x20, 0, 0, 0xfffffffffffff000ull, 0x1000);
    CHECK(refused_with(iommu_tables::parse_ivrs(place(last_page), g_report),
        "an IVMD range wraps around the address space"));
    Bytes backwards = bytes;
    ivhd = ivhd_begin(backwards, 0x10, 0x10, 0xfeb80000);
    ivhd_end(backwards, ivhd);
    ivmd(backwards, 0x22, 0x0200, 0x0100, 0x1000, 0x1000);
    CHECK(refused_with(iommu_tables::parse_ivrs(place(backwards), g_report),
        "an IVMD device range ends before it starts"));

    Bytes many = bytes;
    for (uint16_t i = 0; i <= iommu_tables::kMaxUnits; ++i) {
        ivhd = ivhd_begin(many, 0x10, i, 0xfeb80000 + i * 0x4000ull);
        ivhd_end(many, ivhd);
    }
    CHECK(refused_with(iommu_tables::parse_ivrs(place(many), g_report), "more IOMMU units than the kernel records"));
}

// ---- DMAR -----------------------------------------------------------------

constexpr uint8_t kDmarFixed[12] = {38, 0x01}; // 39-bit host address width, INTR_REMAP

size_t structure_begin(Bytes& bytes, uint16_t type) {
    const size_t start = bytes.size();
    put16(bytes, type);
    put16(bytes, 0);
    return start;
}

void structure_end(Bytes& bytes, size_t start) {
    set16(bytes, start + 2, static_cast<uint16_t>(bytes.size() - start));
}

size_t drhd_begin(Bytes& bytes, uint8_t flags, uint64_t base) {
    const size_t start = structure_begin(bytes, 0);
    bytes.push_back(flags);
    bytes.push_back(0); // one page of registers
    put16(bytes, 0);    // segment
    put64(bytes, base);
    return start;
}

void device_scope(Bytes& bytes, uint8_t type, uint8_t id, uint8_t bus, std::initializer_list<uint8_t> path) {
    bytes.push_back(type);
    bytes.push_back(static_cast<uint8_t>(6 + path.size()));
    put16(bytes, 0);
    bytes.push_back(id);
    bytes.push_back(bus);
    bytes.insert(bytes.end(), path.begin(), path.end());
}

Bytes good_dmar() {
    Bytes bytes = table_start("DMAR", kDmarFixed);

    size_t start = drhd_begin(bytes, 0x00, 0xfed91000);
    device_scope(bytes, 1, 0, 0, {0x02, 0x00});             // 00:02.0, the GPU
    device_scope(bytes, 2, 0, 0, {0x1c, 0x00, 0x00, 0x00}); // behind a bridge
    structure_end(bytes, start);

    start = drhd_begin(bytes, 0x01, 0xfed90000); // INCLUDE_PCI_ALL
    device_scope(bytes, 3, 0x08, 0xf0, {0x1f, 0x00}); // I/O APIC 8
    device_scope(bytes, 4, 0x00, 0xf0, {0x0f, 0x00}); // HPET 0
    device_scope(bytes, 9, 0x00, 0x00, {0x00, 0x00}); // a reserved kind: stepped over
    structure_end(bytes, start);

    start = structure_begin(bytes, 1); // RMRR
    put16(bytes, 0);
    put16(bytes, 0);
    put64(bytes, 0x7b800800); // misaligned: widened
    put64(bytes, 0x7bffffff);
    device_scope(bytes, 1, 0, 0, {0x14, 0x00});
    structure_end(bytes, start);

    start = structure_begin(bytes, 4); // ANDD: not read
    bytes.insert(bytes.end(), 8, 0x55);
    structure_end(bytes, start);
    return bytes;
}

void test_dmar_good() {
    const Bytes bytes = good_dmar();
    CHECK(iommu_tables::parse_dmar(place(bytes), g_report) == nullptr);
    CHECK(g_report.kind == Kind::vt_d);
    CHECK(g_report.address_width == 39);
    CHECK(g_report.remapping_available);
    CHECK(g_report.unit_count == 2);
    CHECK(g_report.units[0].register_base == 0xfed91000 && !g_report.units[0].all_devices);
    CHECK(g_report.units[0].register_size == 4096);
    CHECK(g_report.units[0].first_scope == 0 && g_report.units[0].scope_count == 2);
    CHECK(g_report.units[1].all_devices && g_report.units[1].first_scope == 2 && g_report.units[1].scope_count == 2);

    const auto* scope = g_report.scopes;
    CHECK(scope[0].kind == ScopeKind::pci_endpoint && scope[0].first == 0x0010 && scope[0].path_length == 1);
    CHECK(scope[1].kind == ScopeKind::pci_subhierarchy && scope[1].first == 0x00e0 && scope[1].path_length == 2 &&
          scope[1].path[1][0] == 0 && scope[1].path[1][1] == 0);
    CHECK(scope[2].kind == ScopeKind::ioapic && scope[2].handle == 0x08 && scope[2].first == 0xf0f8);
    CHECK(scope[3].kind == ScopeKind::hpet && scope[3].unit == 1);

    CHECK(g_report.reserved_count == 1);
    const auto& range = g_report.reserved[0];
    CHECK(range.base == 0x7b800000 && range.end == 0x7c000000);
    CHECK(range.first_scope == 4 && range.scope_count == 1);
    CHECK(g_report.scopes[4].unit == iommu_tables::kNoUnit && g_report.scopes[4].first == 0x00a0);
    CHECK(g_report.scope_count == 5);
}

void test_dmar_truncated() {
    const Bytes bytes = good_dmar();
    for (size_t cut = sizeof(SdtHeader); cut < bytes.size(); ++cut) {
        const char* reason = iommu_tables::parse_dmar(place(bytes, cut), g_report);
        CHECK(reason != nullptr || g_report.unit_count != 0);
        CHECK(reason == nullptr || g_report.unit_count == 0);
    }
}

void test_dmar_refused() {
    const Bytes bytes = table_start("DMAR", kDmarFixed);
    CHECK(refused_with(iommu_tables::parse_dmar(place(bytes, 47), g_report), "the DMAR is shorter than its fixed part"));
    CHECK(refused_with(iommu_tables::parse_dmar(place(bytes), g_report),
        "the DMAR describes no remapping hardware unit"));

    Bytes partial = bytes;
    partial.insert(partial.end(), {0x00, 0x00, 0x10});
    CHECK(refused_with(iommu_tables::parse_dmar(place(partial), g_report),
        "a DMAR structure header runs past the end of the table"));
    Bytes zero = bytes;
    zero.insert(zero.end(), {0x00, 0x00, 0x00, 0x00});
    CHECK(refused_with(iommu_tables::parse_dmar(place(zero), g_report), "a DMAR structure is shorter than its header"));
    Bytes long_structure = good_dmar();
    set16(long_structure, 48 + 2, 0x4000);
    CHECK(refused_with(iommu_tables::parse_dmar(place(long_structure), g_report),
        "a DMAR structure runs past the end of the table"));

    Bytes short_drhd = bytes;
    short_drhd.insert(short_drhd.end(), {0x00, 0x00, 0x08, 0x00, 0, 0, 0, 0});
    CHECK(refused_with(iommu_tables::parse_dmar(place(short_drhd), g_report), "a DRHD is shorter than its header"));

    const struct {
        uint64_t base;
        const char* reason;
    } bases[] = {
        {0, "a DRHD's register base is zero or not page aligned"},
        {0xfed90010, "a DRHD's register base is zero or not page aligned"},
        {1ull << 52, "a DRHD's register base is above the physical address space"},
    };
    for (const auto& item : bases) {
        Bytes table = bytes;
        structure_end(table, drhd_begin(table, 1, item.base));
        CHECK(refused_with(iommu_tables::parse_dmar(place(table), g_report), item.reason));
    }

    const auto with_scope = [&](const Bytes& scope) {
        Bytes table = bytes;
        const size_t start = drhd_begin(table, 0, 0xfed90000);
        table.insert(table.end(), scope.begin(), scope.end());
        structure_end(table, start);
        return table;
    };
    CHECK(refused_with(iommu_tables::parse_dmar(place(with_scope({0x01})), g_report),
        "a DMAR device scope header runs past the end of its structure"));
    CHECK(refused_with(iommu_tables::parse_dmar(place(with_scope({0x01, 0x00})), g_report),
        "a DMAR device scope is shorter than its header"));
    CHECK(refused_with(iommu_tables::parse_dmar(place(with_scope({0x01, 0x0a, 0, 0, 0, 0, 2, 0})), g_report),
        "a DMAR device scope runs past the end of its structure"));
    CHECK(refused_with(iommu_tables::parse_dmar(place(with_scope({0x01, 0x07, 0, 0, 0, 0, 2})), g_report),
        "a DMAR device scope's path is not a whole number of hops"));
    CHECK(refused_with(iommu_tables::parse_dmar(place(with_scope({0x01, 0x06, 0, 0, 0, 0})), g_report),
        "a DMAR device scope has no path"));
    CHECK(refused_with(iommu_tables::parse_dmar(place(with_scope({0x01, 0x08, 0, 0, 0, 0, 0x20, 0})), g_report),
        "a DMAR device scope names a device or function out of range"));
    CHECK(refused_with(iommu_tables::parse_dmar(place(with_scope({0x02, 0x0a, 0, 0, 0, 0, 1, 0, 0, 8})), g_report),
        "a DMAR device scope names a device or function out of range"));
    Bytes deep = {0x02, static_cast<uint8_t>(6 + 2 * (iommu_tables::kMaxPathHops + 1)), 0, 0, 0, 0};
    deep.insert(deep.end(), 2 * (iommu_tables::kMaxPathHops + 1), 0);
    CHECK(refused_with(iommu_tables::parse_dmar(place(with_scope(deep)), g_report),
        "a DMAR device scope's path is deeper than the kernel records"));

    const auto with_rmrr = [&](uint64_t base, uint64_t limit) {
        Bytes table = bytes;
        structure_end(table, drhd_begin(table, 1, 0xfed90000));
        const size_t start = structure_begin(table, 1);
        put16(table, 0);
        put16(table, 0);
        put64(table, base);
        put64(table, limit);
        structure_end(table, start);
        return table;
    };
    CHECK(iommu_tables::parse_dmar(place(with_rmrr(0x1000, 0x1fff)), g_report) == nullptr);
    CHECK(refused_with(iommu_tables::parse_dmar(place(with_rmrr(0x2000, 0x1fff)), g_report),
        "an RMRR ends before it starts"));
    CHECK(refused_with(iommu_tables::parse_dmar(place(with_rmrr(0x1000, ~0ull)), g_report),
        "an RMRR wraps around the address space"));
    Bytes short_rmrr = bytes;
    structure_end(short_rmrr, drhd_begin(short_rmrr, 1, 0xfed90000));
    short_rmrr.insert(short_rmrr.end(), {0x01, 0x00, 0x10, 0x00});
    short_rmrr.insert(short_rmrr.end(), 12, 0);
    CHECK(refused_with(iommu_tables::parse_dmar(place(short_rmrr), g_report), "an RMRR is shorter than its header"));

    // One more scope than the kernel keeps, 64 to a DRHD.
    Bytes spread = bytes;
    for (size_t i = 0; i <= iommu_tables::kMaxScopes; i += 64) {
        const size_t drhd = drhd_begin(spread, 0, 0xfed90000 + (i / 64) * 0x1000ull);
        for (size_t j = i; j < i + 64 && j <= iommu_tables::kMaxScopes; ++j) {
            device_scope(spread, 1, 0, static_cast<uint8_t>(j >> 5), {static_cast<uint8_t>(j & 31), 0});
        }
        structure_end(spread, drhd);
    }
    CHECK(refused_with(iommu_tables::parse_dmar(place(spread), g_report),
        "more device entries than the kernel records"));
}

} // namespace

int main() {
    const size_t page = static_cast<size_t>(sysconf(_SC_PAGESIZE));
    g_size = 4 * page;
    void* mapping = mmap(nullptr, g_size + page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mapping == MAP_FAILED || mprotect(static_cast<uint8_t*>(mapping) + g_size, page, PROT_NONE) != 0) {
        std::fprintf(stderr, "iommu_tables_test: cannot set up guarded memory\n");
        return 2;
    }
    g_memory = static_cast<uint8_t*>(mapping);

    test_ivrs_good();
    test_ivrs_legacy_only();
    test_isolation_groups();
    test_ivrs_truncated();
    test_ivrs_refused();
    test_dmar_good();
    test_dmar_truncated();
    test_dmar_refused();

    if (g_failures != 0) {
        std::fprintf(stderr, "iommu_tables_test: %d failure(s)\n", g_failures);
        return 1;
    }
    std::puts("iommu_tables_test: ok");
    return 0;
}
