// Host tests of the region list (include/kernel/region_list.hpp): splitting,
// merging, overlap and overflow, as the Linux roadmap's L2 asks
// (docs/kernel/MEMORY_REGIONS.md).
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>

#include "kernel/region_list.hpp"

namespace {

constexpr uint64_t P = region::kPageSize;
constexpr uint8_t RW = region::kRead | region::kWrite;
constexpr uint8_t R = region::kRead;
constexpr uint8_t NONE = 0;

int g_checks = 0;

void check(bool ok, const char* what) {
    ++g_checks;
    if (!ok) {
        std::fprintf(stderr, "region-list-test: FAIL %s\n", what);
        std::exit(1);
    }
}

struct Fixture {
    region::Region storage[8];
    region::List list;

    explicit Fixture(uint32_t capacity = 8) {
        std::memset(storage, 0xa5, sizeof(storage));
        list = region::List{storage, 0, capacity};
    }
};

// The list holds exactly these (start, end, access) triples, in pages.
bool is(const region::List& list, std::initializer_list<region::Region> want) {
    if (list.count != want.size()) {
        return false;
    }
    uint32_t index = 0;
    for (const region::Region& expected : want) {
        const region::Region& got = list.entries[index++];
        if (got.start != expected.start * P || got.end != expected.end * P || got.access != expected.access) {
            return false;
        }
    }
    return true;
}

void test_ranges() {
    check(region::valid_range(P, 2 * P), "a page is a range");
    check(!region::valid_range(P, P), "an empty range is not");
    check(!region::valid_range(2 * P, P), "a backwards range is not");
    check(!region::valid_range(P + 1, 2 * P), "an unaligned start is not");
    check(!region::valid_range(P, 2 * P - 1), "an unaligned end is not");
    // What start + size gives when it wraps: an end below the start.
    const uint64_t top = ~uint64_t(0) & ~(P - 1);
    check(!region::valid_range(top, top + 2 * P), "a range that wraps past the top is not");
}

void test_insert_and_merge() {
    Fixture f;
    check(region::insert(f.list, 10 * P, 12 * P, RW), "insert into an empty list");
    check(region::insert(f.list, 20 * P, 21 * P, RW), "insert after");
    check(region::insert(f.list, 2 * P, 3 * P, RW), "insert before");
    check(is(f.list, {{2, 3, RW}, {10, 12, RW}, {20, 21, RW}}), "kept sorted");

    check(region::insert(f.list, 12 * P, 14 * P, RW), "touching on the left");
    check(is(f.list, {{2, 3, RW}, {10, 14, RW}, {20, 21, RW}}), "merges with the left neighbour");
    check(region::insert(f.list, 18 * P, 20 * P, RW), "touching on the right");
    check(is(f.list, {{2, 3, RW}, {10, 14, RW}, {18, 21, RW}}), "merges with the right neighbour");
    check(region::insert(f.list, 14 * P, 18 * P, RW), "filling the hole");
    check(is(f.list, {{2, 3, RW}, {10, 21, RW}}), "merges both neighbours into one");

    check(region::insert(f.list, 21 * P, 22 * P, R), "touching with another access");
    check(region::insert(f.list, 1 * P, 2 * P, NONE), "touching on the left with no access");
    check(is(f.list, {{1, 2, NONE}, {2, 3, RW}, {10, 21, RW}, {21, 22, R}}),
          "a different access does not merge");
}

void test_overlap() {
    Fixture f;
    check(region::insert(f.list, 10 * P, 20 * P, RW), "setup");
    check(!region::insert(f.list, 15 * P, 16 * P, RW), "inside an existing region");
    check(!region::insert(f.list, 5 * P, 11 * P, RW), "over the start of one");
    check(!region::insert(f.list, 19 * P, 25 * P, RW), "over the end of one");
    check(!region::insert(f.list, 5 * P, 25 * P, RW), "around one");
    check(!region::insert(f.list, 10 * P, 20 * P, RW), "exactly one");
    check(is(f.list, {{10, 20, RW}}), "refusals change nothing");

    check(region::overlaps(f.list, 19 * P, 21 * P), "overlaps the last page");
    check(!region::overlaps(f.list, 20 * P, 21 * P), "touching is not overlapping");
    check(!region::overlaps(f.list, 9 * P, 10 * P), "touching below is not either");
    check(!region::overlaps(f.list, 15 * P, 15 * P), "an empty range overlaps nothing");

    check(region::find(f.list, 10 * P) == 0, "the first byte is in it");
    check(region::find(f.list, 20 * P - 1) == 0, "the last byte is in it");
    check(region::find(f.list, 20 * P) == -1, "its end is not");
    check(region::find(f.list, 9 * P) == -1, "below is not");
    check(region::containing(f.list, 12 * P, 15 * P) == 0, "a range inside one region");
    check(region::containing(f.list, 12 * P, 21 * P) == -1, "a range past its end");
    check(region::insert(f.list, 20 * P, 22 * P, R), "a neighbour with another access");
    check(region::containing(f.list, 18 * P, 21 * P) == -1, "a range across two regions is in neither");
}

void test_split() {
    Fixture f;
    check(region::insert(f.list, 10 * P, 20 * P, RW), "setup");
    check(region::remove(f.list, 14 * P, 15 * P), "a hole in the middle");
    check(is(f.list, {{10, 14, RW}, {15, 20, RW}}), "splits in two");
    check(region::remove(f.list, 10 * P, 11 * P), "the first page");
    check(is(f.list, {{11, 14, RW}, {15, 20, RW}}), "trims the start");
    check(region::remove(f.list, 19 * P, 30 * P), "past the end");
    check(is(f.list, {{11, 14, RW}, {15, 19, RW}}), "trims the end");
    check(region::remove(f.list, 13 * P, 16 * P), "across the hole");
    check(is(f.list, {{11, 13, RW}, {16, 19, RW}}), "trims both sides of the hole");
    check(region::remove(f.list, 40 * P, 50 * P), "where nothing is");
    check(is(f.list, {{11, 13, RW}, {16, 19, RW}}), "removing nothing is not an error");
    check(region::remove(f.list, 0, 100 * P), "everything");
    check(f.list.count == 0, "leaves an empty list");
    check(!region::remove(f.list, P + 1, 2 * P), "an unaligned range is refused");
}

void test_remove_many() {
    Fixture f;
    for (uint64_t page = 0; page < 6; ++page) {
        check(region::insert(f.list, (10 + 3 * page) * P, (11 + 3 * page) * P, RW), "setup");
    }
    check(f.list.count == 6, "six separate regions");
    check(region::remove(f.list, 13 * P, 23 * P), "a range over four of them");
    check(is(f.list, {{10, 11, RW}, {25, 26, RW}}), "takes the four, keeps the ends");
}

void test_capacity() {
    Fixture f(2);
    check(region::insert(f.list, 10 * P, 11 * P, RW), "first entry");
    check(region::insert(f.list, 20 * P, 21 * P, RW), "second entry");
    check(!region::insert(f.list, 30 * P, 31 * P, RW), "a third does not fit");
    check(region::insert(f.list, 11 * P, 12 * P, RW), "a merge needs no room");
    check(is(f.list, {{10, 12, RW}, {20, 21, RW}}), "after the merge");
    check(region::insert(f.list, 12 * P, 20 * P, RW), "a merge of both needs no room");
    check(is(f.list, {{10, 21, RW}}), "one region left");
    check(region::insert(f.list, 30 * P, 31 * P, RW), "room again");
    check(!region::remove(f.list, 14 * P, 15 * P), "a split with the list full");
    check(is(f.list, {{10, 21, RW}, {30, 31, RW}}), "a refused split changes nothing");
    check(region::remove(f.list, 10 * P, 15 * P), "trimming needs no room");
    check(region::remove(f.list, 30 * P, 31 * P), "removing a whole one needs no room");
    check(is(f.list, {{15, 21, RW}}), "after trimming and removing");
    // Storage past the capacity is never written.
    check(f.storage[2].start == 0xa5a5a5a5a5a5a5a5ull, "nothing past the capacity was touched");
}

struct Wall {
    uint64_t start;
    uint64_t end;
    int asked;
};

uint64_t wall_in_the_way(void* context, uint64_t start, uint64_t end) {
    Wall* wall = static_cast<Wall*>(context);
    ++wall->asked;
    return (start < wall->end && end > wall->start) ? wall->end : 0;
}

uint64_t stalls(void*, uint64_t start, uint64_t) {
    return start;  // says "blocked" without moving: the search must still advance
}

void test_gap() {
    Fixture f;
    uint64_t at = 0;
    check(region::find_gap(f.list, 2 * P, 100 * P, 200 * P, nullptr, nullptr, at) && at == 100 * P,
          "an empty list: the low end");
    check(region::insert(f.list, 100 * P, 103 * P, RW), "setup");
    check(region::insert(f.list, 104 * P, 110 * P, RW), "setup");
    check(region::find_gap(f.list, 2 * P, 100 * P, 200 * P, nullptr, nullptr, at) && at == 110 * P,
          "skips a hole too small");
    check(region::find_gap(f.list, P, 100 * P, 200 * P, nullptr, nullptr, at) && at == 103 * P,
          "takes the first hole that fits");
    check(!region::find_gap(f.list, 100 * P, 100 * P, 200 * P, nullptr, nullptr, at),
          "nothing fits below the limit");
    check(region::find_gap(f.list, 90 * P, 100 * P, 200 * P, nullptr, nullptr, at) && at == 110 * P,
          "fits exactly up to the limit");

    Wall wall{110 * P, 120 * P, 0};
    check(region::find_gap(f.list, 2 * P, 100 * P, 200 * P, wall_in_the_way, &wall, at) && at == 120 * P,
          "an obstruction outside the list is skipped");
    check(wall.asked >= 2, "the obstruction was asked");
    check(region::find_gap(f.list, 2 * P, 100 * P, 200 * P, stalls, nullptr, at) == false,
          "an obstruction everywhere ends the search");

    const uint64_t top = ~uint64_t(0) & ~(P - 1);
    check(!region::find_gap(f.list, 4 * P, top - P, ~uint64_t(0), nullptr, nullptr, at),
          "a size that would wrap is refused");
    check(!region::find_gap(f.list, 0, 100 * P, 200 * P, nullptr, nullptr, at), "a zero size is refused");
    check(!region::find_gap(f.list, P + 1, 100 * P, 200 * P, nullptr, nullptr, at), "an unaligned size is refused");
}

} // namespace

int main() {
    test_ranges();
    test_insert_and_merge();
    test_overlap();
    test_split();
    test_remove_many();
    test_capacity();
    test_gap();
    std::printf("region-list-test: PASS (%d checks: ranges, merging, overlap, splitting, capacity, gaps)\n",
                g_checks);
    return 0;
}
