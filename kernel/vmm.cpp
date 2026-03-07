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

} // namespace vm
