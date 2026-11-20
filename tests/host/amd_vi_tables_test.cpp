/*
 * amd_vi_tables_test.cpp -- the AMD-Vi identity page tables, device table
 * entries, commands and events (docs/kernel/IOMMU.md).
 *
 * The tables are built in host memory handed out by a fake allocator, then
 * walked back the way the unit walks them: every address inside a mapped
 * range must translate to itself, and the pages just outside must fault.
 */

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "kernel/amd_vi_tables.hpp"

namespace {

int g_failures = 0;

#define CHECK(condition) \
    do { \
        if (!(condition)) { \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
            ++g_failures; \
        } \
    } while (0)

// "Physical" tables live at kTableBase + 4 KiB * index.
constexpr uint64_t kTableBase = 0x40000000;

struct FakeMemory {
    std::vector<uint64_t*> tables;
    size_t limit = 1000;
};

uint64_t fake_allocate(void* context) {
    auto* memory = static_cast<FakeMemory*>(context);
    if (memory->tables.size() == memory->limit) {
        return 0;
    }
    auto* table = static_cast<uint64_t*>(std::aligned_alloc(4096, 4096));
    std::memset(table, 0, 4096);
    memory->tables.push_back(table);
    return kTableBase + (memory->tables.size() - 1) * 4096;
}

uint64_t* fake_access(uint64_t physical, void* context) {
    auto* memory = static_cast<FakeMemory*>(context);
    const uint64_t index = (physical - kTableBase) / 4096;
    if (physical < kTableBase || index >= memory->tables.size()) {
        std::fprintf(stderr, "FAIL: walked to 0x%llx, not a table\n", static_cast<unsigned long long>(physical));
        std::exit(1);
    }
    return memory->tables[index];
}

struct Domain {
    FakeMemory memory;
    amd_vi::TableMemory source{&fake_allocate, &fake_access, &memory};
    uint64_t root = fake_allocate(&memory);

    ~Domain() {
        for (uint64_t* table : memory.tables) {
            std::free(table);
        }
    }

