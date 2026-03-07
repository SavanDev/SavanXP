#pragma once

#include <stdint.h>

#include "boot/boot_info.hpp"

namespace memory {

constexpr uint64_t kPageSize = 4096;

struct PageAllocation {
    uint64_t physical_address;
    void* virtual_address;
    uint64_t page_count;
};

void initialize(const boot::BootInfo& boot_info);
bool ready();
bool allocate_page(PageAllocation& allocation);
bool allocate_contiguous_pages(uint64_t page_count, PageAllocation& allocation);
bool free_pages(uint64_t physical_address, uint64_t page_count);
bool free_allocation(const PageAllocation& allocation);
uint64_t total_page_count();
uint64_t free_page_count();

} // namespace memory
