#include "kernel/heap.hpp"

#include "kernel/panic.hpp"
#include "kernel/physical_memory.hpp"

namespace {

constexpr uint64_t kDefaultArenaPages = 16;

struct Arena {
    uint8_t* base;
    uint64_t capacity;
    uint64_t used;
};

Arena g_current_arena = {};
uint64_t g_total_bytes = 0;
uint64_t g_used_bytes = 0;
bool g_ready = false;

uint64_t align_up(uint64_t value, uint64_t alignment) {
    const uint64_t mask = alignment - 1;
    return (value + mask) & ~mask;
}

bool acquire_arena(uint64_t minimum_bytes) {
    const uint64_t wanted_bytes = minimum_bytes > (kDefaultArenaPages * memory::kPageSize)
        ? minimum_bytes
        : (kDefaultArenaPages * memory::kPageSize);
    const uint64_t page_count = (wanted_bytes + memory::kPageSize - 1) / memory::kPageSize;

    memory::PageAllocation allocation = {};
    if (!memory::allocate_contiguous_pages(page_count, allocation)) {
        return false;
    }

    g_current_arena.base = static_cast<uint8_t*>(allocation.virtual_address);
    g_current_arena.capacity = page_count * memory::kPageSize;
    g_current_arena.used = 0;
    g_total_bytes += g_current_arena.capacity;
    return true;
}

} // namespace

namespace heap {

void initialize() {
    g_current_arena = {};
    g_total_bytes = 0;
    g_used_bytes = 0;
    g_ready = acquire_arena(kDefaultArenaPages * memory::kPageSize);
}

bool ready() {
    return g_ready;
}

void* allocate(size_t size, size_t alignment) {
    if (!g_ready || size == 0) {
        return nullptr;
    }

    if (alignment == 0 || (alignment & (alignment - 1)) != 0) {
        alignment = 16;
    }

    uint64_t start = align_up(g_current_arena.used, alignment);
    if (start + size > g_current_arena.capacity) {
        if (!acquire_arena(size + alignment)) {
            return nullptr;
        }
        start = align_up(g_current_arena.used, alignment);
    }

    void* address = g_current_arena.base + start;
    g_current_arena.used = start + size;
    g_used_bytes += size;
    return address;
}

void free(void* address) {
    (void)address;
}

uint64_t total_bytes() {
    return g_total_bytes;
}

uint64_t used_bytes() {
    return g_used_bytes;
}

uint64_t free_bytes() {
    return g_total_bytes >= g_used_bytes ? (g_total_bytes - g_used_bytes) : 0;
}

} // namespace heap

void* operator new(size_t size) {
    if (void* address = heap::allocate(size)) {
        return address;
    }

    panic("heap: operator new failed");
}

void* operator new[](size_t size) {
    return operator new(size);
}

void operator delete(void* address) noexcept {
    heap::free(address);
}

void operator delete[](void* address) noexcept {
    heap::free(address);
}

void operator delete(void* address, size_t) noexcept {
    heap::free(address);
}

void operator delete[](void* address, size_t) noexcept {
    heap::free(address);
}
