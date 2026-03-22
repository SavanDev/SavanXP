#pragma once

#include <stddef.h>
#include <stdint.h>

#include "boot/boot_info.hpp"

namespace object {
struct SectionObject;
}

namespace vm {

constexpr uint64_t kUserBase = 0x0000000000400000ULL;
constexpr uint64_t kUserStackTop = 0x0000007000000000ULL;
constexpr uint64_t kUserStackPages = 32;
constexpr uint64_t kSectionViewBase = 0x0000001000000000ULL;
constexpr size_t kMaxSectionViews = 16;

enum PageFlags : uint64_t {
    kPagePresent = 1ULL << 0,
    kPageWrite = 1ULL << 1,
    kPageUser = 1ULL << 2,
    kPageWriteThrough = 1ULL << 3,
    kPageCacheDisable = 1ULL << 4,
};

struct VmSpace {
    struct SectionView {
        uint64_t base_address;
        uint64_t size_bytes;
        object::SectionObject* section;
        uint32_t access_mask;
        uint8_t share_on_fork;
        uint8_t reserved0;
        uint16_t reserved1;
    };

    uint64_t pml4_physical;
    uint64_t* pml4_virtual;
    uint64_t next_section_base;
    SectionView section_views[kMaxSectionViews];
};

void initialize(const boot::BootInfo& boot_info);
bool ready();
bool create_address_space(VmSpace& space);
void destroy_address_space(VmSpace& space);
bool map_page(VmSpace& space, uint64_t virtual_address, uint64_t physical_address, uint64_t flags);
bool unmap_page(VmSpace& space, uint64_t virtual_address, uint64_t* physical_address);
bool clone_address_space(const VmSpace& source, VmSpace& destination);
bool map_section_view(VmSpace& space, object::SectionObject& section, uint32_t access_mask, uint64_t& base_address, bool share_on_fork);
bool unmap_section_view(VmSpace& space, uint64_t base_address);
bool map_kernel_mmio(uint64_t physical_base, size_t size, uint64_t flags, void** virtual_base);
uint64_t current_pml4();
uint64_t hhdm_offset();
uint64_t* physical_to_virtual(uint64_t physical_address);
bool is_user_range_accessible(const VmSpace& space, uint64_t virtual_address, size_t size, bool require_write);

} // namespace vm
