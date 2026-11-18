#include "paging.hpp"
#include "process.hpp"
#include "cpu.hpp"
#include "kernel/physical_memory.hpp"
#include "kernel/string.hpp"

namespace i386::paging {
namespace {
alignas(4096) uint32_t g_page_directory[1024];
constexpr uint32_t kPresent = 1, kWrite = 2, kUser = 4, kLarge = 128, kBorrowed = 512;
constexpr uint32_t kMask = 0xfffff000u;
constexpr uint32_t kFirst = kUserWindowBase >> 22;
constexpr uint32_t kEnd = kUserWindowEnd >> 22;
bool g_enabled;
uint32_t g_kernel_generation;
uint32_t g_seen_generation[cpu::kMaxCpus];

uint32_t* table(uint32_t entry) {
    return reinterpret_cast<uint32_t*>((entry & kMask) + kDirectMapBase);
}

bool inside(uint32_t address, uint32_t bytes) {
    return address >= kUserWindowBase && address < kUserWindowEnd &&
        static_cast<uint64_t>(address) + bytes <= kUserWindowEnd;
}
} // namespace

bool cpu_has_pse() {
    uint32_t eax, ebx, ecx, edx;
    asm volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(1u), "c"(0u));
    return (edx & (1u << 3)) != 0;
}

void enable_identity_1gib() {
    for (uint32_t index = 0; index < 256; ++index) {
        const uint32_t entry = (index << 22) | kPresent | kWrite | kLarge;
        g_page_directory[index] = entry;
        g_page_directory[(kDirectMapBase >> 22) + index] = entry;
    }
    uint32_t cr4, cr0;
    asm volatile("mov %%cr4, %0" : "=r"(cr4));
    cr4 |= 1u << 4;
    asm volatile("mov %0, %%cr4" : : "r"(cr4) : "memory");
    activate_kernel();
    asm volatile("mov %%cr0, %0" : "=r"(cr0));
    cr0 |= (1u << 31) | (1u << 16); // PG | WP, including kernel writes to user text.
    asm volatile("mov %0, %%cr0" : : "r"(cr0) : "memory");
    g_enabled = true;
}

bool map_identity(uint64_t address, uint32_t bytes) {
    if (bytes == 0 || address >= 0x100000000ull || address + bytes > 0x100000000ull)
        return false;
    // Device identity mappings must not overlap the user window or RAM aliases.
    if (address < kUserWindowEnd && address + bytes > kUserWindowBase) return false;
    if (address < static_cast<uint64_t>(kDirectMapBase) + kDirectMapBytes &&
        address + bytes > kDirectMapBase) return false;
    if (address < kKernelViewsEnd && address + bytes > kKernelViewsBase) return false;
    const uint32_t first = static_cast<uint32_t>(address >> 22);
    const uint32_t last = static_cast<uint32_t>((address + bytes - 1) >> 22);
    for (uint32_t index = first; index <= last; ++index) {
        g_page_directory[index] = (index << 22) | kPresent | kWrite | kLarge;
        for (uint32_t n = 0; n < process::kCapacity; ++n)
            if (process::at(n)->state != process::State::unused)
                process::at(n)->directory[index] = g_page_directory[index];
    }
    ++g_kernel_generation;
    if (g_enabled) sync_kernel_tlb();
    return true;
}

void initialize_process_directory(uint32_t* directory) {
    memcpy(directory, g_page_directory, sizeof(g_page_directory));
    // The user window starts completely absent, including its unused holes.
    for (uint32_t index = kFirst; index < kEnd; ++index) directory[index] = 0;
}

void destroy_process_directory(uint32_t* directory) {
    // Call only after leaving this CR3. Kernel/device mappings are shared.
    for (uint32_t index = kFirst; index < kEnd; ++index) {
        const uint32_t entry = directory[index];
        if ((entry & kPresent) == 0) continue;
        uint32_t* pages = table(entry);
        for (uint32_t page = 0; page < 1024; ++page) {
            if ((pages[page] & (kPresent | kBorrowed)) == kPresent)
                memory::free_pages(pages[page] & kMask, 1);
        }
        memory::free_pages(entry & kMask, 1);
        directory[index] = 0;
    }
}

