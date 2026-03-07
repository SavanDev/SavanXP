#include "kernel/physical_memory.hpp"

#include "kernel/string.hpp"

namespace {

constexpr uint64_t kMinimumPhysicalAddress = 0x100000;
constexpr size_t kMaxFreeRegions = 2048;

struct FreeRegion {
    uint64_t base;
    uint64_t end;
};

FreeRegion g_regions[kMaxFreeRegions] = {};
size_t g_region_count = 0;
uint64_t g_hhdm_offset = 0;
uint64_t g_total_pages = 0;
bool g_ready = false;

uint64_t align_up(uint64_t value, uint64_t alignment) {
    const uint64_t mask = alignment - 1;
    return (value + mask) & ~mask;
}

uint64_t align_down(uint64_t value, uint64_t alignment) {
    return value & ~(alignment - 1);
}

void reset_state() {
    memset(g_regions, 0, sizeof(g_regions));
    g_region_count = 0;
    g_hhdm_offset = 0;
    g_total_pages = 0;
    g_ready = false;
}

void remove_region(size_t index) {
    if (index >= g_region_count) {
        return;
    }

    for (size_t cursor = index + 1; cursor < g_region_count; ++cursor) {
        g_regions[cursor - 1] = g_regions[cursor];
    }
    memset(&g_regions[g_region_count - 1], 0, sizeof(g_regions[0]));
    --g_region_count;
}

bool insert_region(size_t index, uint64_t base, uint64_t end) {
    if (base >= end || g_region_count == kMaxFreeRegions || index > g_region_count) {
        return false;
    }

    for (size_t cursor = g_region_count; cursor > index; --cursor) {
        g_regions[cursor] = g_regions[cursor - 1];
    }

    g_regions[index].base = base;
    g_regions[index].end = end;
    ++g_region_count;
    return true;
}

void merge_neighbors(size_t index) {
    if (index >= g_region_count) {
        return;
    }

    while (index > 0 && g_regions[index - 1].end >= g_regions[index].base) {
        if (g_regions[index].end > g_regions[index - 1].end) {
            g_regions[index - 1].end = g_regions[index].end;
        }
        remove_region(index);
        --index;
    }

    while (index + 1 < g_region_count && g_regions[index].end >= g_regions[index + 1].base) {
        if (g_regions[index + 1].end > g_regions[index].end) {
            g_regions[index].end = g_regions[index + 1].end;
        }
        remove_region(index + 1);
    }
}

bool add_free_region(uint64_t base, uint64_t end) {
    if (base >= end) {
        return false;
    }

    size_t index = 0;
    while (index < g_region_count && g_regions[index].base < base) {
        ++index;
    }

    if (!insert_region(index, base, end)) {
        return false;
    }
    merge_neighbors(index);
    return true;
}

} // namespace

namespace memory {

void initialize(const boot::BootInfo& boot_info) {
    reset_state();
    g_hhdm_offset = boot_info.hhdm_offset;

    if (g_hhdm_offset == 0) {
        return;
    }

    for (size_t index = 0; index < boot_info.memory_map_entries; ++index) {
        const boot::MemoryRegion& entry = boot_info.memory_map[index];
        if (entry.type != boot::MemoryRegionType::usable) {
            continue;
        }

        uint64_t start = align_up(entry.base, kPageSize);
        uint64_t end = align_down(entry.base + entry.length, kPageSize);
        if (start < kMinimumPhysicalAddress) {
            start = kMinimumPhysicalAddress;
        }
        if (start >= end) {
            continue;
        }
        if (!add_free_region(start, end)) {
            reset_state();
            return;
        }
        g_total_pages += (end - start) / kPageSize;
    }

    g_ready = g_region_count != 0 && g_total_pages != 0;
}

bool ready() {
    return g_ready;
}

bool allocate_page(PageAllocation& allocation) {
    return allocate_contiguous_pages(1, allocation);
}

bool allocate_contiguous_pages(uint64_t page_count, PageAllocation& allocation) {
    if (!g_ready || page_count == 0) {
        return false;
    }

    const uint64_t byte_count = page_count * kPageSize;
    for (size_t index = 0; index < g_region_count; ++index) {
        FreeRegion& region = g_regions[index];
        if ((region.end - region.base) < byte_count) {
            continue;
        }

        allocation.physical_address = region.base;
        allocation.virtual_address = reinterpret_cast<void*>(region.base + g_hhdm_offset);
        allocation.page_count = page_count;
        region.base += byte_count;
        if (region.base == region.end) {
            remove_region(index);
        }
        return true;
    }

    return false;
}

bool free_pages(uint64_t physical_address, uint64_t page_count) {
    if (!g_ready || page_count == 0 || (physical_address & (kPageSize - 1)) != 0) {
        return false;
    }

    const uint64_t byte_count = page_count * kPageSize;
    if (byte_count == 0) {
        return false;
    }
    return add_free_region(physical_address, physical_address + byte_count);
}

bool free_allocation(const PageAllocation& allocation) {
    return free_pages(allocation.physical_address, allocation.page_count);
}

uint64_t total_page_count() {
    return g_total_pages;
}

uint64_t free_page_count() {
    if (!g_ready) {
        return 0;
    }

    uint64_t pages = 0;
    for (size_t index = 0; index < g_region_count; ++index) {
        pages += (g_regions[index].end - g_regions[index].base) / kPageSize;
    }
    return pages;
}

} // namespace memory
