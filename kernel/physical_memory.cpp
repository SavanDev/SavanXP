#include "kernel/physical_memory.hpp"

#include "kernel/string.hpp"

namespace {

constexpr uint64_t kMinimumPhysicalAddress = 0x100000;
constexpr size_t kMaxUsableRegions = 512;

struct RegionCursor {
    uint64_t current;
    uint64_t end;
};

RegionCursor g_regions[kMaxUsableRegions] = {};
size_t g_region_count = 0;
size_t g_active_region = 0;
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
    g_active_region = 0;
    g_hhdm_offset = 0;
    g_total_pages = 0;
    g_ready = false;
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
        if (g_region_count == kMaxUsableRegions) {
            break;
        }

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

        g_regions[g_region_count++] = {
            .current = start,
            .end = end,
        };
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
    if (!g_ready) {
        return false;
    }

    while (g_active_region < g_region_count) {
        RegionCursor& region = g_regions[g_active_region];
        const uint64_t byte_count = page_count * kPageSize;
        if (page_count != 0 && region.current + byte_count <= region.end) {
            allocation.physical_address = region.current;
            allocation.virtual_address = reinterpret_cast<void*>(region.current + g_hhdm_offset);
            region.current += byte_count;
            return true;
        }

        ++g_active_region;
    }

    return false;
}

uint64_t total_page_count() {
    return g_total_pages;
}

uint64_t free_page_count() {
    if (!g_ready) {
        return 0;
    }

    uint64_t pages = 0;
    for (size_t index = g_active_region; index < g_region_count; ++index) {
        const RegionCursor& region = g_regions[index];
        if (region.current < region.end) {
            pages += (region.end - region.current) / kPageSize;
        }
    }

    return pages;
}

} // namespace memory
