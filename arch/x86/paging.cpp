#include "paging.hpp"
#include "kernel/physical_memory.hpp"
#include "kernel/string.hpp"

namespace i386::paging {
namespace {
alignas(4096) uint32_t g_page_directory[1024];
constexpr uint32_t kPresent = 1, kWrite = 2, kUser = 4, kLarge = 128;
constexpr uint32_t kMask = 0xfffff000u;
constexpr uint32_t kFirst = kUserWindowBase >> 22;
constexpr uint32_t kEnd = kUserWindowEnd >> 22;
bool g_enabled;

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
    // A device identity mapping must not replace the RAM direct map.
    if (address < static_cast<uint64_t>(kDirectMapBase) + kDirectMapBytes &&
        address + bytes > kDirectMapBase) return false;
    const uint32_t first = static_cast<uint32_t>(address >> 22);
    const uint32_t last = static_cast<uint32_t>((address + bytes - 1) >> 22);
    for (uint32_t index = first; index <= last; ++index)
        g_page_directory[index] = (index << 22) | kPresent | kWrite | kLarge;
    if (g_enabled) activate_kernel();
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
            if (pages[page] & kPresent) memory::free_pages(pages[page] & kMask, 1);
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
        if (!user_pointer(directory, page)) return false;
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

void activate(const uint32_t* directory) {
    asm volatile("mov %0, %%cr3" : : "r"(static_cast<uint32_t>(reinterpret_cast<uintptr_t>(directory))) : "memory");
}
void activate_kernel() { activate(g_page_directory); }
} // namespace i386::paging
