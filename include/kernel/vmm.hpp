#pragma once

#include <stddef.h>
#include <stdint.h>

#include "boot/boot_info.hpp"

namespace vm {

constexpr uint64_t kUserBase = 0x0000000000400000ULL;
constexpr uint64_t kUserStackTop = 0x0000007000000000ULL;
constexpr uint64_t kUserStackPages = 4;

enum PageFlags : uint64_t {
    kPagePresent = 1ULL << 0,
    kPageWrite = 1ULL << 1,
    kPageUser = 1ULL << 2,
};

struct VmSpace {
    uint64_t pml4_physical;
    uint64_t* pml4_virtual;
};

void initialize(const boot::BootInfo& boot_info);
bool ready();
bool create_address_space(VmSpace& space);
void destroy_address_space(VmSpace& space);
bool map_page(VmSpace& space, uint64_t virtual_address, uint64_t physical_address, uint64_t flags);
uint64_t current_pml4();
uint64_t hhdm_offset();
uint64_t* physical_to_virtual(uint64_t physical_address);
bool is_user_range_accessible(const VmSpace& space, uint64_t virtual_address, size_t size, bool require_write);

} // namespace vm
