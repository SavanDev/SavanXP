/*
 * amd_vi_tables_test.cpp -- the AMD-Vi page tables (the identity domain and
 * the per-device domains' map and unmap), device table entries, interrupt
 * remapping, commands and events (docs/kernel/IOMMU.md).
 *
 * The tables are built in host memory handed out by a fake allocator, then
 * walked back the way the unit walks them: every address inside a mapped
 * range must translate where it was mapped, and the pages just outside must
 * fault.
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
    std::vector<bool> released;
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
    memory->released.push_back(false);
    return kTableBase + (memory->tables.size() - 1) * 4096;
}

uint64_t* fake_access(uint64_t physical, void* context) {
    auto* memory = static_cast<FakeMemory*>(context);
    const uint64_t index = (physical - kTableBase) / 4096;
    if (physical < kTableBase || index >= memory->tables.size()) {
        std::fprintf(stderr, "FAIL: walked to 0x%llx, not a table\n", static_cast<unsigned long long>(physical));
        std::exit(1);
    }
    if (memory->released[index]) {
        std::fprintf(stderr, "FAIL: walked to 0x%llx after it was released\n", static_cast<unsigned long long>(physical));
        std::exit(1);
    }
    return memory->tables[index];
}

void fake_release(uint64_t physical, void* context) {
    auto* memory = static_cast<FakeMemory*>(context);
    const uint64_t index = (physical - kTableBase) / 4096;
    if (physical < kTableBase || index >= memory->tables.size() || memory->released[index]) {
        std::fprintf(stderr, "FAIL: released 0x%llx, not a live table\n", static_cast<unsigned long long>(physical));
        std::exit(1);
    }
    memory->released[index] = true;
}

struct Domain {
    FakeMemory memory;
    amd_vi::TableMemory source{&fake_allocate, &fake_access, &fake_release, &memory};
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

    // Where a device access needing `access` at `address` lands; ~0 when it
    // faults.
    uint64_t lands(uint64_t address, uint64_t access) {
        uint64_t physical = 0;
        return amd_vi::translate(source, root, address, physical, access) ? physical : ~0ull;
    }

    size_t live_tables() const {
        size_t live = 0;
        for (bool gone : memory.released) {
            live += gone ? 0 : 1;
        }
        return live;
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

constexpr uint64_t kRead = amd_vi::kReadable;
constexpr uint64_t kWrite = amd_vi::kWritable;
constexpr uint64_t kBoth = kRead | kWrite;

// A device domain: device addresses that are not the physical ones, with
// the access each mapping was given.
void test_map_pages() {
    Domain domain;
    const uint64_t receive = 0x100000;
    const uint64_t transmit = 0x200000;
    CHECK(amd_vi::map_pages(domain.source, domain.root, receive, 0x7654000, 3 * 4096, kWrite));
    CHECK(amd_vi::map_pages(domain.source, domain.root, transmit, 0x1234000, 4096, kRead));

    CHECK(domain.lands(receive, kWrite) == 0x7654000);
    CHECK(domain.lands(receive + 2 * 4096 + 0x123, kWrite) == 0x7656123);
    CHECK(domain.lands(receive + 3 * 4096, kWrite) == ~0ull);
    CHECK(domain.lands(receive - 4096, kWrite) == ~0ull);
    CHECK(domain.lands(transmit + 0xfff, kRead) == 0x1234fff);
    // Each mapping allows only what it was given.
    CHECK(domain.lands(receive, kRead) == ~0ull);
    CHECK(domain.lands(transmit, kWrite) == ~0ull);
    CHECK(domain.lands(transmit, kBoth) == ~0ull);
    // The physical addresses themselves are not mapped: this is not identity.
    CHECK(domain.lands(0x7654000, kWrite) == ~0ull);

    // A page already mapped refuses the whole range, and nothing changes.
    CHECK(!amd_vi::map_pages(domain.source, domain.root, transmit - 4096, 0x9000000, 2 * 4096, kBoth));
    CHECK(domain.lands(transmit - 4096, kRead) == ~0ull);
    CHECK(domain.lands(transmit, kRead) == 0x1234000);

    // Malformed requests.
    CHECK(!amd_vi::map_pages(domain.source, domain.root, 0x300000, 0x1000, 0, kBoth));
    CHECK(!amd_vi::map_pages(domain.source, domain.root, 0x300800, 0x1000, 4096, kBoth));
    CHECK(!amd_vi::map_pages(domain.source, domain.root, 0x300000, 0x1800, 4096, kBoth));
    CHECK(!amd_vi::map_pages(domain.source, domain.root, 0x300000, 0x1000, 0x1800, kBoth));
    CHECK(!amd_vi::map_pages(domain.source, domain.root, 0x300000, 0x1000, 4096, 0));
    CHECK(!amd_vi::map_pages(domain.source, domain.root, 0x300000, 0x1000, 4096, amd_vi::kPresent));
    CHECK(!amd_vi::map_pages(domain.source, domain.root, (1ull << 48) - 4096, 0x1000, 2 * 4096, kBoth));
    CHECK(!amd_vi::map_pages(domain.source, domain.root, 0x300000, (1ull << 52) - 4096, 2 * 4096, kBoth));
    CHECK(!amd_vi::map_pages(domain.source, domain.root, ~0ull - 4095, 0x1000, 4096 * 2, kBoth));
    CHECK(domain.lands(0x300000, kRead) == ~0ull);

    // The top of what four levels translate.
    CHECK(amd_vi::map_pages(domain.source, domain.root, (1ull << 48) - 4096, 0x5000, 4096, kBoth));
    CHECK(domain.lands((1ull << 48) - 1, kBoth) == 0x5fff);
}

void test_unmap_and_free() {
    Domain domain;
    CHECK(amd_vi::map_pages(domain.source, domain.root, 0x100000, 0x7654000, 4 * 4096, kBoth));
    CHECK(amd_vi::map_pages(domain.source, domain.root, 64 * kGiB, 0x8000000, 4096, kBoth));

    // The middle two pages go; their neighbours stay.
    CHECK(amd_vi::unmap_pages(domain.source, domain.root, 0x101000, 2 * 4096) == 2);
    CHECK(domain.lands(0x100000, kBoth) == 0x7654000);
    CHECK(domain.lands(0x101000, kRead) == ~0ull);
    CHECK(domain.lands(0x102000, kRead) == ~0ull);
    CHECK(domain.lands(0x103000, kBoth) == 0x7657000);
    // Unmapping what is not mapped clears nothing, tables or not.
    CHECK(amd_vi::unmap_pages(domain.source, domain.root, 0x101000, 2 * 4096) == 0);
    CHECK(amd_vi::unmap_pages(domain.source, domain.root, 512 * kGiB, 4096) == 0);
    // An unmapped page can be mapped again, somewhere else.
    CHECK(amd_vi::map_pages(domain.source, domain.root, 0x101000, 0x9999000, 4096, kRead));
    CHECK(domain.lands(0x101000, kRead) == 0x9999000);

    // Freeing releases every table, root included, and nothing twice (the
    // fake allocator exits on a double release).
    CHECK(domain.live_tables() == domain.memory.tables.size());
    amd_vi::free_tables(domain.source, domain.root);
    CHECK(domain.live_tables() == 0);
}

// Memory running out halfway through a range leaves none of it mapped.
void test_map_out_of_memory() {
    Domain domain;
    // The root, three tables down to the first level-1 table, and no more:
    // the range crosses into a second level-1 table and fails there.
    domain.memory.limit = 4;
    CHECK(!amd_vi::map_pages(domain.source, domain.root, 2 * kMiB - 2 * 4096, 0x100000, 4 * 4096, kBoth));
    CHECK(domain.lands(2 * kMiB - 2 * 4096, kRead) == ~0ull);
    CHECK(domain.lands(2 * kMiB - 4096, kRead) == ~0ull);
    domain.memory.limit = 1000;
    CHECK(amd_vi::map_pages(domain.source, domain.root, 2 * kMiB - 2 * 4096, 0x100000, 4 * 4096, kBoth));
    CHECK(domain.lands(2 * kMiB + 4096, kBoth) == 0x103000);
}

// The blocked domain: an empty root. Nothing translates.
void test_blocked() {
    Domain domain;
    CHECK(domain.lands(0, kRead) == ~0ull);
    CHECK(domain.lands(0x100000, kWrite) == ~0ull);
    uint64_t entry[4] = {};
    amd_vi::device_table_entry(domain.root, 2, entry);
    CHECK((entry[0] & amd_vi::kAddressMask) == domain.root && entry[1] == 2);
}

// Interrupt remapping (M4): the interrupt half of a device table entry, a
// remapping table entry, the invalidation, and a refused interrupt's event.
void test_interrupt_remapping() {
    const uint64_t plain = amd_vi::interrupt_remapping(0x12345400, 0);
    CHECK((plain & 1) != 0);                              // IV
    CHECK(((plain >> 1) & 0xf) == 8);                     // 256 entries
    CHECK((plain & 0x000fffffffffffc0ull) == 0x12345400); // the table
    CHECK(((plain >> 60) & 3) == 2);                      // IntCtl: remapped
    CHECK((plain & ((7ull << 56) | (3ull << 62))) == 0);  // nothing passes through
    // The IVRS's DTE setting: INITPass, EIntPass, NMIPass, Lint0Pass, Lint1Pass.
    const uint64_t passing = amd_vi::interrupt_remapping(0x12345400, 0xc7);
    CHECK((passing & ((7ull << 56) | (3ull << 62))) == ((7ull << 56) | (3ull << 62)));
    CHECK((passing & ~((7ull << 56) | (3ull << 62))) == plain);
    // SysMgt (bits 4-5 of the setting) is not a pass-through bit.
    CHECK(amd_vi::interrupt_remapping(0x12345400, 0x30) == plain);
    // Low bits of a misaligned table never leak into the flags.
    CHECK(amd_vi::interrupt_remapping(0x1234543f, 0) == plain);
    CHECK(amd_vi::kInterruptTableBytes == 1024);

    const uint32_t entry = amd_vi::interrupt_table_entry(0x31, 0x02);
    CHECK((entry & 1) != 0);              // RemapEn
    CHECK(((entry >> 2) & 7) == 0);       // fixed
    CHECK((entry & (1u << 6)) == 0);      // physical
    CHECK((entry & (1u << 7)) == 0);      // not guest mode
    CHECK(((entry >> 8) & 0xff) == 0x02); // destination
    CHECK(((entry >> 16) & 0xff) == 0x31);
    CHECK((entry >> 24) == 0);

    const amd_vi::Command invalidate = amd_vi::invalidate_interrupt_table(0x0028);
    CHECK(invalidate.words[0] == 0x0028 && invalidate.words[1] == (5u << 28));
    CHECK(invalidate.words[2] == 0 && invalidate.words[3] == 0);

    // An interrupt refused by hardware; and a DMA fault as QEMU writes it,
    // with the interrupt flag set but no address, which is not one.
    const uint32_t refused[4] = {0x0028, (2u << 28) | (static_cast<uint32_t>(amd_vi::kFaultInterrupt) << 16),
                                 0xfee00000u, 0};
    CHECK(amd_vi::is_refused_interrupt(amd_vi::decode_event(refused)));
    const uint32_t dma[4] = {0x0028, (2u << 28) | (0x00au << 16), 0, 0};
    CHECK(!amd_vi::is_refused_interrupt(amd_vi::decode_event(dma)));
    const uint32_t outside[4] = {0x0028, (2u << 28) | (static_cast<uint32_t>(amd_vi::kFaultInterrupt) << 16),
                                 0xfef00000u, 0};
    CHECK(!amd_vi::is_refused_interrupt(amd_vi::decode_event(outside)));
}

} // namespace

int main() {
    test_identity();
    test_exact_edges();
    test_out_of_memory();
    test_device_table_entry();
    test_commands_and_events();
    test_map_pages();
    test_unmap_and_free();
    test_map_out_of_memory();
    test_blocked();
    test_interrupt_remapping();
    if (g_failures != 0) {
        std::fprintf(stderr, "amd_vi_tables_test: %d failure(s)\n", g_failures);
        return 1;
    }
    std::puts("amd_vi_tables_test: ok");
    return 0;
}
