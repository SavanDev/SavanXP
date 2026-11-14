#include "kernel/physical_memory.hpp"
#include <assert.h>
#include <stdio.h>

int main() {
    boot::MemoryRegion regions[] = {
        {0, 0x100000, boot::MemoryRegionType::usable}, // Never allocate below 1 MiB.
        {0x100003, 0x6ffd, boot::MemoryRegionType::usable}, // Six aligned pages.
        {0x200000, 4096, boot::MemoryRegionType::reserved},
        {0x300000, 8192, boot::MemoryRegionType::usable}
    };
    boot::BootInfo boot = {};
    boot.hhdm_offset = 0x40000000;
    boot.memory_map = regions;
    boot.memory_map_entries = 4;
    memory::initialize(boot);
    assert(memory::ready() && memory::total_page_count() == 8);
    memory::PageAllocation pages[8];
    for (unsigned index = 0; index < 8; ++index) {
        assert(memory::allocate_page(pages[index]));
        assert(reinterpret_cast<uintptr_t>(pages[index].virtual_address) ==
               pages[index].physical_address + boot.hhdm_offset);
        for (unsigned previous = 0; previous < index; ++previous)
            assert(pages[previous].physical_address != pages[index].physical_address);
    }
    memory::PageAllocation failed = {1, reinterpret_cast<void*>(1), 1};
    assert(!memory::allocate_page(failed) && failed.page_count == 0 && !failed.virtual_address);
    assert(memory::free_page_count() == 0);
    // Fragmentation defeats a contiguous request, but not independent pages.
    assert(memory::free_allocation(pages[0]) && memory::free_allocation(pages[2]));
    assert(!memory::allocate_contiguous_pages(2, failed));
    assert(memory::free_allocation(pages[1]));
    assert(memory::allocate_contiguous_pages(3, failed));
    assert(failed.physical_address == pages[0].physical_address);
    assert(memory::free_allocation(failed));
    assert(!memory::free_allocation(failed));
    for (unsigned index = 3; index < 8; ++index) assert(memory::free_allocation(pages[index]));
    assert(memory::free_page_count() == memory::total_page_count());
    assert(!memory::allocate_contiguous_pages(0, failed));
    assert(!memory::allocate_contiguous_pages(UINT64_MAX / 4096 + 1, failed));
    assert(!memory::free_pages(0x100001, 1));
    assert(!memory::free_pages(UINT64_MAX & ~4095ull, 2));
    assert(!memory::free_pages(0, UINT64_MAX / 4096 + 1));
    regions[1].length = UINT64_MAX;
    memory::initialize(boot);
    assert(!memory::ready() && memory::free_page_count() == 0);
    regions[1] = {0x100000, 8192, boot::MemoryRegionType::usable};
    regions[3] = regions[1];
    memory::initialize(boot);
    assert(!memory::ready());
    puts("physical allocator exhaustion, fragmentation, reuse and overflow: PASS");
}
