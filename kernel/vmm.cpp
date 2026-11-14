#include "kernel/vmm.hpp"

#include "arch/cpu.hpp"
#include "kernel/heap.hpp"
#include "kernel/object.hpp"
#include "kernel/panic.hpp"
#include "kernel/physical_memory.hpp"
#include "kernel/region_list.hpp"
#include "kernel/smp.hpp"
#include "kernel/string.hpp"

/* vmm.hpp no puede incluir physical_memory.hpp (la dependencia va al reves),
 * asi que repite el tamano de pagina. Aca se comprueba que no se separen. */
static_assert(vm::kPageSizeBytes == memory::kPageSize,
              "vm::kPageSizeBytes quedo desalineado de memory::kPageSize");

namespace {

using arch::invalidate_page;
using arch::read_page_table_root;

constexpr uint64_t kPageMask = 0x000ffffffffff000ULL;
constexpr uint64_t kKernelPml4Start = 256;
constexpr uint64_t kKernelPml4WindowBytes = 1ULL << 39;
constexpr uint64_t kSectionAslrWindowBytes = 16ULL << 30;

uint64_t g_hhdm_offset = 0;
uint64_t g_kernel_pml4_physical = 0;
uint64_t g_kernel_mmio_base = 0;
uint64_t g_kernel_mmio_next = 0;
uint64_t g_kernel_mmio_limit = 0;
bool g_ready = false;

// Cuantas veces se desmapeo una pagina del kernel. Sube con el lock del kernel
// tomado. Ver sync_kernel_tlb() en vmm.hpp.
uint64_t g_kernel_tlb_generation = 0;
// La generacion con la que quedo al dia la TLB de cada core.
uint64_t g_kernel_tlb_synced[arch::kMaxCpus] = {};

// User-half shootdown (see vmm.hpp). All of it changes with the kernel lock
// held, and every core reads it with the lock held too.
static_assert(arch::kMaxCpus <= 32, "deferred_cpus is a 32-bit mask");
// The user address space each core has in CR3 (its PML4), 0 for none yet.
uint64_t g_loaded_pml4[arch::kMaxCpus] = {};
// Shootdown numbers: the last one that asked each core to flush, and the last
// one each core has flushed for. Equal means up to date.
uint64_t g_shootdown_sequence = 0;
uint64_t g_flush_requested[arch::kMaxCpus] = {};
uint64_t g_flush_done[arch::kMaxCpus] = {};
// Other cores holding a space this batch of unmaps touched. unmap_page adds
// to it; finish_shootdown() asks them to flush and clears it.
uint32_t g_stale_cpus = 0;
// Section references that wait for g_flush_done to catch up.
object::SectionObject* g_deferred_views = nullptr;

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
    return virtual_address >= vm::kUserFloor && virtual_address < kKernelPml4Start * (1ULL << 39);
}

bool canonical_user_range(uint64_t virtual_address, uint64_t size) {
    if (size == 0) {
        return false;
    }

    const uint64_t last = virtual_address + size - 1;
    if (last < virtual_address) {
        return false;
    }

    return canonical_user_address(virtual_address) &&
        canonical_user_address(last) &&
        last < vm::kUserStackGuardBottom;
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

    // A region's untouched pages have no table entry yet, and are taken all
    // the same.
    if (space.regions != nullptr && region::overlaps(*space.regions, virtual_address, virtual_address + size)) {
        return false;
    }

    return true;
}

