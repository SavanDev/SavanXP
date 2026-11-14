#pragma once

#include <stddef.h>
#include <stdint.h>

/*
 * The anonymous memory regions of an address space, as a sorted list
 * (docs/kernel/MEMORY_REGIONS.md). Bookkeeping only: nothing here touches a
 * page or allocates, so the host tests run this file as it is
 * (tests/host/region_list_test.cpp). vm:: owns the storage and the pages.
 */

namespace region {

constexpr uint64_t kPageSize = 4096;

// What a region's pages allow. Linux's PROT_* bits have the same values.
constexpr uint8_t kRead = 1;
constexpr uint8_t kWrite = 2;
constexpr uint8_t kExecute = 4;

struct Region {
    uint64_t start;  // page aligned
    uint64_t end;    // page aligned, one past the last byte; start < end
    uint8_t access;
};

// Sorted by start, never overlapping, and never two that touch with the same
// access: those are one region. The storage is the caller's.
struct List {
    Region* entries;
    uint32_t count;
    uint32_t capacity;
};

// The most entries one insert or remove adds: taking the middle out of a
// region leaves two.
constexpr uint32_t kMaxGrowth = 1;

// Whether [start, end) is a range the list can hold: page aligned, not empty,
// not wrapping.
bool valid_range(uint64_t start, uint64_t end);

// The index of the region that holds `address`, or -1.
int32_t find(const List& list, uint64_t address);

// The index of the one region that holds all of [start, end), or -1.
int32_t containing(const List& list, uint64_t start, uint64_t end);

// Whether some region overlaps [start, end).
bool overlaps(const List& list, uint64_t start, uint64_t end);

// Adds [start, end) with `access`. The range must be valid and free of
// regions; a neighbour it touches with the same access absorbs it. False,
// with the list unchanged, otherwise or when a new entry finds no room.
bool insert(List& list, uint64_t start, uint64_t end, uint8_t access);

// Takes [start, end) out of every region it overlaps, splitting one that
// straddles it. Nothing there is not an error. False, with the list
// unchanged, for an invalid range or when a split finds no room.
bool remove(List& list, uint64_t start, uint64_t end);

// What the address space holds besides the list (an image, a stack, a view).
// Given a range, 0 when nothing else is in it, otherwise an address past the
// first obstruction, where the search resumes.
using Obstruction = uint64_t (*)(void* context, uint64_t start, uint64_t end);

// The lowest page-aligned `start` at or above `low` such that
// [start, start + size) ends at or below `high` and is clear of the list and
// of `obstruction` (which may be null). False when there is none.
bool find_gap(const List& list, uint64_t size, uint64_t low, uint64_t high,
              Obstruction obstruction, void* context, uint64_t& start);

} // namespace region
