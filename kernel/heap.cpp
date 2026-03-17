#include "kernel/heap.hpp"

#include "kernel/panic.hpp"
#include "kernel/physical_memory.hpp"

namespace {

constexpr uint64_t kDefaultArenaPages = 16;
constexpr uint64_t kMinimumAlignment = 16;
constexpr uint32_t kBlockMagic = 0x4b484541u;

struct BlockHeader;

struct Arena {
    memory::PageAllocation allocation;
    Arena* next;
    Arena* previous;
    BlockHeader* first_block;
};

struct BlockHeader {
    uint64_t size;
    Arena* arena;
    BlockHeader* next;
    BlockHeader* previous;
    uint32_t magic;
    uint32_t free;
    uint64_t reserved;
};

struct AllocationLayout {
    uint8_t* header_address;
    uint64_t leading_span;
    uint64_t trailing_span;
};

static_assert((sizeof(Arena) % kMinimumAlignment) == 0, "Arena must preserve alignment");
static_assert((sizeof(BlockHeader) % kMinimumAlignment) == 0, "BlockHeader must preserve alignment");

Arena* g_first_arena = nullptr;
uint64_t g_arena_count = 0;
uint64_t g_used_bytes = 0;
bool g_ready = false;

uint64_t align_up(uint64_t value, uint64_t alignment) {
    const uint64_t mask = alignment - 1;
    return (value + mask) & ~mask;
}

bool is_power_of_two(uint64_t value) {
    return value != 0 && (value & (value - 1)) == 0;
}

uint64_t normalize_alignment(size_t alignment) {
    if (!is_power_of_two(alignment) || alignment < kMinimumAlignment) {
        return kMinimumAlignment;
    }
    return alignment;
}

uint64_t minimum_block_span() {
    return sizeof(BlockHeader) + kMinimumAlignment;
}

uint8_t* arena_base(Arena* arena) {
    return reinterpret_cast<uint8_t*>(arena);
}

const uint8_t* arena_base(const Arena* arena) {
    return reinterpret_cast<const uint8_t*>(arena);
}

uint64_t arena_capacity(const Arena* arena) {
    return arena->allocation.page_count * memory::kPageSize;
}

uint8_t* arena_payload_start(Arena* arena) {
    return arena_base(arena) + align_up(sizeof(Arena), kMinimumAlignment);
}

const uint8_t* arena_payload_start(const Arena* arena) {
    return arena_base(arena) + align_up(sizeof(Arena), kMinimumAlignment);
}

uint64_t arena_initial_block_size(const Arena* arena) {
    return arena_capacity(arena) - (arena_payload_start(arena) - arena_base(arena)) - sizeof(BlockHeader);
}

bool blocks_are_adjacent(const BlockHeader* left, const BlockHeader* right) {
    return left != nullptr
        && right != nullptr
        && (reinterpret_cast<const uint8_t*>(left) + sizeof(BlockHeader) + left->size)
            == reinterpret_cast<const uint8_t*>(right);
}

void stamp_block(BlockHeader* block, Arena* arena, uint64_t size, bool free) {
    block->size = size;
    block->arena = arena;
    block->magic = kBlockMagic;
    block->free = free ? 1u : 0u;
    block->reserved = 0;
}

void merge_with_next(BlockHeader* block) {
    if (block == nullptr || block->free == 0u) {
        return;
    }

    while (block->next != nullptr && block->next->free != 0u && blocks_are_adjacent(block, block->next)) {
        BlockHeader* next = block->next;
        block->size += sizeof(BlockHeader) + next->size;
        block->next = next->next;
        if (block->next != nullptr) {
            block->next->previous = block;
        }
    }
}

bool block_is_valid(const BlockHeader* block) {
    const Arena* arena = nullptr;
    const uint8_t* arena_start = nullptr;
    const uint8_t* arena_end = nullptr;
    const uint8_t* block_start = nullptr;
    const uint8_t* block_end = nullptr;

    if (block == nullptr || block->magic != kBlockMagic || block->arena == nullptr) {
        return false;
    }

    arena = block->arena;
    arena_start = arena_base(arena);
    arena_end = arena_start + arena_capacity(arena);
    block_start = reinterpret_cast<const uint8_t*>(block);
    block_end = block_start + sizeof(BlockHeader) + block->size;
    return block_start >= arena_payload_start(arena) && block_end <= arena_end;
}

bool compute_layout(const BlockHeader* block, uint64_t size, uint64_t alignment, AllocationLayout& layout) {
    const uint8_t* block_start = reinterpret_cast<const uint8_t*>(block);
    const uint8_t* block_end = block_start + sizeof(BlockHeader) + block->size;
    uint64_t payload_address = align_up(reinterpret_cast<uint64_t>(block_start) + sizeof(BlockHeader), alignment);

    if (block->free == 0u) {
        return false;
    }

    for (;;) {
        uint8_t* header_address = reinterpret_cast<uint8_t*>(payload_address - sizeof(BlockHeader));
        if (header_address < block_start || header_address + sizeof(BlockHeader) + size > block_end) {
            return false;
        }

        const uint64_t leading_span = static_cast<uint64_t>(header_address - block_start);
        if (leading_span == 0 || leading_span >= minimum_block_span()) {
            layout.header_address = header_address;
            layout.leading_span = leading_span;
            layout.trailing_span = static_cast<uint64_t>(block_end - (header_address + sizeof(BlockHeader) + size));
            return true;
        }

        payload_address = align_up(payload_address + alignment, alignment);
    }
}

BlockHeader* commit_allocation(BlockHeader* block, const AllocationLayout& layout, uint64_t size) {
    Arena* arena = block->arena;
    BlockHeader* old_previous = block->previous;
    BlockHeader* old_next = block->next;
    uint8_t* block_start = reinterpret_cast<uint8_t*>(block);
    uint8_t* block_end = block_start + sizeof(BlockHeader) + block->size;
    BlockHeader* allocated = reinterpret_cast<BlockHeader*>(layout.header_address);

    if (layout.leading_span != 0) {
        stamp_block(block, arena, layout.leading_span - sizeof(BlockHeader), true);
        block->previous = old_previous;
        block->next = allocated;
        if (old_previous != nullptr) {
            old_previous->next = block;
        } else {
            arena->first_block = block;
        }
        allocated->previous = block;
    } else {
        allocated = block;
        allocated->previous = old_previous;
        if (old_previous != nullptr) {
            old_previous->next = allocated;
        } else {
            arena->first_block = allocated;
        }
    }

    if (layout.trailing_span >= minimum_block_span()) {
        BlockHeader* trailing = reinterpret_cast<BlockHeader*>(layout.header_address + sizeof(BlockHeader) + size);
        stamp_block(trailing, arena, layout.trailing_span - sizeof(BlockHeader), true);
        trailing->previous = allocated;
        trailing->next = old_next;
        if (old_next != nullptr) {
            old_next->previous = trailing;
        }
        allocated->next = trailing;
        stamp_block(allocated, arena, size, false);
        return allocated;
    }

    allocated->next = old_next;
    if (old_next != nullptr) {
        old_next->previous = allocated;
    }
    stamp_block(allocated, arena, static_cast<uint64_t>(block_end - (layout.header_address + sizeof(BlockHeader))), false);
    return allocated;
}

bool acquire_arena(uint64_t minimum_payload_bytes, uint64_t alignment) {
    const uint64_t minimum_bytes = align_up(sizeof(Arena), kMinimumAlignment)
        + sizeof(BlockHeader)
        + minimum_payload_bytes
        + alignment;
    const uint64_t wanted_bytes = minimum_bytes > (kDefaultArenaPages * memory::kPageSize)
        ? minimum_bytes
        : (kDefaultArenaPages * memory::kPageSize);
    const uint64_t page_count = (wanted_bytes + memory::kPageSize - 1) / memory::kPageSize;

    memory::PageAllocation allocation = {};
    if (!memory::allocate_contiguous_pages(page_count, allocation)) {
        return false;
    }

    Arena* arena = static_cast<Arena*>(allocation.virtual_address);
    arena->allocation = allocation;
    arena->next = g_first_arena;
    arena->previous = nullptr;
    if (g_first_arena != nullptr) {
        g_first_arena->previous = arena;
    }
    g_first_arena = arena;
    ++g_arena_count;

    BlockHeader* initial = reinterpret_cast<BlockHeader*>(arena_payload_start(arena));
    stamp_block(initial, arena, arena_initial_block_size(arena), true);
    initial->previous = nullptr;
    initial->next = nullptr;
    arena->first_block = initial;
    return true;
}

bool arena_is_fully_free(const Arena* arena) {
    const BlockHeader* block = arena != nullptr ? arena->first_block : nullptr;
    return arena != nullptr
        && block != nullptr
        && block->free != 0u
        && block->previous == nullptr
        && block->next == nullptr
        && reinterpret_cast<const uint8_t*>(block) == arena_payload_start(arena)
        && block->size == arena_initial_block_size(arena);
}

void release_arena(Arena* arena) {
    if (arena == nullptr || g_arena_count <= 1 || !arena_is_fully_free(arena)) {
        return;
    }

    memory::PageAllocation allocation = arena->allocation;
    Arena* previous = arena->previous;
    Arena* next = arena->next;

    if (previous != nullptr) {
        previous->next = next;
    } else {
        g_first_arena = next;
    }
    if (next != nullptr) {
        next->previous = previous;
    }

    --g_arena_count;
    (void)memory::free_allocation(allocation);
}

BlockHeader* find_allocated_block(uint64_t size, uint64_t alignment, AllocationLayout& layout) {
    for (Arena* arena = g_first_arena; arena != nullptr; arena = arena->next) {
        for (BlockHeader* block = arena->first_block; block != nullptr; block = block->next) {
            if (compute_layout(block, size, alignment, layout)) {
                return block;
            }
        }
    }
    return nullptr;
}

uint64_t sum_block_bytes(bool include_free, bool include_used) {
    uint64_t total = 0;

    for (Arena* arena = g_first_arena; arena != nullptr; arena = arena->next) {
        for (BlockHeader* block = arena->first_block; block != nullptr; block = block->next) {
            if ((block->free != 0u && include_free) || (block->free == 0u && include_used)) {
                total += block->size;
            }
        }
    }
    return total;
}

} // namespace

