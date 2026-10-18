#pragma once

/*
 * Paginacion de 32 bits sin PAE: paginas de 4 MiB (CR4.PSE) que mapean en
 * identidad los primeros 1 GiB. Es el "paging bootstrap" de
 * docs/REAL_HARDWARE.md: el kernel completo vive en esas direcciones, y sin
 * NX porque EFER no existe en modo 32 bits.
 */

namespace i386 {

namespace paging {

/* CPUID.01H:EDX.PSE (bit 3): sin esto no hay paginas de 4 MiB. */
bool cpu_has_pse();

/* Identidad 0..1 GiB: CR3 propio + CR4.PSE + CR0.PG|WP. Solo una vez, y
 * nunca con paginacion ya encendida. */
void enable_identity_1gib();

}

}