bool map_user_range(uint32_t* directory, uint32_t address, uint32_t bytes, bool writable) {
    if (!inside(address, bytes)) return false;
    if (bytes == 0) return true;
    const uint32_t end = address + bytes;
    for (uint32_t page = address & kMask; page < end; page += kPageBytes) {
        uint32_t& entry = directory[page >> 22];
        memory::PageAllocation allocation = {};
        if ((entry & kPresent) == 0) {
            if (!memory::allocate_page(allocation)) return false;
            memset(allocation.virtual_address, 0, kPageBytes);
            entry = static_cast<uint32_t>(allocation.physical_address) | kPresent | kWrite | kUser;
        }
        uint32_t& leaf = table(entry)[(page >> 12) & 1023];
        if ((leaf & kPresent) == 0) {
            if (!memory::allocate_page(allocation)) return false;
            memset(allocation.virtual_address, 0, kPageBytes);
            leaf = static_cast<uint32_t>(allocation.physical_address) | kPresent | kUser;
        }
        if (writable) leaf |= kWrite; // Union permissions for PT_LOAD page sharing.
    }
    return true;
}

void* user_pointer(const uint32_t* directory, uint32_t address) {
    if (!inside(address, 1)) return nullptr;
    const uint32_t entry = directory[address >> 22];
    if ((entry & (kPresent | kUser | kLarge)) != (kPresent | kUser)) return nullptr;
    const uint32_t leaf = table(entry)[(address >> 12) & 1023];
    if ((leaf & (kPresent | kUser)) != (kPresent | kUser)) return nullptr;
    return reinterpret_cast<void*>(kDirectMapBase + (leaf & kMask) + (address & 4095));
}

bool user_range(const uint32_t* directory, uint32_t address, uint32_t bytes, bool writable) {
    if (!inside(address, bytes)) return false;
    const uint32_t end = address + bytes;
    for (uint32_t page = address & kMask; page < end; page += kPageBytes) {
        if (!user_pointer(directory, page)) {
            // Match the reference VMM: syscall buffers can be the first use
            // of stack pages reserved by a large automatic C object.
            if (page < kUserStackBottom || page >= kUserStackTop ||
                !map_user_range(const_cast<uint32_t*>(directory), page, kPageBytes, true)) return false;
        }
        const uint32_t entry = directory[page >> 22];
        const uint32_t leaf = table(entry)[(page >> 12) & 1023];
        if (writable && ((entry & kWrite) == 0 || (leaf & kWrite) == 0)) return false;
    }
    return true;
}

bool copy_to_user(uint32_t* directory, uint32_t address, const void* source, uint32_t bytes) {
    // Also used by the ELF loader to initialize read-only text through its alias.
    if (!user_range(directory, address, bytes, false)) return false;
    const auto* data = static_cast<const uint8_t*>(source);
    while (bytes != 0) {
        uint32_t chunk = kPageBytes - (address & 4095);
        if (chunk > bytes) chunk = bytes;
        memcpy(user_pointer(directory, address), data, chunk);
        address += chunk; data += chunk; bytes -= chunk;
    }
    return true;
}

bool copy_from_user(const uint32_t* directory, void* destination, uint32_t address, uint32_t bytes) {
    if (!user_range(directory, address, bytes, false)) return false;
    auto* data = static_cast<uint8_t*>(destination);
    while (bytes != 0) {
        uint32_t chunk = kPageBytes - (address & 4095);
        if (chunk > bytes) chunk = bytes;
        memcpy(data, user_pointer(directory, address), chunk);
        address += chunk; data += chunk; bytes -= chunk;
    }
    return true;
}

uint32_t resident_bytes(const uint32_t* directory) {
    uint32_t pages = 0;
    for (uint32_t index = kFirst; index < kEnd; ++index) {
        if ((directory[index] & kPresent) == 0) continue;
        for (uint32_t page = 0; page < 1024; ++page)
            if (table(directory[index])[page] & kPresent) ++pages;
    }
    return pages * kPageBytes;
}

