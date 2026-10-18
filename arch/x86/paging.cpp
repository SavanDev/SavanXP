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
}

}
