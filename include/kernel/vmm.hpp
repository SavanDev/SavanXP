#pragma once

#include <stddef.h>
#include <stdint.h>

namespace vm {

enum PageFlags : uint64_t {
    kPageWrite = 1ULL << 1,
    kPageWriteThrough = 1ULL << 3,
    kPageCacheDisable = 1ULL << 4,
};

bool map_kernel_pages(const uint64_t* physical_pages, uint64_t page_count, uint64_t flags, void** virtual_base);
bool unmap_kernel_pages(void* virtual_base, uint64_t page_count);
bool map_kernel_mmio(uint64_t physical_base, size_t size, uint64_t flags, void** virtual_base);
bool map_kernel_device_memory(uint64_t physical_base, size_t size, uint64_t flags, void** virtual_base);
bool kernel_page_cache_flags(uint64_t virtual_address, uint64_t& flags);
bool write_combining_page_flags(uint64_t& flags);
uint64_t hhdm_offset();

} // namespace vm