bool clone_private_pages(const uint32_t* source, uint32_t* destination) {
    for (uint32_t index = kFirst; index < kEnd; ++index) {
        if (!(source[index] & kPresent)) continue;
        const auto* leaves = table(source[index]);
        for (uint32_t page = 0; page < 1024; ++page) {
            if ((leaves[page] & (kPresent | kBorrowed)) != kPresent) continue;
            const uint32_t address = (index << 22) | (page << 12);
            if (!map_user_range(destination, address, kPageBytes, (leaves[page] & kWrite) != 0))
                return false;
            memcpy(user_pointer(destination, address), user_pointer(source, address), kPageBytes);
        }
    }
    return true;
}

bool range_free(const uint32_t* directory, uint32_t address, uint32_t bytes) {
    if (!bytes || (address & 4095) || (bytes & 4095) || !inside(address, bytes)) return false;
    // Keep the entire reserved stack and its permanent guard out of views.
    if (address < kUserStackTop && address + bytes > kUserStackGuard) return false;
    for (uint32_t page = address; page < address + bytes; page += kPageBytes)
        if (user_pointer(directory, page)) return false;
    return true;
}

bool map_borrowed_page(uint32_t* directory, uint32_t address, uint32_t physical, bool writable) {
    if (!range_free(directory, address, kPageBytes) || (physical & 4095)) return false;
    uint32_t& entry = directory[address >> 22];
    if (!(entry & kPresent)) {
        memory::PageAllocation allocation = {};
        if (!memory::allocate_page(allocation)) return false;
        memset(allocation.virtual_address, 0, kPageBytes);
        entry = static_cast<uint32_t>(allocation.physical_address) | kPresent | kWrite | kUser;
    }
    table(entry)[(address >> 12) & 1023] = physical | kPresent | kUser | kBorrowed |
        (writable ? kWrite : 0);
    return true;
}

void unmap_borrowed_range(uint32_t* directory, uint32_t address, uint32_t bytes) {
    for (uint32_t page = address; page < address + bytes; page += kPageBytes) {
        uint32_t& entry = directory[page >> 22];
        if (!(entry & kPresent)) continue;
        auto* leaves = table(entry);
        uint32_t& leaf = leaves[(page >> 12) & 1023];
        if (leaf & kBorrowed) {
            leaf = 0;
            asm volatile("invlpg (%0)" : : "r"(page) : "memory");
        }
        bool empty = true;
        for (uint32_t index = 0; index < 1024; ++index)
            if (leaves[index] & kPresent) { empty = false; break; }
        if (empty) { memory::free_pages(entry & kMask, 1); entry = 0; }
    }
}

void activate(const uint32_t* directory) {
    asm volatile("mov %0, %%cr3" : : "r"(static_cast<uint32_t>(reinterpret_cast<uintptr_t>(directory))) : "memory");
}
void activate_kernel() { activate(g_page_directory); }
void sync_kernel_tlb() {
    auto& seen = g_seen_generation[cpu::index()];
    if (seen == g_kernel_generation) return;
    uint32_t cr3;
    asm volatile("mov %%cr3, %0; mov %0, %%cr3" : "=r"(cr3) :: "memory");
    seen = g_kernel_generation;
}
namespace {

bool kernel_leaf(uint32_t address, uint32_t*& leaf, bool create) {
    const uint32_t index = address >> 22;
    auto& entry = g_page_directory[index];
    if (!entry) {
        if (!create) return false;
        memory::PageAllocation allocation = {};
        if (!memory::allocate_page(allocation)) return false;
        memset(allocation.virtual_address, 0, kPageBytes);
        entry = static_cast<uint32_t>(allocation.physical_address) | kPresent | kWrite;
        // The global kernel lock serializes publication. New directories
        // inherit the root; existing directories share the same page tables.
        for (uint32_t slot = 0; slot < process::kCapacity; ++slot) {
            auto* task = process::at(slot);
            if (task->state != process::State::unused) task->directory[index] = entry;
        }
    }
    if (entry & kLarge) return false;
    leaf = &table(entry)[(address >> 12) & 1023];
    return true;
}
}

