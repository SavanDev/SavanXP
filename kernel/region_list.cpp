#include "kernel/region_list.hpp"

namespace region {
namespace {

bool page_aligned(uint64_t value) {
    return (value & (kPageSize - 1)) == 0;
}

// The first index whose region ends after `address`: the only one that can
// hold it or lie past it, by the list's order.
uint32_t first_ending_after(const List& list, uint64_t address) {
    uint32_t low = 0;
    uint32_t high = list.count;
    while (low < high) {
        const uint32_t middle = low + (high - low) / 2;
        if (list.entries[middle].end <= address) {
            low = middle + 1;
        } else {
            high = middle;
        }
    }
    return low;
}

void shift_right(List& list, uint32_t from) {
    for (uint32_t index = list.count; index > from; --index) {
        list.entries[index] = list.entries[index - 1];
    }
    ++list.count;
}

void erase(List& list, uint32_t at) {
    for (uint32_t index = at; index + 1 < list.count; ++index) {
        list.entries[index] = list.entries[index + 1];
    }
    --list.count;
}

} // namespace

bool valid_range(uint64_t start, uint64_t end) {
    return start < end && page_aligned(start) && page_aligned(end);
}

int32_t find(const List& list, uint64_t address) {
    const uint32_t index = first_ending_after(list, address);
    if (index < list.count && list.entries[index].start <= address) {
        return static_cast<int32_t>(index);
    }
    return -1;
}

int32_t containing(const List& list, uint64_t start, uint64_t end) {
    if (start >= end) {
        return -1;
    }
    const int32_t index = find(list, start);
    if (index >= 0 && end <= list.entries[index].end) {
        return index;
    }
    return -1;
}

bool overlaps(const List& list, uint64_t start, uint64_t end) {
    if (start >= end) {
        return false;
    }
    const uint32_t index = first_ending_after(list, start);
    return index < list.count && list.entries[index].start < end;
}

bool insert(List& list, uint64_t start, uint64_t end, uint8_t access) {
    if (!valid_range(start, end) || overlaps(list, start, end)) {
        return false;
    }
    // Where it goes: after every region that ends at or before its start.
    const uint32_t at = first_ending_after(list, start);
    const bool joins_left = at > 0 && list.entries[at - 1].end == start &&
                            list.entries[at - 1].access == access;
    const bool joins_right = at < list.count && list.entries[at].start == end &&
                             list.entries[at].access == access;
    if (joins_left && joins_right) {
        list.entries[at - 1].end = list.entries[at].end;
        erase(list, at);
        return true;
    }
    if (joins_left) {
        list.entries[at - 1].end = end;
        return true;
    }
    if (joins_right) {
        list.entries[at].start = start;
        return true;
    }
    if (list.count >= list.capacity) {
        return false;
    }
    shift_right(list, at);
    list.entries[at] = Region{start, end, access};
    return true;
}

bool remove(List& list, uint64_t start, uint64_t end) {
    if (!valid_range(start, end)) {
        return false;
    }
    const uint32_t first = first_ending_after(list, start);
    if (first >= list.count || list.entries[first].start >= end) {
        return true;
    }
    // One region around the whole range: it becomes two, and nothing else
    // can overlap the range.
    Region& around = list.entries[first];
    if (around.start < start && around.end > end) {
        if (list.count >= list.capacity) {
            return false;
        }
        const Region tail{end, around.end, around.access};
        around.end = start;
        shift_right(list, first + 1);
        list.entries[first + 1] = tail;
        return true;
    }
    // Otherwise each overlapping region loses its part inside the range,
    // and those wholly inside go.
    uint32_t kept = first;
    for (uint32_t index = first; index < list.count; ++index) {
        Region current = list.entries[index];
        if (current.start < end && current.end > start) {
            if (current.start < start) {
                current.end = start;
            } else if (current.end > end) {
                current.start = end;
            } else {
                continue;
            }
        }
        list.entries[kept++] = current;
    }
    list.count = kept;
    return true;
}

bool find_gap(const List& list, uint64_t size, uint64_t low, uint64_t high,
              Obstruction obstruction, void* context, uint64_t& start) {
    if (size == 0 || !page_aligned(size) || !page_aligned(low)) {
        return false;
    }
    uint64_t candidate = low;
    for (;;) {
        const uint64_t candidate_end = candidate + size;
        if (candidate_end < candidate || candidate_end > high) {
            return false;
        }
        const uint32_t index = first_ending_after(list, candidate);
        if (index < list.count && list.entries[index].start < candidate_end) {
            candidate = list.entries[index].end;
            continue;
        }
        if (obstruction != nullptr) {
            const uint64_t resume = obstruction(context, candidate, candidate_end);
            if (resume != 0) {
                // Always forward, and on a page: a careless answer cannot
                // stall the search or misalign it.
                uint64_t next = (resume + kPageSize - 1) & ~(kPageSize - 1);
                if (next <= candidate) {
                    next = candidate + kPageSize;
                }
                if (next < candidate) {
                    return false;
                }
                candidate = next;
                continue;
            }
        }
        start = candidate;
        return true;
    }
}

} // namespace region