bool choose_section_view_base(vm::VmSpace& space, uint64_t size, uint64_t& base_address) {
    const uint64_t aligned_size = align_up(size, memory::kPageSize);
    const uint64_t start = align_up(space.next_section_base != 0 ? space.next_section_base : vm::kSectionViewBase, memory::kPageSize);
    const uint64_t stack_bottom = vm::kUserStackGuardBottom;

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

bool reserve_kernel_mmio_window() {
    uint64_t* pml4 = kernel_pml4();
    if (pml4 == nullptr) {
        return false;
    }

    for (uint64_t index = 510; index >= kKernelPml4Start; --index) {
        if (pml4[index] == 0) {
            uint64_t pdpt_physical = 0;
            if (allocate_table_page(pdpt_physical) == nullptr) {
                return false;
            }
            pml4[index] = pdpt_physical | vm::kPagePresent | vm::kPageWrite;
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

    // Los demas cores se enteran al tomar el lock. Este ya invalido la pagina:
    // si estaba al dia, sigue al dia.
    const uint32_t cpu = arch::cpu_index();
    const bool was_synced = g_kernel_tlb_synced[cpu] == g_kernel_tlb_generation;
    g_kernel_tlb_generation += 1;
    if (was_synced) {
        g_kernel_tlb_synced[cpu] = g_kernel_tlb_generation;
    }
    return true;
}

// A user page of `space` stopped being mapped, or lost permissions: this core
// invalidates it if it holds the space, and every other core holding it is
// noted for finish_shootdown().
void note_user_unmap(const vm::VmSpace& space, uint64_t virtual_address) {
    const uint32_t self = arch::cpu_index();
    if ((read_page_table_root() & kPageMask) == space.pml4_physical) {
        invalidate_page(virtual_address);
    }
    for (uint32_t cpu = 0; cpu < arch::kMaxCpus; ++cpu) {
        if (cpu != self && g_loaded_pml4[cpu] == space.pml4_physical) {
            g_stale_cpus |= 1u << cpu;
        }
    }
}

bool flushed_since(uint32_t cpus, uint64_t sequence) {
    for (uint32_t cpu = 0; cpu < arch::kMaxCpus; ++cpu) {
        if ((cpus & (1u << cpu)) != 0 && g_flush_done[cpu] < sequence) {
            return false;
        }
    }
    return true;
}

// Ends a batch of user unmaps. The cores noted stale get a new shootdown
// number and a reschedule IPI; nobody waits for them. Returns the number, or
// 0 when no other core held the space, which also leaves *cpus at 0.
uint64_t finish_shootdown(uint32_t& cpus) {
    cpus = g_stale_cpus;
    g_stale_cpus = 0;
    if (cpus == 0) {
        return 0;
    }
    g_shootdown_sequence += 1;
    for (uint32_t cpu = 0; cpu < arch::kMaxCpus; ++cpu) {
        if ((cpus & (1u << cpu)) != 0) {
            g_flush_requested[cpu] = g_shootdown_sequence;
            smp::kick(cpu);
        }
    }
    return g_shootdown_sequence;
}

// Gives up one reference to `section` whose pages were just unmapped. Released
// now when no other core can reach them; otherwise queued until the cores of
// this shootdown have flushed. A section already queued waits for the newer
// shootdown too, which is later than the one it waited for.
void release_after_shootdown(object::SectionObject& section) {
    uint32_t cpus = 0;
    const uint64_t sequence = finish_shootdown(cpus);
    if (sequence == 0) {
        object::Header* header = &section.header;
        object::release(header);
        return;
    }
    if (section.deferred_refs == 0) {
        section.deferred_next = g_deferred_views;
        g_deferred_views = &section;
    }
    section.deferred_refs += 1;
    section.deferred_cpus |= cpus;
    section.deferred_sequence = sequence;
}

// --- Anonymous regions (vm::map_region and the rest) -------------------------

// Raw pages an unmap freed while another core may still reach them through
// its TLB. reclaim_deferred_views() frees them once those cores have flushed,
// as it does the section references above. The record is on the kernel heap,
// never in the pages themselves: a stale translation could still write there.
constexpr uint32_t kDeferredPagesPerRecord = 254;

struct DeferredPages {
    DeferredPages* next;
    uint64_t sequence;
    uint32_t cpus;
    uint32_t count;
    uint64_t pages[kDeferredPagesPerRecord];
};

DeferredPages* g_deferred_pages = nullptr;

constexpr uint32_t kInitialRegionCapacity = 16;

const region::List kNoRegions = {nullptr, 0, 0};

const region::List& regions_of(const vm::VmSpace& space) {
    return space.regions != nullptr ? *space.regions : kNoRegions;
}

uint64_t next_boundary(uint64_t address, uint64_t span) {
    const uint64_t next = (address & ~(span - 1)) + span;
    return next > address ? next : ~0ULL;
}

// The first page of [start, end) that has a table entry in `space`, or `end`.
// An absent table skips its whole span, so a large empty range costs little.
uint64_t next_mapped_page(const vm::VmSpace& space, uint64_t start, uint64_t end) {
    uint64_t address = start & ~(memory::kPageSize - 1);
    while (address < end) {
        const uint64_t pml4e = space.pml4_virtual[pml4_index(address)];
        if ((pml4e & vm::kPagePresent) == 0) {
            address = next_boundary(address, 1ULL << 39);
            continue;
        }
        const uint64_t* pdpt = vm::physical_to_virtual(pml4e & kPageMask);
        const uint64_t pdpte = pdpt[pdpt_index(address)];
        if ((pdpte & vm::kPagePresent) == 0) {
            address = next_boundary(address, 1ULL << 30);
            continue;
        }
        const uint64_t* pd = vm::physical_to_virtual(pdpte & kPageMask);
        const uint64_t pde = pd[pd_index(address)];
        if ((pde & vm::kPagePresent) == 0) {
            address = next_boundary(address, 1ULL << 21);
            continue;
        }
        const uint64_t* pt = vm::physical_to_virtual(pde & kPageMask);
        if ((pt[pt_index(address)] & vm::kPagePresent) != 0) {
            return address;
        }
        address += memory::kPageSize;
    }
    return end;
}

// The table that holds the entry for `address`, made with its parents when
// missing. nullptr when a table page could not be allocated.
uint64_t* leaf_table(vm::VmSpace& space, uint64_t address) {
    uint64_t* pdpt = next_table(space.pml4_virtual, pml4_index(address), vm::kPageUser);
    if (pdpt == nullptr) {
        return nullptr;
    }
    uint64_t* pd = next_table(pdpt, pdpt_index(address), vm::kPageUser);
    if (pd == nullptr) {
        return nullptr;
    }
    return next_table(pd, pd_index(address), vm::kPageUser);
}

bool loaded_on_another_core(const vm::VmSpace& space) {
    const uint32_t self = arch::cpu_index();
    for (uint32_t cpu = 0; cpu < arch::kMaxCpus; ++cpu) {
        if (cpu != self && g_loaded_pml4[cpu] == space.pml4_physical) {
            return true;
        }
    }
    return false;
}

void free_deferred_record(DeferredPages* record) {
    for (uint32_t index = 0; index < record->count; ++index) {
        (void)memory::free_pages(record->pages[index], 1);
    }
    heap::free(record);
}

// Unmaps every page of [start, end) that has an entry and frees it, or, when
// another core holds the space, queues it until that core flushes. False,
// with nothing changed, when the queue needs a record the heap cannot give.
bool release_mapped_pages(vm::VmSpace& space, uint64_t start, uint64_t end) {
    uint64_t count = 0;
    for (uint64_t page = next_mapped_page(space, start, end); page < end;
         page = next_mapped_page(space, page + memory::kPageSize, end)) {
        ++count;
    }
    if (count == 0) {
        return true;
    }

    DeferredPages* records = nullptr;
    if (loaded_on_another_core(space)) {
        for (uint64_t needed = (count + kDeferredPagesPerRecord - 1) / kDeferredPagesPerRecord; needed > 0;
             --needed) {
            auto* record = static_cast<DeferredPages*>(heap::allocate(sizeof(DeferredPages)));
            if (record == nullptr) {
                while (records != nullptr) {
                    DeferredPages* next = records->next;
                    heap::free(records);
                    records = next;
                }
                return false;
            }
            record->next = records;
            record->sequence = 0;
            record->cpus = 0;
            record->count = 0;
            records = record;
        }
    }

    DeferredPages* filling = records;
    for (uint64_t page = next_mapped_page(space, start, end); page < end;
         page = next_mapped_page(space, page + memory::kPageSize, end)) {
        uint64_t physical = 0;
        (void)vm::unmap_page(space, page, &physical);
        if (filling == nullptr) {
            // No other core holds the space: this one already dropped the
            // translation, or never had it.
            (void)memory::free_pages(physical, 1);
            continue;
        }
        if (filling->count == kDeferredPagesPerRecord) {
            filling = filling->next;
        }
        filling->pages[filling->count++] = physical;
    }

    uint32_t cpus = 0;
    const uint64_t sequence = finish_shootdown(cpus);
    while (records != nullptr) {
        DeferredPages* next = records->next;
        if (sequence == 0) {
            free_deferred_record(records);
        } else {
            records->sequence = sequence;
            records->cpus = cpus;
            records->next = g_deferred_pages;
            g_deferred_pages = records;
        }
        records = next;
    }
    return true;
}

// Room in the space's list for `extra` more entries, making the list on its
// first use. False when the heap has none or the list would pass kMaxRegions.
bool reserve_regions(vm::VmSpace& space, uint32_t extra) {
    region::List* list = space.regions;
    if (list == nullptr) {
        list = static_cast<region::List*>(heap::allocate(sizeof(region::List)));
        if (list == nullptr) {
            return false;
        }
        *list = region::List{nullptr, 0, 0};
        space.regions = list;
    }
    const uint64_t needed = static_cast<uint64_t>(list->count) + extra;
    if (needed <= list->capacity) {
        return true;
    }
    if (needed > vm::kMaxRegions) {
        return false;
    }
    uint64_t capacity = list->capacity == 0 ? kInitialRegionCapacity : list->capacity * 2ULL;
    while (capacity < needed) {
        capacity *= 2;
    }
    if (capacity > vm::kMaxRegions) {
        capacity = vm::kMaxRegions;
    }
    auto* entries = static_cast<region::Region*>(heap::allocate(capacity * sizeof(region::Region)));
    if (entries == nullptr) {
        return false;
    }
    if (list->count != 0) {
        memcpy(entries, list->entries, list->count * sizeof(region::Region));
    }
    if (list->entries != nullptr) {
        heap::free(list->entries);
    }
    list->entries = entries;
    list->capacity = static_cast<uint32_t>(capacity);
    return true;
}

void free_regions(vm::VmSpace& space) {
    if (space.regions == nullptr) {
        return;
    }
    if (space.regions->entries != nullptr) {
        heap::free(space.regions->entries);
    }
    heap::free(space.regions);
    space.regions = nullptr;
}

// What [start, end) holds besides regions: a section view, the stack's
// reserved range, or a page with an entry that no region covers (the image's,
// the stack's). 0 when nothing; otherwise an address past the first one, for
// region::find_gap to resume from.
uint64_t other_mapping(void* context, uint64_t start, uint64_t end) {
    const vm::VmSpace& space = *static_cast<const vm::VmSpace*>(context);
    for (const vm::VmSpace::SectionView& view : space.section_views) {
        if (view.section != nullptr && start < view.base_address + view.size_bytes &&
            end > view.base_address) {
            return view.base_address + view.size_bytes;
        }
    }
    if (end > vm::kUserStackGuardBottom && start < vm::kUserStackTop) {
        return vm::kUserStackTop;
    }
    const region::List& list = regions_of(space);
    for (uint64_t page = next_mapped_page(space, start, end); page < end;
         page = next_mapped_page(space, page + memory::kPageSize, end)) {
        if (region::find(list, page) < 0) {
            return page + memory::kPageSize;
        }
    }
    return 0;
}

bool region_overlaps(const vm::VmSpace& space, uint64_t start, uint64_t end) {
    return space.regions != nullptr && region::overlaps(*space.regions, start, end);
}

// A page-aligned range of user addresses below the stack's guard page.
bool region_range(uint64_t start, uint64_t size) {
    return size != 0 && (start & (memory::kPageSize - 1)) == 0 && (size & (memory::kPageSize - 1)) == 0 &&
           canonical_user_range(start, size);
}

bool valid_region_access(uint8_t access) {
    const uint8_t known = region::kRead | region::kWrite | region::kExecute;
    return (access & ~known) == 0 &&
           !((access & region::kWrite) != 0 && (access & region::kExecute) != 0);
}

uint64_t region_page_flags(uint8_t access) {
    uint64_t flags = vm::kPageUser;
    if ((access & region::kWrite) != 0) {
        flags |= vm::kPageWrite;
    }
    if ((access & region::kExecute) == 0) {
        flags |= vm::kPageNoExecute;
    }
    return flags;
}

// Moves the entry of `from`, which is mapped, to `to`, whose table exists:
// the page changes address and keeps its contents and its access.
void move_page(vm::VmSpace& space, uint64_t from, uint64_t to) {
    uint64_t* from_table = page_table_for(space, from, false);
    uint64_t* to_table = leaf_table(space, to);
    const uint64_t entry = from_table[pt_index(from)];
    from_table[pt_index(from)] = 0;
    note_user_unmap(space, from);
    to_table[pt_index(to)] = entry;
}

} // namespace

namespace vm {

/* Motivo del ultimo fallo de map_section_view, para poder distinguirlo del
 * ENOMEM generico que devuelve: 1 seccion invalida, 2 grant, 3 sin lugar libre,
 * 4 rango ocupado, 5 sin vistas libres, 6 map_page. */
uint32_t g_view_failure = 0;

void initialize(const boot::BootInfo& boot_info) {
    g_hhdm_offset = boot_info.hhdm_offset;
    g_kernel_pml4_physical = read_page_table_root() & kPageMask;
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

uint32_t last_view_failure() {
    return g_view_failure;
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
    space.regions = nullptr;
    space.image_end = 0;
    return true;
}

void randomize_section_base(VmSpace& space, uint64_t seed) {
    if (space.pml4_virtual == nullptr) {
        return;
    }

    const uint64_t offset = (seed % kSectionAslrWindowBytes) & ~(memory::kPageSize - 1);
    space.next_section_base = kSectionViewBase + offset;
}

void destroy_address_space(VmSpace& space) {
    if (!g_ready || space.pml4_physical == 0 || space.pml4_virtual == nullptr) {
        return;
    }

    // Its process runs nowhere else by now, and the cores that ran it loaded
    // another space since. This core may still hold it (a process that reaps
    // itself), until the scheduler switches away before dropping the lock.
    const uint32_t self = arch::cpu_index();
    for (uint32_t cpu = 0; cpu < arch::kMaxCpus; ++cpu) {
        if (g_loaded_pml4[cpu] != space.pml4_physical) {
            continue;
        }
        if (cpu != self) {
            panic("vm: destroying an address space another core has loaded");
        }
        g_loaded_pml4[cpu] = 0;
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
    // The regions' pages went with the tables above; this is their list.
    free_regions(space);
    space.image_end = 0;
}

uint64_t resident_user_bytes(const VmSpace& space) {
    if (!g_ready || space.pml4_virtual == nullptr) {
        return 0;
    }

    // Se cuenta recorriendo las tablas en vez de llevar un contador incremental.
    // Un contador habria que mantenerlo en map_page, unmap_page, el fork (que
    // copia tablas sin pasar por map_page), las vistas de seccion y el destroy:
    // cinco lugares donde desincronizarse. El recorrido no puede mentir, y solo
    // pasa por las entradas presentes, asi que cuesta proporcional a lo mapeado
    // y no al espacio de direcciones. Lo pide el administrador de tareas una vez
    // por proceso por refresco.
    uint64_t pages = 0;
    for (uint64_t pml4_slot = 0; pml4_slot < kKernelPml4Start; ++pml4_slot) {
        const uint64_t pml4e = space.pml4_virtual[pml4_slot];
        if ((pml4e & kPagePresent) == 0 || (pml4e & kPageUser) == 0) {
            continue;
        }

        const uint64_t* pdpt = physical_to_virtual(pml4e & kPageMask);
        if (pdpt == nullptr) {
            continue;
        }
        for (uint64_t pdpt_slot = 0; pdpt_slot < 512; ++pdpt_slot) {
            const uint64_t pdpte = pdpt[pdpt_slot];
            if ((pdpte & kPagePresent) == 0 || (pdpte & kPageUser) == 0) {
                continue;
            }

            const uint64_t* pd = physical_to_virtual(pdpte & kPageMask);
            if (pd == nullptr) {
                continue;
            }
            for (uint64_t pd_slot = 0; pd_slot < 512; ++pd_slot) {
                const uint64_t pde = pd[pd_slot];
                if ((pde & kPagePresent) == 0 || (pde & kPageUser) == 0) {
                    continue;
                }

                const uint64_t* pt = physical_to_virtual(pde & kPageMask);
                if (pt == nullptr) {
                    continue;
                }
                for (uint64_t pt_slot = 0; pt_slot < 512; ++pt_slot) {
                    const uint64_t pte = pt[pt_slot];
                    if ((pte & kPagePresent) != 0 && (pte & kPageUser) != 0) {
                        pages += 1;
                    }
                }
            }
        }
    }

    return pages * kPageSizeBytes;
}

bool map_page(VmSpace& space, uint64_t virtual_address, uint64_t physical_address, uint64_t flags) {
    // map_page es exclusivamente la API de mappings de usuario. El half kernel
    // se copia de las tablas de Limine y nunca debe recibir entradas creadas por
    // un proceso, aunque una imagen ELF manipulada lo intente.
    if (!g_ready || !canonical_user_address(virtual_address) ||
        (virtual_address & 0xfff) != 0 || (physical_address & 0xfff) != 0) {
        return false;
    }

    // NX is enforced at the leaf. Do not copy it into upper-level entries:
    // an upper-level NX bit would constrain every executable page in that
    // subtree when a later mapping needs different permissions.
    const uint64_t table_flags = flags & ~vm::kPageNoExecute;
    uint64_t* pdpt = next_table(space.pml4_virtual, pml4_index(virtual_address), table_flags);
    if (pdpt == nullptr) {
        return false;
    }

    uint64_t* pd = next_table(pdpt, pdpt_index(virtual_address), table_flags);
    if (pd == nullptr) {
        return false;
    }

    uint64_t* pt = next_table(pd, pd_index(virtual_address), table_flags);
    if (pt == nullptr) {
        return false;
    }

    const uint64_t entry_index = pt_index(virtual_address);
    if ((pt[entry_index] & kPagePresent) != 0) {
        return false;
    }

    pt[entry_index] = (physical_address & kPageMask) | flags | kPagePresent;
    if ((read_page_table_root() & kPageMask) == space.pml4_physical) {
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
    note_user_unmap(space, virtual_address);
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
    clone.image_end = source.image_end;
    // The regions' pages are plain user pages, copied below like any other;
    // the child needs the list that says what they are.
    if (source.regions != nullptr && source.regions->count != 0) {
        if (!reserve_regions(clone, source.regions->count)) {
            destroy_address_space(clone);
            return false;
        }
        memcpy(clone.regions->entries, source.regions->entries, source.regions->count * sizeof(region::Region));
        clone.regions->count = source.regions->count;
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

                    // Las paginas de section views se re-mapean compartidas o
                    // clonadas mas abajo; copiarlas aca solo perderia la copia.
                    const uint64_t virtual_address =
                        sign_extend_48((pml4_slot << 39) | (pdpt_slot << 30) | (pd_slot << 21) | (pt_slot << 12));
                    if (section_view_for_address(source, virtual_address) != nullptr) {
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
                    if ((pte & kPageNoExecute) != 0) {
                        flags |= kPageNoExecute;
                    }

                    memcpy(page.virtual_address, physical_to_virtual(source_physical), memory::kPageSize);

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
    g_view_failure = 1;
    if (!g_ready || !section.in_use || section.page_count == 0 || section.physical_pages == nullptr) {
        return false;
    }

    if ((access_mask & section.access_mask) != access_mask) {
        g_view_failure = 2;
        return false;
    }

    /* NX es un permiso de la vista, no un default del mapa: sale puesto salvo
     * que la vista pida ejecucion, y para eso la seccion tiene que haberla
     * concedido -- el chequeo de arriba ya exigio que requested este dentro
     * del grant, asi que no hay forma de obtener exec de una seccion que no
     * la da.
     *
     * W^X sigue siendo estructural: una vista escribible y ejecutable a la vez
     * se rechaza aca. map_view_handle la rechaza tambien, porque ahi el rechazo
     * se puede volver EINVAL en vez del ENOMEM generico que devuelve esta
     * funcion, y el que programo el cargador merece un error que diga algo. */
    const bool wants_execute = (access_mask & object::access_execute) != 0;
    if (wants_execute && (access_mask & object::access_write) != 0) {
        return false;
    }

    const uint64_t view_size = section.size_bytes;
    if (view_size == 0) {
        return false;
    }

    if (base_address == 0) {
        if (!choose_section_view_base(space, view_size, base_address)) {
            g_view_failure = 3;
            return false;
        }
    } else if (!user_range_is_free(space, base_address, view_size)) {
        g_view_failure = 4;
        return false;
    }

    VmSpace::SectionView* view = find_free_section_view(space);
    if (view == nullptr) {
        g_view_failure = 5;
        return false;
    }

    uint64_t page_flags = kPageUser;
    if ((access_mask & object::access_write) != 0) {
        page_flags |= kPageWrite;
    }
    if (!wants_execute) {
        page_flags |= kPageNoExecute;
    }

    for (uint64_t page_index = 0; page_index < section.page_count; ++page_index) {
        const uint64_t virtual_address = base_address + (page_index * memory::kPageSize);
        if (!map_page(space, virtual_address, section.physical_pages[page_index], page_flags)) {
            g_view_failure = 6;
            for (uint64_t rollback = 0; rollback < page_index; ++rollback) {
                (void)unmap_page(space, base_address + (rollback * memory::kPageSize), nullptr);
            }
            // A sibling thread on another core may already have touched the
            // pages: the caller's reference must not be the one that frees
            // them before that core flushes.
            object::retain(&section.header);
            release_after_shootdown(section);
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

FixedRange classify_fixed_range(const VmSpace& space, uint64_t base_address, uint64_t size) {
    if (!canonical_user_range(base_address, size)) {
        return FixedRange::outside;
    }
    return user_range_is_free(space, base_address, size) ? FixedRange::free : FixedRange::occupied;
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

    reclaim_deferred_views();
    release_after_shootdown(*view->section);
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
                (flags | vm::kPageWrite | vm::kPageNoExecute) & ~vm::kPageUser)) {
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

bool map_kernel_device_memory(uint64_t physical_base, size_t size, uint64_t flags, void** virtual_base) {
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
                (flags | vm::kPageWrite | vm::kPageNoExecute) & ~vm::kPageUser)) {
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

bool map_kernel_mmio(uint64_t physical_base, size_t size, uint64_t flags, void** virtual_base) {
    // Registros de dispositivo: siempre sin cachear.
    return map_kernel_device_memory(
        physical_base, size, flags | vm::kPageCacheDisable, virtual_base);
}

bool kernel_page_cache_flags(uint64_t virtual_address, uint64_t& flags) {
    if (!g_ready) {
        return false;
    }

    uint64_t* table = vm::physical_to_virtual(g_kernel_pml4_physical);
    if (table == nullptr) {
        return false;
    }

    for (int level = 3; level >= 0; --level) {
        const uint64_t index = (virtual_address >> (12 + (level * 9))) & 0x1FFull;
        const uint64_t entry = table[index];
        if ((entry & vm::kPagePresent) == 0) {
            return false;
        }

        // Una entrada intermedia con PS prendido ya ES la hoja: es una pagina
        // grande. Ahi PWT y PCD estan en el mismo lugar pero el bit PAT es el
        // 12, no el 7 (que es justamente el PS). Hay que traducirlo al encoding
        // de una pagina de 4 KiB, que es lo que devuelve esta funcion.
        // Confundirlos hace leer PS como PAT y elegir el tipo de memoria
        // equivocado -- el framebuffer de Limine viene mapeado asi.
        const bool large_page = level > 0 && (entry & (1ull << 7)) != 0;
        if (large_page || level == 0) {
            uint64_t cache_bits = entry & (vm::kPageWriteThrough | vm::kPageCacheDisable);
            const uint64_t pat_bit = large_page ? (1ull << 12) : vm::kPagePat;
            if ((entry & pat_bit) != 0) {
                cache_bits |= vm::kPagePat;
            }
            flags = cache_bits;
            return true;
        }

        table = vm::physical_to_virtual(entry & kPageMask);
        if (table == nullptr) {
            return false;
        }
    }
    return false;
}

bool write_combining_page_flags(uint64_t& flags) {
    if (!g_ready) {
        return false;
    }

    // Which IA32_PAT index is set up as write-combining is the firmware's
    // choice, so it is looked up rather than assumed. Limine leaves WC in an
    // entry of its own but maps the framebuffer with another (write-through),
    // which is safe but not the best for burst writes to VRAM.
    constexpr uint32_t kMsrPat = 0x277u;
    const uint64_t pat = arch::read_msr(kMsrPat);

    constexpr uint64_t kMemoryTypeWriteCombining = 0x01;
    for (uint64_t index = 0; index < 8; ++index) {
        if (((pat >> (index * 8)) & 0xFFull) != kMemoryTypeWriteCombining) {
            continue;
        }
        uint64_t cache_bits = 0;
        if ((index & 1u) != 0) {
            cache_bits |= vm::kPageWriteThrough;
        }
        if ((index & 2u) != 0) {
            cache_bits |= vm::kPageCacheDisable;
        }
        if ((index & 4u) != 0) {
            cache_bits |= vm::kPagePat;
        }
        flags = cache_bits;
        return true;
    }
    return false;
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

/* Agrega permisos a una pagina de usuario ya mapeada. Devuelve false si no lo
 * esta. Existe para los ELF donde dos PT_LOAD comparten una pagina: la pagina
 * tiene que quedar con la union de los permisos de los dos. */
bool add_user_page_flags(VmSpace& space, uint64_t address, uint64_t flags) {
    const uint64_t page = address & ~static_cast<uint64_t>(memory::kPageSize - 1);
    /* page_table_for devuelve la TABLA, no la entrada. */
    uint64_t* table = page_table_for(space, page, false);

    if (table == nullptr) {
        return false;
    }
    /* Suma permisos, pero el bit de ejecucion se RESUELVE en vez de sumarse: si
     * el segmento no ejecutable se mapea primero, la pagina queda con NX y un
     * segmento de texto posterior no podria volver a ejecutarse nunca. Con solo
     * OR el NX es irreversible, que es al reves de como funciona una union de
     * permisos.
     *
     * El caso que se resuelve es R con R+X, que es el que produce cualquier
     * binario con interprete. W^X no queda en manos de aca: la union escribible
     * y ejecutable la rechaza el loader antes de mapear. */
    if ((flags & kPageNoExecute) == 0) {
        table[pt_index(page)] &= ~static_cast<uint64_t>(kPageNoExecute);
    }
    table[pt_index(page)] |= (flags & (kPageWrite | kPageUser | kPageNoExecute));
    if ((read_page_table_root() & kPageMask) == space.pml4_physical) {
        invalidate_page(page);
    }
    return true;
}

bool retarget_kernel_page(void* virtual_address, uint64_t physical_address, uint64_t flags) {
    const uint64_t address = reinterpret_cast<uint64_t>(virtual_address);
    return unmap_kernel_page(address) &&
        map_kernel_page(address, physical_address,
                        (flags | vm::kPageWrite | vm::kPageNoExecute) & ~vm::kPageUser);
}

void sync_kernel_tlb() {
    const uint32_t cpu = arch::cpu_index();
    const uint64_t generation = g_kernel_tlb_generation;
    if (g_kernel_tlb_synced[cpu] == generation) {
        return;
    }
    arch::flush_tlb();
    g_kernel_tlb_synced[cpu] = generation;
}

void load_address_space(const VmSpace& space) {
    const uint32_t cpu = arch::cpu_index();
    g_loaded_pml4[cpu] = space.pml4_physical;
    if ((read_page_table_root() & kPageMask) == space.pml4_physical) {
        return;
    }
    arch::load_page_table_root(space.pml4_physical);
    // Reloading CR3 drops every user translation: whatever shootdown was asked
    // of this core is done.
    g_flush_done[cpu] = g_flush_requested[cpu];
}

void sync_user_tlb() {
    const uint32_t cpu = arch::cpu_index();
    if (g_flush_done[cpu] == g_flush_requested[cpu]) {
        return;
    }
    arch::load_page_table_root(read_page_table_root());
    g_flush_done[cpu] = g_flush_requested[cpu];
}

void reclaim_deferred_views() {
    DeferredPages** pages_link = &g_deferred_pages;
    while (*pages_link != nullptr) {
        DeferredPages* const record = *pages_link;
        if (!flushed_since(record->cpus, record->sequence)) {
            pages_link = &record->next;
            continue;
        }
        *pages_link = record->next;
        free_deferred_record(record);
    }

    object::SectionObject** link = &g_deferred_views;
    while (*link != nullptr) {
        object::SectionObject* const section = *link;
        if (!flushed_since(section->deferred_cpus, section->deferred_sequence)) {
            link = &section->deferred_next;
            continue;
        }
        *link = section->deferred_next;
        uint32_t refs = section->deferred_refs;
        section->deferred_next = nullptr;
        section->deferred_refs = 0;
        section->deferred_cpus = 0;
        section->deferred_sequence = 0;
        while (refs-- > 0) {
            object::Header* header = &section->header;
            object::release(header);
        }
    }
}

bool ensure_user_stack_page(VmSpace& space, uint64_t address) {
    memory::PageAllocation allocation = {};
    const uint64_t page = address & ~static_cast<uint64_t>(memory::kPageSize - 1);

    if (!g_ready || address < kUserStackBottom || address >= kUserStackTop) {
        return false;
    }
    if (page_table_for(space, page, true) != nullptr) {
        return true;
    }
    if (!memory::allocate_page(allocation)) {
        return false;
    }
    memset(allocation.virtual_address, 0, memory::kPageSize);
    if (!map_page(space, page, allocation.physical_address, kPageUser | kPageWrite | kPageNoExecute)) {
        (void)memory::free_allocation(allocation);
        return false;
    }
    return true;
}

/* NOTE: this query HAS a side effect on the stack and on regions.
 *
 * The user stack grows on demand from the #PF handler, and a region's pages
 * appear on first touch the same way, but the kernel does not fault when it
 * touches user memory: it checks the range with this function and then
 * copies. If pages not there yet counted as inaccessible, a read() into a
 * large array on the stack would fail with EFAULT instead of growing it --
 * which is what happened to the 64 KiB buffer windowd rereads a .sxicon
 * with -- and a read() into fresh mmap memory would fail the same way.
 *
 * Making the pages here and not in each caller is deliberate: some twenty
 * places in the kernel check user memory, and this is the one they all go
 * through. */
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
            VmSpace& writable = const_cast<VmSpace&>(space);
            const bool made = (page >= kUserStackBottom && page < kUserStackTop)
                ? ensure_user_stack_page(writable, page)
                : fault_in_region(writable, page, require_write, false);
            if (!made) {
                return false;
            }
        }
        if (page == last_page) {
            break;
        }
        page += memory::kPageSize;
    }
    return true;
}

RegionStatus map_region(VmSpace& space, uint64_t& address, uint64_t size, uint8_t access,
                        RegionPlacement placement) {
    if (!g_ready || !valid_region_access(access) || size == 0 || (size & (memory::kPageSize - 1)) != 0) {
        return RegionStatus::invalid;
    }
    uint64_t start = address;
    if (placement == RegionPlacement::anywhere) {
        const bool hint_free = start != 0 && region_range(start, size) &&
                               !region_overlaps(space, start, start + size) &&
                               other_mapping(&space, start, start + size) == 0;
        if (!hint_free && !region::find_gap(regions_of(space), size, kRegionBase, kUserStackGuardBottom,
                                            other_mapping, &space, start)) {
            return RegionStatus::no_memory;
        }
    } else {
        if (!region_range(start, size)) {
            return RegionStatus::invalid;
        }
        const uint64_t end = start + size;
        if (placement == RegionPlacement::exact) {
            if (region_overlaps(space, start, end) || other_mapping(&space, start, end) != 0) {
                return RegionStatus::occupied;
            }
        } else if (other_mapping(&space, start, end) != 0) {
            return RegionStatus::foreign;
        }
    }

    // Replacing can split one region (one entry more) before the new one
    // goes in (another).
    if (!reserve_regions(space, 2 * region::kMaxGrowth)) {
        return RegionStatus::no_memory;
    }
    const uint64_t end = start + size;
    if (placement == RegionPlacement::replace) {
        if (!release_mapped_pages(space, start, end)) {
            return RegionStatus::no_memory;
        }
        (void)region::remove(*space.regions, start, end);
    }
    if (!region::insert(*space.regions, start, end, access)) {
        return RegionStatus::no_memory;
    }
    address = start;
    return RegionStatus::ok;
}

RegionStatus unmap_regions(VmSpace& space, uint64_t address, uint64_t size) {
    if (!g_ready || !region_range(address, size)) {
        return RegionStatus::invalid;
    }
    const uint64_t end = address + size;
    if (other_mapping(&space, address, end) != 0) {
        return RegionStatus::foreign;
    }
    if (!region_overlaps(space, address, end)) {
        return RegionStatus::ok;
    }
    if (!reserve_regions(space, region::kMaxGrowth) || !release_mapped_pages(space, address, end)) {
        return RegionStatus::no_memory;
    }
    (void)region::remove(*space.regions, address, end);
    return RegionStatus::ok;
}

RegionStatus remap_region(VmSpace& space, uint64_t address, uint64_t old_size, uint64_t new_size,
                          bool may_move, uint64_t& new_address) {
    if (!g_ready || !region_range(address, old_size) || new_size == 0 ||
        (new_size & (memory::kPageSize - 1)) != 0) {
        return RegionStatus::invalid;
    }
    const uint64_t old_end = address + old_size;
    const int32_t index = region::containing(regions_of(space), address, old_end);
    if (index < 0) {
        return RegionStatus::not_mapped;
    }
    const uint8_t access = space.regions->entries[index].access;

    if (new_size == old_size) {
        new_address = address;
        return RegionStatus::ok;
    }
    if (new_size < old_size) {
        if (!reserve_regions(space, region::kMaxGrowth) ||
            !release_mapped_pages(space, address + new_size, old_end)) {
            return RegionStatus::no_memory;
        }
        (void)region::remove(*space.regions, address + new_size, old_end);
        new_address = address;
        return RegionStatus::ok;
    }

    // In place, when what follows is free.
    const uint64_t new_end = address + new_size;
    if (region_range(address, new_size) && !region_overlaps(space, old_end, new_end) &&
        other_mapping(&space, old_end, new_end) == 0) {
        if (!reserve_regions(space, region::kMaxGrowth) ||
            !region::insert(*space.regions, old_end, new_end, access)) {
            return RegionStatus::no_memory;
        }
        new_address = address;
        return RegionStatus::ok;
    }
    if (!may_move) {
        return RegionStatus::no_memory;
    }

    // Elsewhere: the pages' entries move, their contents stay put. Every
    // table the destination needs is made first, so the move itself cannot
    // fail half way.
    uint64_t target = 0;
    if (!region::find_gap(regions_of(space), new_size, kRegionBase, kUserStackGuardBottom, other_mapping, &space,
                          target) ||
        !reserve_regions(space, 2 * region::kMaxGrowth)) {
        return RegionStatus::no_memory;
    }
    for (uint64_t page = next_mapped_page(space, address, old_end); page < old_end;
         page = next_mapped_page(space, page + memory::kPageSize, old_end)) {
        if (leaf_table(space, target + (page - address)) == nullptr) {
            return RegionStatus::no_memory;
        }
    }
    for (uint64_t page = next_mapped_page(space, address, old_end); page < old_end;
         page = next_mapped_page(space, page + memory::kPageSize, old_end)) {
        move_page(space, page, target + (page - address));
    }
    // The pages stay owned, so nothing waits for the other cores' flush: they
    // only have to stop using the old addresses.
    uint32_t cpus = 0;
    (void)finish_shootdown(cpus);
    (void)region::remove(*space.regions, address, old_end);
    (void)region::insert(*space.regions, target, target + new_size, access);
    new_address = target;
    return RegionStatus::ok;
}

bool fault_in_region(VmSpace& space, uint64_t address, bool write, bool execute) {
    if (!g_ready || space.regions == nullptr || !canonical_user_address(address)) {
        return false;
    }
    const int32_t index = region::find(*space.regions, address);
    if (index < 0) {
        return false;
    }
    const uint8_t access = space.regions->entries[index].access;
    if (access == 0 || (write && (access & region::kWrite) == 0) ||
        (execute && (access & region::kExecute) == 0)) {
        return false;
    }
    const uint64_t page = address & ~static_cast<uint64_t>(memory::kPageSize - 1);
    // Mapped already, with the region's own access: asked twice.
    if (page_table_for(space, page, false) != nullptr) {
        return true;
    }
    memory::PageAllocation allocation = {};
    if (!memory::allocate_page(allocation)) {
        return false;
    }
    memset(allocation.virtual_address, 0, memory::kPageSize);
    if (!map_page(space, page, allocation.physical_address, region_page_flags(access))) {
        (void)memory::free_allocation(allocation);
        return false;
    }
    return true;
}

} // namespace vm