    bool mapped(uint64_t address) {
        uint64_t physical = 0;
        return amd_vi::translate(source, root, address, physical) && physical == address;
    }
};

constexpr uint64_t kMiB = 1ull << 20;
constexpr uint64_t kGiB = 1ull << 30;

void test_identity() {
    Domain domain;
    // A memory map's worth: low memory with a hole, a large run, a run that
    // straddles 2 MiB boundaries without covering one, and a reserved range
    // high up that overlaps nothing.
    CHECK(amd_vi::map_identity(domain.source, domain.root, 0x1000, 0x9f000));
    CHECK(amd_vi::map_identity(domain.source, domain.root, 0x100000, 0x3fe00000));
    CHECK(amd_vi::map_identity(domain.source, domain.root, 0x3ff00000, 0x40100000));
    CHECK(amd_vi::map_identity(domain.source, domain.root, 0xfd00000000ull, 0xfd00004000ull));

    CHECK(!domain.mapped(0));
    CHECK(domain.mapped(0x1000) && domain.mapped(0x9e000) && domain.mapped(0x9efff));
    CHECK(!domain.mapped(0x9f000) && !domain.mapped(0xa0000) && !domain.mapped(0xfffff));
    CHECK(domain.mapped(0x100000) && domain.mapped(0x200000 + 0x1234) && domain.mapped(0x3fdfffff));
    CHECK(!domain.mapped(0x3fe00000) && !domain.mapped(0x3fefffff));
    CHECK(domain.mapped(0x3ff00000) && domain.mapped(0x40000000) && domain.mapped(0x400fffff));
    CHECK(!domain.mapped(0x40100000));
    CHECK(domain.mapped(0xfd00000000ull) && domain.mapped(0xfd00003fffull));
    CHECK(!domain.mapped(0xfd00004000ull) && !domain.mapped(0xfcfffff000ull));
    CHECK(!domain.mapped(64 * kGiB));
    // Past what four levels translate.
    CHECK(!domain.mapped(1ull << 48));

    // The large run uses 2 MiB leaves: about 510 of them, under one level-2
    // table, instead of a quarter of a million 4 KiB ones.
    CHECK(domain.memory.tables.size() < 16);

    // Mapping it all again changes nothing and allocates nothing.
    const size_t before = domain.memory.tables.size();
    CHECK(amd_vi::map_identity(domain.source, domain.root, 0x100000, 0x3fe00000));
    CHECK(amd_vi::map_identity(domain.source, domain.root, 0x200000, 0x201000)); // inside a 2 MiB leaf
    CHECK(domain.memory.tables.size() == before);
    CHECK(amd_vi::map_identity(domain.source, domain.root, 0, 0)); // empty
}

// Every page of a small odd range, and its neighbours.
void test_exact_edges() {
    Domain domain;
    const uint64_t base = 2 * kMiB - 3 * 4096;
    const uint64_t end = 4 * kMiB + 5 * 4096;
    CHECK(amd_vi::map_identity(domain.source, domain.root, base, end));
    CHECK(!domain.mapped(base - 4096));
    for (uint64_t page = base; page < end; page += 4096) {
        CHECK(domain.mapped(page));
    }
    CHECK(!domain.mapped(end));
}

void test_out_of_memory() {
    Domain domain;
    domain.memory.limit = 2; // the root and one more
    CHECK(!amd_vi::map_identity(domain.source, domain.root, 0x1000, 0x2000));
}

void test_device_table_entry() {
    uint64_t entry[4] = {~0ull, ~0ull, ~0ull, ~0ull};
    amd_vi::device_table_entry(0x12345000, 1, entry);
    CHECK((entry[0] & 1) != 0);                  // V
    CHECK((entry[0] & 2) != 0);                  // TV
    CHECK(((entry[0] >> 9) & 7) == 4);           // four levels
    CHECK((entry[0] & amd_vi::kAddressMask) == 0x12345000);
    CHECK((entry[0] >> 61 & 3) == 3);            // IR, IW
    CHECK((entry[0] & ~(amd_vi::kAddressMask | (7ull << 9) | 3ull | (3ull << 61))) == 0);
    CHECK(entry[1] == 1 && entry[2] == 0 && entry[3] == 0); // IV clear: interrupts untranslated
}

void test_commands_and_events() {
    const amd_vi::Command wait = amd_vi::completion_wait(0x123456789abcdef8ull, 0x1122334455667788ull);
    CHECK(wait.words[0] == 0x9abcdef9u); // address bits 31:3 and S
    CHECK(wait.words[1] == ((1u << 28) | 0x45678u));
    CHECK(wait.words[2] == 0x55667788u && wait.words[3] == 0x11223344u);

    const amd_vi::Command device = amd_vi::invalidate_device_entry(0x00a0);
    CHECK(device.words[0] == 0x00a0 && device.words[1] == (2u << 28));

    const amd_vi::Command pages = amd_vi::invalidate_domain_pages(1);
    CHECK(pages.words[1] == ((3u << 28) | 1u));
    CHECK(pages.words[2] == 0xfffff003u && pages.words[3] == 0x7fffffffu);

    CHECK(amd_vi::invalidate_all().words[1] == (8u << 28));

    // An I/O page fault as QEMU writes it: device 00:04.0, a write, at 64 GiB.
    const uint32_t raw[4] = {0x0020, (2u << 28) | (static_cast<uint32_t>(amd_vi::kFaultWrite | 0x8) << 16) | 1u,
                             0x00000000u, 0x10u};
    const amd_vi::Event event = amd_vi::decode_event(raw);
    CHECK(event.code == amd_vi::kEventIoPageFault);
    CHECK(event.device == 0x0020 && event.domain == 1);
    CHECK((event.flags & amd_vi::kFaultWrite) != 0);
    CHECK(event.address == 64 * kGiB);
    CHECK(std::strcmp(amd_vi::event_name(event.code), "I/O page fault") == 0);
    CHECK(std::strcmp(amd_vi::event_name(0xf), "event") == 0);
}

} // namespace

int main() {
    test_identity();
    test_exact_edges();
    test_out_of_memory();
    test_device_table_entry();
    test_commands_and_events();
    if (g_failures != 0) {
        std::fprintf(stderr, "amd_vi_tables_test: %d failure(s)\n", g_failures);
        return 1;
    }
    std::puts("amd_vi_tables_test: ok");
    return 0;
}
