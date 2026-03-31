#include "kernel/vmm.hpp"

#include "kernel/object.hpp"
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

bool canonical_user_range(uint64_t virtual_address, uint64_t size) {
    if (size == 0) {
        return false;
    }

    const uint64_t last = virtual_address + size - 1;
    if (last < virtual_address) {
        return false;
    }

    const uint64_t stack_bottom = vm::kUserStackTop - (vm::kUserStackPages * memory::kPageSize);
    return canonical_user_address(virtual_address) &&
        canonical_user_address(last) &&
        last < stack_bottom;
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

const vm::VmSpace::SectionView* section_view_for_address(const vm::VmSpace& space, uint64_t virtual_address) {
    for (const vm::VmSpace::SectionView& view : space.section_views) {
        if (view.section == nullptr) {
            continue;
        }
        if (virtual_address >= view.base_address && virtual_address < (view.base_address + view.size_bytes)) {
            return &view;
        }
    }
    return nullptr;
}

vm::VmSpace::SectionView* find_section_view(vm::VmSpace& space, uint64_t base_address) {
    for (vm::VmSpace::SectionView& view : space.section_views) {
        if (view.section != nullptr && view.base_address == base_address) {
            return &view;
        }
    }
    return nullptr;
}

vm::VmSpace::SectionView* find_free_section_view(vm::VmSpace& space) {
    for (vm::VmSpace::SectionView& view : space.section_views) {
        if (view.section == nullptr) {
            return &view;
        }
    }
    return nullptr;
}

bool user_range_is_free(const vm::VmSpace& space, uint64_t virtual_address, uint64_t size) {
    if (!canonical_user_range(virtual_address, size)) {
        return false;
    }

    const uint64_t last = virtual_address + size - 1;
    const uint64_t last_page = last & ~(memory::kPageSize - 1);
    for (uint64_t page = virtual_address; page <= last_page; page += memory::kPageSize) {
        if (page_table_for(space, page, false) != nullptr) {
            return false;
        }
        if (page == last_page) {
            break;
        }
    }

    for (const vm::VmSpace::SectionView& view : space.section_views) {
        if (view.section == nullptr) {
            continue;
        }
        const uint64_t view_end = view.base_address + view.size_bytes;
        const uint64_t range_end = virtual_address + size;
        if (virtual_address < view_end && range_end > view.base_address) {
            return false;
        }
    }

    return true;
}

bool choose_section_view_base(vm::VmSpace& space, uint64_t size, uint64_t& base_address) {
    const uint64_t aligned_size = align_up(size, memory::kPageSize);
    const uint64_t start = align_up(space.next_section_base != 0 ? space.next_section_base : vm::kSectionViewBase, memory::kPageSize);
    const uint64_t stack_bottom = vm::kUserStackTop - (vm::kUserStackPages * memory::kPageSize);

    for (uint64_t candidate = start; candidate < stack_bottom; candidate += memory::kPageSize) {
        if (candidate + aligned_size < candidate || candidate + aligned_size > stack_bottom) {
            break;
        }
        if (!user_range_is_free(space, candidate, aligned_size)) {
            continue;
        }
        base_address = candidate;
        space.next_section_base = candidate + aligned_size;
        return true;
    }

    for (uint64_t candidate = vm::kSectionViewBase; candidate < start; candidate += memory::kPageSize) {
        if (candidate + aligned_size < candidate || candidate + aligned_size > stack_bottom) {
            break;
        }
        if (!user_range_is_free(space, candidate, aligned_size)) {
            continue;
        }
        base_address = candidate;
        space.next_section_base = candidate + aligned_size;
        return true;
    }

    return false;
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

bool unmap_kernel_page(uint64_t virtual_address) {
    if (!g_ready || (virtual_address & (memory::kPageSize - 1)) != 0) {
        return false;
    }

    uint64_t* pml4 = kernel_pml4();
    if (pml4 == nullptr) {
        return false;
    }

    const uint64_t pml4e = pml4[pml4_index(virtual_address)];
    if ((pml4e & vm::kPagePresent) == 0) {
        return false;
    }

    uint64_t* pdpt = vm::physical_to_virtual(pml4e & kPageMask);
    if (pdpt == nullptr) {
        return false;
    }
    const uint64_t pdpte = pdpt[pdpt_index(virtual_address)];
    if ((pdpte & vm::kPagePresent) == 0) {
        return false;
    }

    uint64_t* pd = vm::physical_to_virtual(pdpte & kPageMask);
    if (pd == nullptr) {
        return false;
    }
    const uint64_t pde = pd[pd_index(virtual_address)];
    if ((pde & vm::kPagePresent) == 0) {
        return false;
    }

    uint64_t* pt = vm::physical_to_virtual(pde & kPageMask);
    if (pt == nullptr) {
        return false;
    }

    const uint64_t entry_index = pt_index(virtual_address);
    if ((pt[entry_index] & vm::kPagePresent) == 0) {
        return false;
    }

    pt[entry_index] = 0;
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
    space.next_section_base = kSectionViewBase;
    memset(space.section_views, 0, sizeof(space.section_views));
    return true;
}

void destroy_address_space(VmSpace& space) {
    if (!g_ready || space.pml4_physical == 0 || space.pml4_virtual == nullptr) {
        return;
    }

    for (size_t index = 0; index < kMaxSectionViews; ++index) {
        if (space.section_views[index].section != nullptr) {
            (void)unmap_section_view(space, space.section_views[index].base_address);
        }
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
    space.next_section_base = 0;
    memset(space.section_views, 0, sizeof(space.section_views));
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

    const uint64_t entry_index = pt_index(virtual_address);
    if ((pt[entry_index] & kPagePresent) != 0) {
        return false;
    }

    pt[entry_index] = (physical_address & kPageMask) | flags | kPagePresent;
    if ((read_cr3() & kPageMask) == space.pml4_physical) {
        invalidate_page(virtual_address);
    }
    return true;
}

bool unmap_page(VmSpace& space, uint64_t virtual_address, uint64_t* physical_address) {
    if (!g_ready || (virtual_address & 0xfff) != 0) {
        return false;
    }

    uint64_t* pt = page_table_for(space, virtual_address, false);
    if (pt == nullptr) {
        return false;
    }

    const uint64_t entry_index = pt_index(virtual_address);
    const uint64_t entry = pt[entry_index];
    if ((entry & kPagePresent) == 0) {
        return false;
    }

    if (physical_address != nullptr) {
        *physical_address = entry & kPageMask;
    }
    pt[entry_index] = 0;
    if ((read_cr3() & kPageMask) == space.pml4_physical) {
        invalidate_page(virtual_address);
    }
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
    clone.next_section_base = source.next_section_base;

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
                    if (section_view_for_address(source, virtual_address) != nullptr) {
                        continue;
                    }
                    if (!map_page(clone, virtual_address, page.physical_address, flags)) {
                        (void)memory::free_allocation(page);
                        destroy_address_space(clone);
                        return false;
                    }
                }
            }
        }
    }

    for (const VmSpace::SectionView& view : source.section_views) {
        if (view.section == nullptr) {
            continue;
        }

        uint64_t base_address = view.base_address;
        if (view.share_on_fork != 0) {
            if (!map_section_view(clone, *view.section, view.access_mask, base_address, true)) {
                destroy_address_space(clone);
                return false;
            }
            continue;
        }

        object::SectionObject* cloned_section = object::clone_section(*view.section);
        if (cloned_section == nullptr) {
            destroy_address_space(clone);
            return false;
        }

        if (!map_section_view(clone, *cloned_section, view.access_mask, base_address, false)) {
            object::Header* header = &cloned_section->header;
            object::release(header);
            destroy_address_space(clone);
            return false;
        }

        object::Header* header = &cloned_section->header;
        object::release(header);
    }

    destination = clone;
    return true;
}

