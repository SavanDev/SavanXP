#include "kernel/vmm.hpp"

#include "kernel/physical_memory.hpp"
#include "kernel/string.hpp"

namespace {

constexpr uint64_t kPageMask = 0x000ffffffffff000ULL;
constexpr uint64_t kKernelPml4Start = 256;
constexpr uint64_t kKernelPml4WindowBytes = 1ULL << 39;

uint64_t g_hhdm_offset = 0;
uint64_t g_kernel_pml4_physical = 0;
uint64_t g_kernel_mmio_base = 0;
uint64_t g_kernel_mmio_next = 0;
uint64_t g_kernel_mmio_limit = 0;
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

uint64_t align_up(uint64_t value, uint64_t alignment) {
    const uint64_t mask = alignment - 1;
    return (value + mask) & ~mask;
}

uint64_t sign_extend_48(uint64_t value) {
    if ((value & (1ULL << 47)) != 0) {
        value |= 0xffff000000000000ULL;
    }
    return value;
}

uint64_t pml4_base(uint64_t index) {
    return sign_extend_48(index << 39);
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

void destroy_table(uint64_t table_physical, uint8_t level) {
    uint64_t* table = vm::physical_to_virtual(table_physical);
    if (table == nullptr) {
        return;
    }

    for (size_t index = 0; index < 512; ++index) {
        const uint64_t entry = table[index];
        if ((entry & vm::kPagePresent) == 0) {
            continue;
        }

        const uint64_t child_physical = entry & kPageMask;
        if (level == 1) {
            (void)memory::free_pages(child_physical, 1);
            continue;
        }

        destroy_table(child_physical, static_cast<uint8_t>(level - 1));
        (void)memory::free_pages(child_physical, 1);
    }
}

uint64_t* kernel_pml4() {
    return vm::physical_to_virtual(g_kernel_pml4_physical);
}

void invalidate_page(uint64_t virtual_address) {
    asm volatile("invlpg (%0)" : : "r"(virtual_address) : "memory");
}

bool reserve_kernel_mmio_window() {
    uint64_t* pml4 = kernel_pml4();
    if (pml4 == nullptr) {
        return false;
    }

    for (uint64_t index = 510; index >= kKernelPml4Start; --index) {
        if (pml4[index] == 0) {
            g_kernel_mmio_base = pml4_base(index);
            g_kernel_mmio_next = g_kernel_mmio_base;
            g_kernel_mmio_limit = g_kernel_mmio_base + kKernelPml4WindowBytes;
            return true;
        }
        if (index == kKernelPml4Start) {
            break;
        }
    }

    return false;
}

bool map_kernel_page(uint64_t virtual_address, uint64_t physical_address, uint64_t flags) {
    if (!g_ready || (virtual_address & (memory::kPageSize - 1)) != 0 || (physical_address & (memory::kPageSize - 1)) != 0) {
        return false;
    }

    uint64_t* pml4 = kernel_pml4();
    if (pml4 == nullptr) {
        return false;
    }

    uint64_t* pdpt = next_table(pml4, pml4_index(virtual_address), 0);
    if (pdpt == nullptr) {
        return false;
    }

    uint64_t* pd = next_table(pdpt, pdpt_index(virtual_address), 0);
    if (pd == nullptr) {
        return false;
    }

    uint64_t* pt = next_table(pd, pd_index(virtual_address), 0);
    if (pt == nullptr) {
        return false;
    }

    const uint64_t entry_index = pt_index(virtual_address);
    if ((pt[entry_index] & vm::kPagePresent) != 0) {
        return false;
    }

    pt[entry_index] = (physical_address & kPageMask) | flags | vm::kPagePresent;
    invalidate_page(virtual_address);
    return true;
}

} // namespace

namespace vm {

void initialize(const boot::BootInfo& boot_info) {
    g_hhdm_offset = boot_info.hhdm_offset;
    g_kernel_pml4_physical = read_cr3() & kPageMask;
    g_kernel_mmio_base = 0;
    g_kernel_mmio_next = 0;
    g_kernel_mmio_limit = 0;
    g_ready = g_hhdm_offset != 0 && g_kernel_pml4_physical != 0;
    if (g_ready) {
        (void)reserve_kernel_mmio_window();
    }
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

void destroy_address_space(VmSpace& space) {
    if (!g_ready || space.pml4_physical == 0 || space.pml4_virtual == nullptr) {
        return;
    }

    for (uint64_t index = 0; index < kKernelPml4Start; ++index) {
        const uint64_t entry = space.pml4_virtual[index];
        if ((entry & kPagePresent) == 0) {
            continue;
        }

        destroy_table(entry & kPageMask, 3);
        (void)memory::free_pages(entry & kPageMask, 1);
        space.pml4_virtual[index] = 0;
    }

    (void)memory::free_pages(space.pml4_physical, 1);
    space.pml4_physical = 0;
    space.pml4_virtual = nullptr;
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

bool clone_address_space(const VmSpace& source, VmSpace& destination) {
    if (!g_ready || source.pml4_physical == 0 || source.pml4_virtual == nullptr) {
        return false;
    }

    VmSpace clone = {};
    if (!create_address_space(clone)) {
        return false;
    }

    for (uint64_t pml4_slot = 0; pml4_slot < kKernelPml4Start; ++pml4_slot) {
        const uint64_t pml4e = source.pml4_virtual[pml4_slot];
        if ((pml4e & kPagePresent) == 0 || (pml4e & kPageUser) == 0) {
            continue;
        }

        uint64_t* pdpt = physical_to_virtual(pml4e & kPageMask);
        if (pdpt == nullptr) {
            destroy_address_space(clone);
            return false;
        }

        for (uint64_t pdpt_slot = 0; pdpt_slot < 512; ++pdpt_slot) {
            const uint64_t pdpte = pdpt[pdpt_slot];
            if ((pdpte & kPagePresent) == 0 || (pdpte & kPageUser) == 0) {
                continue;
            }

            uint64_t* pd = physical_to_virtual(pdpte & kPageMask);
            if (pd == nullptr) {
                destroy_address_space(clone);
                return false;
            }

            for (uint64_t pd_slot = 0; pd_slot < 512; ++pd_slot) {
                const uint64_t pde = pd[pd_slot];
                if ((pde & kPagePresent) == 0 || (pde & kPageUser) == 0) {
                    continue;
                }

                uint64_t* pt = physical_to_virtual(pde & kPageMask);
                if (pt == nullptr) {
                    destroy_address_space(clone);
                    return false;
                }

                for (uint64_t pt_slot = 0; pt_slot < 512; ++pt_slot) {
                    const uint64_t pte = pt[pt_slot];
                    if ((pte & kPagePresent) == 0 || (pte & kPageUser) == 0) {
                        continue;
                    }

                    memory::PageAllocation page = {};
                    if (!memory::allocate_page(page)) {
                        destroy_address_space(clone);
                        return false;
                    }

                    const uint64_t source_physical = pte & kPageMask;
                    uint64_t flags = kPageUser;
                    if ((pte & kPageWrite) != 0) {
                        flags |= kPageWrite;
                    }

                    memcpy(page.virtual_address, physical_to_virtual(source_physical), memory::kPageSize);

                    const uint64_t virtual_address =
                        sign_extend_48((pml4_slot << 39) | (pdpt_slot << 30) | (pd_slot << 21) | (pt_slot << 12));
                    if (!map_page(clone, virtual_address, page.physical_address, flags)) {
                        (void)memory::free_allocation(page);
                        destroy_address_space(clone);
                        return false;
                    }
                }
            }
        }
    }

    destination = clone;
    return true;
}

bool map_kernel_mmio(uint64_t physical_base, size_t size, uint64_t flags, void** virtual_base) {
    if (!g_ready || g_kernel_mmio_base == 0 || virtual_base == nullptr || size == 0) {
        return false;
    }

    const uint64_t aligned_physical = physical_base & kPageMask;
    const uint64_t intra_page_offset = physical_base - aligned_physical;
    const uint64_t total_bytes = align_up(intra_page_offset + static_cast<uint64_t>(size), memory::kPageSize);
    const uint64_t mapping_base = align_up(g_kernel_mmio_next, memory::kPageSize);

    if (mapping_base < g_kernel_mmio_base ||
        mapping_base + total_bytes < mapping_base ||
        mapping_base + total_bytes > g_kernel_mmio_limit) {
        return false;
    }

    for (uint64_t offset = 0; offset < total_bytes; offset += memory::kPageSize) {
        if (!map_kernel_page(
                mapping_base + offset,
                aligned_physical + offset,
                (flags | vm::kPageWrite | vm::kPageCacheDisable) & ~vm::kPageUser)) {
            return false;
        }
    }

    g_kernel_mmio_next = mapping_base + total_bytes;
    *virtual_base = reinterpret_cast<void*>(mapping_base + intra_page_offset);
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
