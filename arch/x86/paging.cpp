#include "paging.hpp"

#include <stdint.h>

namespace i386 {

namespace {

/* 256 x 4 MiB = 1 GiB. Alineado a pagina porque CR3 lo exige. */
alignas(4096) uint32_t g_page_directory[1024];

constexpr uint32_t kPageTableEntries = 256;
constexpr uint32_t kPageSize4Mib = 0x00400000;

constexpr uint32_t kPdePresent = 1u << 0;
constexpr uint32_t kPdeWritable = 1u << 1;
constexpr uint32_t kPdeUser = 1u << 2;
constexpr uint32_t kPdePageSize = 1u << 7;

inline uint32_t read_cr0() {
    uint32_t value;
    asm volatile("mov %%cr0, %0" : "=r"(value));
    return value;
}

inline void write_cr0(uint32_t value) {
    asm volatile("mov %0, %%cr0" : : "r"(value) : "memory");
}

inline void write_cr3(uint32_t value) {
    asm volatile("mov %0, %%cr3" : : "r"(value) : "memory");
}

inline uint32_t read_cr4() {
    uint32_t value;
    asm volatile("mov %%cr4, %0" : "=r"(value));
    return value;
}

inline void write_cr4(uint32_t value) {
    asm volatile("mov %0, %%cr4" : : "r"(value) : "memory");
}

}

namespace {

bool g_enabled = false;

constexpr uint64_t kAddressSpaceLimit = 0x100000000ull;

/* Reload CR3 so newly mapped framebuffer pages are visible immediately. */
void flush_tlb(uint32_t directory) {
    write_cr3(directory);
}

}  // namespace

bool paging::cpu_has_pse() {
    uint32_t eax = 0;
    uint32_t ebx = 0;
    uint32_t ecx = 0;
    uint32_t edx = 0;
    asm volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(1u), "c"(0u));
    (void)eax;
    (void)ebx;
    (void)ecx;
    return (edx & (1u << 3)) != 0;
}

void paging::enable_identity_1gib() {
    for (uint32_t i = 0; i < kPageTableEntries; ++i) {
        g_page_directory[i] = (i * kPageSize4Mib) | kPdePresent | kPdeWritable | kPdePageSize;
    }

    // The idle/kernel directory stays supervisor-only. Each process installs
    // private user mappings in its own directory after its image is loaded.

    /* Sin paginacion encendida la direccion del directorio es tambien su
     * direccion fisica, que es lo que CR3 quiere. */
    const uint32_t directory = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(g_page_directory));

    uint32_t cr4 = read_cr4();
    cr4 |= (1u << 4); /* PSE */
    write_cr4(cr4);

    write_cr3(directory);

    uint32_t cr0 = read_cr0();
    cr0 |= (1u << 31) | (1u << 16); /* PG | WP */
    write_cr0(cr0);
    g_enabled = true;
}

bool paging::map_identity(uint64_t address, uint32_t bytes) {
    if (bytes == 0 || address >= kAddressSpaceLimit ||
        address + bytes > kAddressSpaceLimit) {
        return false;
    }
    /* Identity-map the entire 4 MiB pages covering the physical range. */
    const uint32_t first = static_cast<uint32_t>(address) / kPageSize4Mib;
    const uint32_t last = static_cast<uint32_t>(address + bytes - 1u) / kPageSize4Mib;
    if (last >= 1024) {
        return false;
    }
    for (uint32_t index = first; index <= last; ++index) {
        const uint32_t entry = (index * kPageSize4Mib) | kPdePresent | kPdeWritable | kPdePageSize;
        /* Preserve user access on any previously mapped page. */
        g_page_directory[index] = entry | (g_page_directory[index] & kPdeUser);
    }
    if (g_enabled) {
        flush_tlb(static_cast<uint32_t>(reinterpret_cast<uintptr_t>(g_page_directory)));
    }
    return true;
}

}

void i386::paging::initialize_process_directory(uint32_t* directory, uint32_t physical_base) {
    for (uint32_t index = 0; index < 1024; ++index) {
        directory[index] = g_page_directory[index] & ~kPdeUser;
    }
    const uint32_t first = kUserWindowBase / kPageSize4Mib;
    const uint32_t pages = (kUserWindowEnd - kUserWindowBase) / kPageSize4Mib;
    for (uint32_t index = 0; index < pages; ++index) {
        directory[first + index] = (physical_base + index * kPageSize4Mib) |
            kPdePresent | kPdeWritable | kPdeUser | kPdePageSize;
    }
}

void i386::paging::activate(const uint32_t* directory) {
    write_cr3(static_cast<uint32_t>(reinterpret_cast<uintptr_t>(directory)));
}

void i386::paging::activate_kernel() {
    activate(g_page_directory);
}