bool map_section_view(VmSpace& space, object::SectionObject& section, uint32_t access_mask, uint64_t& base_address, bool share_on_fork) {
    if (!g_ready || !section.in_use || section.page_count == 0 || section.physical_pages == nullptr) {
        return false;
    }

    if ((access_mask & section.access_mask) != access_mask) {
        return false;
    }

    const uint64_t view_size = section.size_bytes;
    if (view_size == 0) {
        return false;
    }

    if (base_address == 0) {
        if (!choose_section_view_base(space, view_size, base_address)) {
            return false;
        }
    } else if (!user_range_is_free(space, base_address, view_size)) {
        return false;
    }

    VmSpace::SectionView* view = find_free_section_view(space);
    if (view == nullptr) {
        return false;
    }

    uint64_t page_flags = kPageUser;
    if ((access_mask & object::access_write) != 0) {
        page_flags |= kPageWrite;
    }

    for (uint64_t page_index = 0; page_index < section.page_count; ++page_index) {
        const uint64_t virtual_address = base_address + (page_index * memory::kPageSize);
        if (!map_page(space, virtual_address, section.physical_pages[page_index], page_flags)) {
            for (uint64_t rollback = 0; rollback < page_index; ++rollback) {
                (void)unmap_page(space, base_address + (rollback * memory::kPageSize), nullptr);
            }
            return false;
        }
    }

    object::retain(&section.header);
    view->base_address = base_address;
    view->size_bytes = view_size;
    view->section = &section;
    view->access_mask = access_mask;
    view->share_on_fork = share_on_fork ? 1u : 0u;
    view->reserved0 = 0;
    view->reserved1 = 0;
    if (base_address + view_size > space.next_section_base) {
        space.next_section_base = base_address + view_size;
    }
    return true;
}

