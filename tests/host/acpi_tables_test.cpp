/*
 * acpi_tables_test.cpp -- the bounded ACPI readers against firmware that lies.
 *
 * "Physical memory" is one mapping whose last page is followed by a page with
 * no access, so a reader that follows a bad length or address off the end of
 * what it was given dies with SIGSEGV instead of passing by luck. The Reader
 * refuses any range outside the mapping, as the kernel's refuses one above
 * its physical ceiling.
 */

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <sys/mman.h>
#include <unistd.h>

#include "kernel/acpi_tables.hpp"

namespace {

using acpi_tables::MadtEntryHeader;
using acpi_tables::Rsdp;
using acpi_tables::SdtHeader;

int g_failures = 0;

#define CHECK(condition) \
    do { \
        if (!(condition)) { \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
            ++g_failures; \
        } \
    } while (0)

// Physical addresses start here so that zero and small numbers are outside.
constexpr uint64_t kBase = 0x100000;
constexpr size_t kPages = 8;

uint8_t* g_memory = nullptr;
size_t g_size = 0;

const void* map_memory(uint64_t physical, size_t length, void*) {
    if (physical < kBase || length == 0 || physical - kBase >= g_size || length > g_size - (physical - kBase)) {
        return nullptr;
    }
    return g_memory + (physical - kBase);
}

constexpr acpi_tables::Reader kReader = {&map_memory, nullptr};

void reset_memory() {
    std::memset(g_memory, 0, g_size);
}

uint8_t* at(uint64_t physical) {
    return g_memory + (physical - kBase);
}

void fix_checksum(uint8_t* data, size_t length, size_t checksum_offset) {
    data[checksum_offset] = 0;
    uint8_t sum = 0;
    for (size_t i = 0; i < length; ++i) {
        sum = static_cast<uint8_t>(sum + data[i]);
    }
    data[checksum_offset] = static_cast<uint8_t>(0u - sum);
}

// Writes a table header at `physical` claiming `length` bytes.
SdtHeader* put_table(uint64_t physical, const char* signature, uint32_t length) {
    auto* header = reinterpret_cast<SdtHeader*>(at(physical));
    std::memcpy(header->signature, signature, 4);
    header->length = length;
    return header;
}

Rsdp make_rsdp(uint8_t revision, uint32_t rsdt, uint64_t xsdt) {
    Rsdp rsdp = {};
    std::memcpy(rsdp.signature, "RSD PTR ", 8);
    rsdp.revision = revision;
    rsdp.rsdt_address = rsdt;
    rsdp.length = sizeof(Rsdp);
    rsdp.xsdt_address = xsdt;
    fix_checksum(reinterpret_cast<uint8_t*>(&rsdp), acpi_tables::kRsdpV1Length, offsetof(Rsdp, checksum));
    fix_checksum(reinterpret_cast<uint8_t*>(&rsdp), sizeof(Rsdp), offsetof(Rsdp, extended_checksum));
    return rsdp;
}

void put_entries64(uint64_t root, const uint64_t* entries, size_t count) {
    std::memcpy(at(root) + sizeof(SdtHeader), entries, count * sizeof(uint64_t));
}

constexpr uint64_t kRoot = kBase + 0x100;
constexpr uint64_t kFadt = kBase + 0x1000;
constexpr uint64_t kMadt = kBase + 0x2000;
uint64_t last_page() {
    return kBase + g_size - 0x1000;
}

void test_rsdp() {
    Rsdp rsdp = make_rsdp(2, 0, kRoot);
    CHECK(acpi_tables::rsdp_valid(rsdp));
    rsdp.oem_id[0] ^= 1;
    CHECK(!acpi_tables::rsdp_valid(rsdp));
    rsdp = make_rsdp(2, 0, kRoot);
    rsdp.signature[0] = 'X';
    CHECK(!acpi_tables::rsdp_valid(rsdp));
}

void test_find_through_xsdt() {
    reset_memory();
    const uint64_t entries[] = {
        0,                       // empty slot
        0xdead0000beef0000ull,   // nowhere
        kBase + g_size - 8,      // header straddles the end of memory
        kMadt,
        kFadt,
    };
    put_table(kRoot, "XSDT", sizeof(SdtHeader) + sizeof(entries));
    put_entries64(kRoot, entries, 5);
    put_table(kMadt, "APIC", 44);
    put_table(kFadt, "FACP", 116);
    const Rsdp rsdp = make_rsdp(2, 0, kRoot);

    const SdtHeader* fadt = acpi_tables::find_table(kReader, rsdp, "FACP");
    CHECK(fadt == reinterpret_cast<const SdtHeader*>(at(kFadt)));
    CHECK(acpi_tables::find_table(kReader, rsdp, "APIC") == reinterpret_cast<const SdtHeader*>(at(kMadt)));
    CHECK(acpi_tables::find_table(kReader, rsdp, "HPET") == nullptr);
    CHECK(!acpi_tables::table_checksum_ok(*fadt));
    fix_checksum(at(kFadt), 116, offsetof(SdtHeader, checksum));
    CHECK(acpi_tables::table_checksum_ok(*fadt));
}

void test_find_through_rsdt() {
    reset_memory();
    const uint32_t entries[] = {0, static_cast<uint32_t>(kFadt)};
    put_table(kRoot, "RSDT", sizeof(SdtHeader) + sizeof(entries));
    std::memcpy(at(kRoot) + sizeof(SdtHeader), entries, sizeof(entries));
    put_table(kFadt, "FACP", 116);

    CHECK(acpi_tables::find_table(kReader, make_rsdp(0, static_cast<uint32_t>(kRoot), 0), "FACP") != nullptr);

    // Revision 2 whose structure is too short to hold an XSDT address: the
    // field is not there to be read, whatever the bytes say.
    Rsdp rsdp = make_rsdp(2, static_cast<uint32_t>(kRoot), 0xdead0000beef0000ull);
    rsdp.length = acpi_tables::kRsdpV1Length;
    CHECK(acpi_tables::find_table(kReader, rsdp, "FACP") != nullptr);

    // A wrong extended checksum falls back to the RSDT.
    rsdp = make_rsdp(2, static_cast<uint32_t>(kRoot), 0xdead0000beef0000ull);
    rsdp.reserved[0] ^= 1;
    CHECK(acpi_tables::find_table(kReader, rsdp, "FACP") != nullptr);
}

void test_hostile_roots() {
    reset_memory();
    const uint64_t entries[] = {kFadt};
    put_table(kFadt, "FACP", 116);

    // Shorter than its own header: the entry count used to underflow.
    SdtHeader* root = put_table(kRoot, "XSDT", 8);
    put_entries64(kRoot, entries, 1);
    const Rsdp rsdp = make_rsdp(2, 0, kRoot);
    CHECK(acpi_tables::find_table(kReader, rsdp, "FACP") == nullptr);

    // Longer than any root table, and longer than memory.
    root->length = 0xffffffffu;
    CHECK(acpi_tables::find_table(kReader, rsdp, "FACP") == nullptr);
    root->length = static_cast<uint32_t>(g_size);
    CHECK(acpi_tables::find_table(kReader, rsdp, "FACP") == nullptr);

    // Within the cap but past the end of memory.
    const uint64_t late_root = last_page() + 0x800;
    put_table(late_root, "XSDT", 0x1000);
    CHECK(acpi_tables::find_table(kReader, make_rsdp(2, 0, late_root), "FACP") == nullptr);

    // Not a root table at all.
    root->length = sizeof(SdtHeader) + sizeof(entries);
    std::memcpy(root->signature, "FACP", 4);
    CHECK(acpi_tables::find_table(kReader, rsdp, "FACP") == nullptr);
    std::memcpy(root->signature, "XSDT", 4);
    CHECK(acpi_tables::find_table(kReader, rsdp, "FACP") != nullptr);

    // The root points nowhere.
    CHECK(acpi_tables::find_table(kReader, make_rsdp(2, 0, 0x10), "FACP") == nullptr);
    CHECK(acpi_tables::find_table(kReader, make_rsdp(0, 0, 0), "FACP") == nullptr);
}

void test_hostile_tables() {
    reset_memory();
    // A table whose length runs off the end of memory is not returned, so
    // nobody reads its tail.
    const uint64_t late = last_page() + 0xf00;
    put_table(late, "FACP", 0x200);
    CHECK(acpi_tables::map_table(kReader, late) == nullptr);
    put_table(late, "FACP", 0x100);
    CHECK(acpi_tables::map_table(kReader, late) != nullptr);
    put_table(late, "FACP", sizeof(SdtHeader) - 1);
    CHECK(acpi_tables::map_table(kReader, late) == nullptr);
    put_table(late, "FACP", acpi_tables::kMaxTableLength + 1);
    CHECK(acpi_tables::map_table(kReader, late) == nullptr);
    CHECK(acpi_tables::map_table(kReader, 0) == nullptr);
}

void test_madt_entries() {
    reset_memory();
    // The table ends exactly where memory does.
    const uint32_t length = static_cast<uint32_t>(acpi_tables::kMadtFixedLength + 12 + 10 + 3);
    const uint64_t physical = kBase + g_size - length;
    const SdtHeader* madt = put_table(physical, "APIC", length);
    uint8_t* entries = at(physical) + acpi_tables::kMadtFixedLength;
    entries[0] = 1;  entries[1] = 12;   // I/O APIC
    entries[12] = 2; entries[13] = 10;  // interrupt source override
    entries[22] = 9; entries[23] = 200; // claims 200 bytes with 3 left

    const MadtEntryHeader* entry = acpi_tables::madt_next_entry(*madt, nullptr);
    CHECK(entry != nullptr && entry->type == 1 && entry->length == 12);
    entry = acpi_tables::madt_next_entry(*madt, entry);
    CHECK(entry != nullptr && entry->type == 2 && entry->length == 10);
    CHECK(acpi_tables::madt_next_entry(*madt, entry) == nullptr);

    // A zero length must end the walk, not spin on the same entry.
    entries[13] = 0;
    entry = acpi_tables::madt_next_entry(*madt, nullptr);
    CHECK(acpi_tables::madt_next_entry(*madt, entry) == nullptr);
    entries[13] = 1;
    CHECK(acpi_tables::madt_next_entry(*madt, entry) == nullptr);

    // One byte left after the last entry is not a header.
    entries[13] = 12;
    entry = acpi_tables::madt_next_entry(*madt, entry);
    CHECK(entry != nullptr && entry->length == 12);
    CHECK(acpi_tables::madt_next_entry(*madt, entry) == nullptr);

    // Shorter than its fixed part: no entries at all.
    const SdtHeader* stub = put_table(kMadt, "APIC", sizeof(SdtHeader) + 4);
    CHECK(acpi_tables::madt_next_entry(*stub, nullptr) == nullptr);
}

// A DSDT of `body_length` bytes ending exactly at the end of memory.
const SdtHeader* put_dsdt(const uint8_t* body, size_t body_length) {
    reset_memory();
    const uint32_t length = static_cast<uint32_t>(sizeof(SdtHeader) + body_length);
    const uint64_t physical = kBase + g_size - length;
    const SdtHeader* dsdt = put_table(physical, "DSDT", length);
    std::memcpy(at(physical) + sizeof(SdtHeader), body, body_length);
    return dsdt;
}

void test_s5() {
    uint8_t a = 0xff;
    uint8_t b = 0xff;

    // Name(\_S5_, Package(4){0x05, 0x05, 0, 0}), with and without prefixes.
    const uint8_t plain[] = {0x00, 0x08, '_', 'S', '5', '_', 0x12, 0x06, 0x04, 0x05, 0x05, 0x00, 0x00};
    CHECK(acpi_tables::find_s5(*put_dsdt(plain, sizeof(plain)), a, b) && a == 5 && b == 5);

    const uint8_t prefixed[] = {0x08, '\\', '_', 'S', '5', '_', 0x12, 0x0a, 0x04, 0x0a, 0x07, 0x0a, 0x03, 0x00, 0x00};
    a = b = 0xff;
    CHECK(acpi_tables::find_s5(*put_dsdt(prefixed, sizeof(prefixed)), a, b) && a == 7 && b == 3);

    // The same package cut off at every length: each one ends at the guard
    // page, so reading one byte too many is a crash. None may answer yes
    // before both values are inside the table, nor change the outputs.
    for (size_t cut = 1; cut < sizeof(prefixed); ++cut) {
        a = b = 0xee;
        const bool found = acpi_tables::find_s5(*put_dsdt(prefixed, cut), a, b);
        CHECK(found == (cut >= 13));
        CHECK(found || (a == 0xee && b == 0xee));
    }

    // Not a definition: no NameOp in front.
    const uint8_t reference[] = {0x00, 0x00, '_', 'S', '5', '_', 0x12, 0x06, 0x04, 0x05, 0x05, 0x00, 0x00};
    CHECK(!acpi_tables::find_s5(*put_dsdt(reference, sizeof(reference)), a, b));

    // A PkgLength lead byte that announces three more bytes than there are.
    const uint8_t long_length[] = {0x08, '_', 'S', '5', '_', 0x12, 0xc6};
    CHECK(!acpi_tables::find_s5(*put_dsdt(long_length, sizeof(long_length)), a, b));

    // No body.
    reset_memory();
    CHECK(!acpi_tables::find_s5(*put_table(kFadt, "DSDT", sizeof(SdtHeader)), a, b));
}

} // namespace

int main() {
    const size_t page = static_cast<size_t>(sysconf(_SC_PAGESIZE));
    g_size = kPages * page;
    void* mapping = mmap(nullptr, g_size + page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mapping == MAP_FAILED || mprotect(static_cast<uint8_t*>(mapping) + g_size, page, PROT_NONE) != 0) {
        std::fprintf(stderr, "acpi_tables_test: cannot set up guarded memory\n");
        return 2;
    }
    g_memory = static_cast<uint8_t*>(mapping);

    test_rsdp();
    test_find_through_xsdt();
    test_find_through_rsdt();
    test_hostile_roots();
    test_hostile_tables();
    test_madt_entries();
    test_s5();

    if (g_failures != 0) {
        std::fprintf(stderr, "acpi_tables_test: %d failure(s)\n", g_failures);
        return 1;
    }
    std::puts("acpi_tables_test: ok");
    return 0;
}
