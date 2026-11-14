#pragma once

#include <stdint.h>

/*
 * Paginacion de 32 bits sin PAE: paginas de 4 MiB (CR4.PSE) que mapean en
 * identidad los primeros 1 GiB. Es el "paging bootstrap" de
 * docs/REAL_HARDWARE.md: el kernel completo vive en esas direcciones, y sin
 * NX porque EFER no existe en modo 32 bits.
 */

namespace i386 {

namespace paging {

/* ELF32 virtual window. Each process maps different physical memory here;
 * the kernel and idle directory never grant user access to the identity map. */
constexpr uint32_t kUserWindowBase = 0x08000000u;
constexpr uint32_t kUserWindowEnd = 0x09000000u;

/* CPUID.01H:EDX.PSE (bit 3): sin esto no hay paginas de 4 MiB. */
bool cpu_has_pse();

/* Identidad 0..1 GiB: CR3 propio + CR4.PSE + CR0.PG|WP. Solo una vez, y
 * nunca con paginacion ya encendida. */
void enable_identity_1gib();

/* Extend the identity map over a physical range, using 4 MiB pages.
 * Framebuffers above the initial 1 GiB map need this. Reject ranges outside
 * the 32-bit address space. */
bool map_identity(uint64_t address, uint32_t bytes);

}

}

namespace i386::paging {
// Each private directory inherits supervisor-only kernel/video mappings.
void initialize_process_directory(uint32_t* directory, uint32_t physical_base);
void activate(const uint32_t* directory);
void activate_kernel();
}