bool unmap_kernel_pages(void* base, uint32_t count) {
    const uintptr_t address = reinterpret_cast<uintptr_t>(base);
    if ((address & 4095) || address < kKernelViewsBase ||
        static_cast<uint64_t>(address) + static_cast<uint64_t>(count) * kPageBytes > kKernelViewsEnd)
        return false;
    ++g_kernel_generation;
    for (uint32_t n = 0; n < count; ++n) {
        const uint32_t page = address + n * kPageBytes;
        uint32_t* leaf;
        if (kernel_leaf(page, leaf, false)) *leaf = 0;
        asm volatile("invlpg (%0)" : : "r"(page) : "memory");
    }
    return true;
}

namespace {
// Both RAM sections and PCI BARs use the existing shared supervisor tables.
// MMIO supplies a contiguous physical base instead of an array of RAM pages.
bool map_kernel_range(const uint64_t* pages, uint32_t physical, uint32_t count,
                      uint32_t flags, void** base) {
    if (!base || !count || count > (kKernelViewsEnd - kKernelViewsBase) / kPageBytes)
        return false;
    uint32_t run = 0, start = kKernelViewsBase;
    for (uint32_t page = kKernelViewsBase; page < kKernelViewsEnd; page += kPageBytes) {
        uint32_t* leaf;
        if (kernel_leaf(page, leaf, false) && (*leaf & kPresent)) { run = 0; continue; }
        if (!run) start = page;
        if (++run != count) continue;
        for (uint32_t n = 0; n < count; ++n) {
            if (!kernel_leaf(start + n * kPageBytes, leaf, true)) {
                unmap_kernel_pages(reinterpret_cast<void*>(start), n);
                return false;
            }
            const uint32_t address = pages ? static_cast<uint32_t>(pages[n]) : physical + n * kPageBytes;
            *leaf = address | kPresent | (flags & (kWrite | 0x18u));
            asm volatile("invlpg (%0)" : : "r"(start + n * kPageBytes) : "memory");
        }
        ++g_kernel_generation;
        *base = reinterpret_cast<void*>(start);
        return true;
    }
    return false;
}
}

bool retarget_kernel_page(void* base, uint32_t physical) {
    const uint32_t address = reinterpret_cast<uintptr_t>(base);
    uint32_t* leaf;
    if ((address & 4095) || (physical & 4095) || physical >= kDirectMapBytes ||
        address < kKernelViewsBase || address >= kKernelViewsEnd ||
        !kernel_leaf(address, leaf, false) || !(*leaf & kPresent)) return false;
    *leaf = physical | (*leaf & ~kMask);
    ++g_kernel_generation;
    asm volatile("invlpg (%0)" :: "r"(address) : "memory");
    return true;
}

bool map_kernel_pages(const uint64_t* pages, uint32_t count, uint32_t flags, void** base) {
    if (!pages || count > (kKernelViewsEnd - kKernelViewsBase) / kPageBytes) return false;
    for (uint32_t n = 0; n < count; ++n)
        if ((pages[n] & 4095) || pages[n] >= kDirectMapBytes) return false;
    return map_kernel_range(pages, 0, count, flags, base);
}

bool map_kernel_mmio(uint64_t physical, uint32_t bytes, void** base) {
    constexpr uint64_t end = 0x100000000ull;
    if (!base || !bytes || physical >= end || bytes > end - physical) return false;
    const uint32_t offset = physical & 4095u;
    const uint64_t span = static_cast<uint64_t>(offset) + bytes;
    const uint32_t count = (span + kPageBytes - 1) / kPageBytes;
    void* mapped = nullptr;
    // PWT | PCD selects uncached device memory with the default x86 PAT.
    if (!map_kernel_range(nullptr, static_cast<uint32_t>(physical) & kMask,
                          count, kWrite | 0x18u, &mapped)) return false;
    *base = static_cast<uint8_t*>(mapped) + offset;
    return true;
}

} // namespace i386::paging
