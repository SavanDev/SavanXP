#include "memory.hpp"

#include "boot/boot_info.hpp"
#include "boot_state.hpp"
#include "kernel/physical_memory.hpp"
#include "multiboot2.hpp"
#include "paging.hpp"

extern "C" uint8_t __kernel_end[];

namespace i386 {
namespace {
constexpr uint32_t kMaxRegions = 256;
boot::MemoryRegion g_regions[kMaxRegions];
uint32_t g_region_count;

bool append(uint64_t base, uint64_t end) {
    if (base >= end) return true;
    if (g_region_count == kMaxRegions) return false;
    g_regions[g_region_count++] = {base, end - base, boot::MemoryRegionType::usable};
    return true;
}

// Split usable ranges around boot-owned objects before giving them to the PMM.
// Unlike Limine, GRUB's E820 map does not reserve the loaded kernel or modules.
bool reserve(uint64_t base, uint64_t end) {
    for (uint32_t index = 0; index < g_region_count; ++index) {
        auto& region = g_regions[index];
        const uint64_t old_end = region.base + region.length;
        if (base >= old_end || end <= region.base) continue;
        const uint64_t old_base = region.base;
        region.length = base > old_base ? base - old_base : 0;
        if (end < old_end && !append(end, old_end)) return false;
    }
    return true;
}
} // namespace

bool initialize_memory(uint32_t information) {
    const auto* info = reinterpret_cast<const uint8_t*>(information);
    const uint32_t size = *reinterpret_cast<const uint32_t*>(info);
    g_region_count = 0;
    for (uint32_t offset = 8; offset + 8 <= size;) {
        const auto* tag = reinterpret_cast<const Tag*>(info + offset);
        if (tag->type == 0) break;
        if (tag->size < 8 || tag->size > size - offset) return false;
        if (tag->type == static_cast<uint32_t>(TagType::memory_map)) {
            if (tag->size < 16) return false;
            const auto* map = reinterpret_cast<const MemoryMapTag*>(tag);
            if (map->entry_size < sizeof(MemoryMapEntry)) return false;
            for (uint32_t cursor = 16; map->entry_size <= tag->size - cursor;
                 cursor += map->entry_size) {
                const auto* entry = reinterpret_cast<const MemoryMapEntry*>(info + offset + cursor);
                if (entry->type != 1 || entry->base >= paging::kDirectMapBytes) continue;
                if (entry->length > UINT64_MAX - entry->base) return false;
                uint64_t end = entry->base + entry->length;
                if (end > paging::kDirectMapBytes) end = paging::kDirectMapBytes;
                if (!append(entry->base, end)) return false;
            }
        }
        offset += (tag->size + 7u) & ~7u;
    }
    if (!reserve(0, reinterpret_cast<uintptr_t>(__kernel_end)) ||
        !reserve(information, static_cast<uint64_t>(information) + size)) return false;
    for (uint32_t offset = 8; offset + 8 <= size;) {
        const auto* tag = reinterpret_cast<const Tag*>(info + offset);
        if (tag->type == 0) break;
        if (tag->type == 3) { // Multiboot2 module start/end physical addresses.
            if (tag->size < 16) return false;
            const auto* words = reinterpret_cast<const uint32_t*>(tag);
            if (words[3] < words[2] || !reserve(words[2], words[3])) return false;
        }
        offset += (tag->size + 7u) & ~7u;
    }
    const auto& video = boot_state::state();
    if (video.framebuffer_ready && !reserve(video.framebuffer_address,
            video.framebuffer_address + static_cast<uint64_t>(video.framebuffer_pitch) *
            video.framebuffer_height)) return false;
    boot::BootInfo boot = {};
    boot.hhdm_offset = paging::kDirectMapBase;
    boot.memory_map = g_regions;
    boot.memory_map_entries = g_region_count;
    memory::initialize(boot);
    return memory::ready();
}
} // namespace i386