bool unmap_section_view(VmSpace& space, uint64_t base_address) {
    VmSpace::SectionView* view = find_section_view(space, base_address);
    if (view == nullptr) {
        return false;
    }

    const uint64_t page_count = view->size_bytes / memory::kPageSize;
    for (uint64_t page_index = 0; page_index < page_count; ++page_index) {
        (void)unmap_page(space, view->base_address + (page_index * memory::kPageSize), nullptr);
    }

    object::Header* header = &view->section->header;
    object::release(header);
    memset(view, 0, sizeof(*view));
    return true;
}

bool map_kernel_pages(const uint64_t* physical_pages, uint64_t page_count, uint64_t flags, void** virtual_base) {
    if (!g_ready || g_kernel_mmio_base == 0 || virtual_base == nullptr || physical_pages == nullptr || page_count == 0) {
        return false;
    }

    const uint64_t total_bytes = align_up(page_count * memory::kPageSize, memory::kPageSize);
    const uint64_t mapping_base = align_up(g_kernel_mmio_next, memory::kPageSize);
    if (mapping_base < g_kernel_mmio_base ||
        mapping_base + total_bytes < mapping_base ||
        mapping_base + total_bytes > g_kernel_mmio_limit) {
        return false;
    }

    uint64_t mapped_pages = 0;
    for (uint64_t page_index = 0; page_index < page_count; ++page_index) {
        if ((physical_pages[page_index] & (memory::kPageSize - 1)) != 0 ||
            !map_kernel_page(
                mapping_base + (page_index * memory::kPageSize),
                physical_pages[page_index],
                (flags | vm::kPageWrite) & ~vm::kPageUser)) {
            for (uint64_t rollback = 0; rollback < mapped_pages; ++rollback) {
                (void)unmap_kernel_page(mapping_base + (rollback * memory::kPageSize));
            }
            return false;
        }
        mapped_pages += 1;
    }

    g_kernel_mmio_next = mapping_base + total_bytes;
    *virtual_base = reinterpret_cast<void*>(mapping_base);
    return true;
}

bool unmap_kernel_pages(void* virtual_base, uint64_t page_count) {
    if (!g_ready || virtual_base == nullptr || page_count == 0) {
        return false;
    }

    const uint64_t mapping_base = reinterpret_cast<uint64_t>(virtual_base);
    if ((mapping_base & (memory::kPageSize - 1)) != 0) {
        return false;
    }

    bool unmapped_any = false;
    for (uint64_t page_index = 0; page_index < page_count; ++page_index) {
        unmapped_any = unmap_kernel_page(mapping_base + (page_index * memory::kPageSize)) || unmapped_any;
    }
    return unmapped_any;
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

    uint64_t mapped_bytes = 0;
    for (uint64_t offset = 0; offset < total_bytes; offset += memory::kPageSize) {
        if (!map_kernel_page(
                mapping_base + offset,
                aligned_physical + offset,
                (flags | vm::kPageWrite | vm::kPageCacheDisable) & ~vm::kPageUser)) {
            for (uint64_t rollback = 0; rollback < mapped_bytes; rollback += memory::kPageSize) {
                (void)unmap_kernel_page(mapping_base + rollback);
            }
            return false;
        }
        mapped_bytes += memory::kPageSize;
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
