#include "kernel/vmm.hpp"

#include "kernel/physical_memory.hpp"
#include "kernel/string.hpp"

namespace {

constexpr uint64_t kPageMask = 0x000ffffffffff000ULL;
constexpr uint64_t kKernelPml4Start = 256;

uint64_t g_hhdm_offset = 0;
uint64_t g_kernel_pml4_physical = 0;
bool g_ready = false;

uint64_t pml4_index(uint64_t virtual_address) {
    return (virtual_address >> 39) & 0x1ff;
}

uint64_t pdpt_index(uint64_t virtual_address) {
    return (virtual_address >> 30) & 0x1ff;
}

uint64_t pd_index(uint64_t virtual_address) {
    return (virtual_address >> 21) & 0x1ff;
}

uint64_t pt_index(uint64_t virtual_address) {
    return (virtual_address >> 12) & 0x1ff;
}

bool canonical_user_address(uint64_t virtual_address) {
    return virtual_address >= vm::kUserBase && virtual_address < kKernelPml4Start * (1ULL << 39);
}

uint64_t* page_table_for(const vm::VmSpace& space, uint64_t virtual_address, bool require_write) {
    if (!canonical_user_address(virtual_address)) {
        return nullptr;
    }

    const uint64_t pml4e = space.pml4_virtual[pml4_index(virtual_address)];
    if ((pml4e & vm::kPagePresent) == 0 || (pml4e & vm::kPageUser) == 0) {
        return nullptr;
    }

    uint64_t* pdpt = vm::physical_to_virtual(pml4e & kPageMask);
    const uint64_t pdpte = pdpt[pdpt_index(virtual_address)];
    if ((pdpte & vm::kPagePresent) == 0 || (pdpte & vm::kPageUser) == 0) {
        return nullptr;
    }

    uint64_t* pd = vm::physical_to_virtual(pdpte & kPageMask);
    const uint64_t pde = pd[pd_index(virtual_address)];
    if ((pde & vm::kPagePresent) == 0 || (pde & vm::kPageUser) == 0) {
        return nullptr;
    }

    uint64_t* pt = vm::physical_to_virtual(pde & kPageMask);
    const uint64_t pte = pt[pt_index(virtual_address)];
    if ((pte & vm::kPagePresent) == 0 || (pte & vm::kPageUser) == 0) {
        return nullptr;
    }
    if (require_write && (pte & vm::kPageWrite) == 0) {
        return nullptr;
    }

    return pt;
}

uint64_t read_cr3() {
    uint64_t value = 0;
    asm volatile("mov %%cr3, %0" : "=r"(value));
    return value;
}

uint64_t* allocate_table_page(uint64_t& physical) {
    memory::PageAllocation allocation = {};
    if (!memory::allocate_page(allocation)) {
        return nullptr;
    }

    physical = allocation.physical_address;
    auto* table = static_cast<uint64_t*>(allocation.virtual_address);
    memset(table, 0, memory::kPageSize);
    return table;
}

uint64_t* next_table(uint64_t* table, uint64_t index, uint64_t flags) {
    if ((table[index] & vm::kPagePresent) != 0) {
        return vm::physical_to_virtual(table[index] & kPageMask);
    }

    uint64_t next_physical = 0;
    uint64_t* next = allocate_table_page(next_physical);
    if (next == nullptr) {
        return nullptr;
    }

    table[index] = next_physical | flags | vm::kPagePresent | vm::kPageWrite;
    return next;
}

} // namespace

namespace vm {

void initialize(const boot::BootInfo& boot_info) {
    g_hhdm_offset = boot_info.hhdm_offset;
    g_kernel_pml4_physical = read_cr3() & kPageMask;
    g_ready = g_hhdm_offset != 0 && g_kernel_pml4_physical != 0;
}

bool ready() {
    return g_ready;
}

bool create_address_space(VmSpace& space) {
    if (!g_ready) {
        return false;
    }

    uint64_t pml4_physical = 0;
    uint64_t* pml4_virtual = allocate_table_page(pml4_physical);
    if (pml4_virtual == nullptr) {
        return false;
    }

    auto* kernel_pml4 = physical_to_virtual(g_kernel_pml4_physical);
    for (uint64_t index = kKernelPml4Start; index < 512; ++index) {
        pml4_virtual[index] = kernel_pml4[index];
    }

    space.pml4_physical = pml4_physical;
    space.pml4_virtual = pml4_virtual;
    return true;
}

bool map_page(VmSpace& space, uint64_t virtual_address, uint64_t physical_address, uint64_t flags) {
    if (!g_ready || (virtual_address & 0xfff) != 0 || (physical_address & 0xfff) != 0) {
        return false;
    }

    uint64_t* pdpt = next_table(space.pml4_virtual, pml4_index(virtual_address), flags);
    if (pdpt == nullptr) {
        return false;
    }

    uint64_t* pd = next_table(pdpt, pdpt_index(virtual_address), flags);
    if (pd == nullptr) {
        return false;
    }

    uint64_t* pt = next_table(pd, pd_index(virtual_address), flags);
    if (pt == nullptr) {
        return false;
    }

    pt[pt_index(virtual_address)] = (physical_address & kPageMask) | flags | kPagePresent;
    return true;
}

uint64_t current_pml4() {
    return g_kernel_pml4_physical;
}

uint64_t hhdm_offset() {
    return g_hhdm_offset;
}

uint64_t* physical_to_virtual(uint64_t physical_address) {
    return reinterpret_cast<uint64_t*>(physical_address + g_hhdm_offset);
}

bool is_user_range_accessible(const VmSpace& space, uint64_t virtual_address, size_t size, bool require_write) {
    if (size == 0) {
        return true;
    }

    const uint64_t last = virtual_address + static_cast<uint64_t>(size - 1);
    if (last < virtual_address) {
        return false;
    }

    uint64_t page = virtual_address & ~static_cast<uint64_t>(memory::kPageSize - 1);
    const uint64_t last_page = last & ~static_cast<uint64_t>(memory::kPageSize - 1);
    while (page <= last_page) {
        if (page_table_for(space, page, require_write) == nullptr) {
            return false;
        }
        if (page == last_page) {
            break;
        }
        page += memory::kPageSize;
    }
    return true;
}

} // namespace vm