namespace heap {

void initialize() {
    g_first_arena = nullptr;
    g_arena_count = 0;
    g_used_bytes = 0;
    g_ready = acquire_arena(kMinimumAlignment, kMinimumAlignment);
}

bool ready() {
    return g_ready;
}

void* allocate(size_t size, size_t alignment) {
    if (!g_ready || size == 0) {
        return nullptr;
    }

    const uint64_t normalized_alignment = normalize_alignment(alignment);
    const uint64_t normalized_size = align_up(size, kMinimumAlignment);

    for (;;) {
        AllocationLayout layout = {};
        BlockHeader* block = find_allocated_block(normalized_size, normalized_alignment, layout);
        if (block != nullptr) {
            BlockHeader* allocated = commit_allocation(block, layout, normalized_size);
            g_used_bytes += allocated->size;
            return allocated + 1;
        }

        if (!acquire_arena(normalized_size, normalized_alignment)) {
            return nullptr;
        }
    }
}

void free(void* address) {
    if (address == nullptr) {
        return;
    }

    BlockHeader* block = reinterpret_cast<BlockHeader*>(static_cast<uint8_t*>(address) - sizeof(BlockHeader));
    if (!block_is_valid(block) || block->free != 0u) {
        panic("heap: invalid free");
    }

    if (g_used_bytes >= block->size) {
        g_used_bytes -= block->size;
    } else {
        g_used_bytes = 0;
    }

    block->free = 1u;
    merge_with_next(block);
    if (block->previous != nullptr && block->previous->free != 0u) {
        block = block->previous;
        merge_with_next(block);
    }

    release_arena(block->arena);
}

uint64_t total_bytes() {
    return sum_block_bytes(true, true);
}

uint64_t used_bytes() {
    return g_used_bytes;
}

uint64_t free_bytes() {
    return sum_block_bytes(true, false);
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
