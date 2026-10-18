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

/* Ventana de usuario del MVP: cuatro PDEs de 4 MiB (32..35) con el bit de
 * usuario puesto, donde se enlacean las imagenes ELF32 y donde vive la pila
 * del proceso. Comparte paginas con la identidad del kernel, asi que entrar en
 * ring 3 no invalida TLB. */
constexpr uint32_t kUserWindowBase = 0x08000000u;
constexpr uint32_t kUserWindowEnd = 0x09000000u;

/* CPUID.01H:EDX.PSE (bit 3): sin esto no hay paginas de 4 MiB. */
bool cpu_has_pse();

/* Identidad 0..1 GiB: CR3 propio + CR4.PSE + CR0.PG|WP. Solo una vez, y
 * nunca con paginacion ya encendida. */
void enable_identity_1gib();

}

}
