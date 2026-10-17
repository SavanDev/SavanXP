#pragma once

#include <stdint.h>

/*
 * Senales: vocabulario de POSIX, no del kernel (rebanada D de aislamiento).
 *
 * El kernel no tiene senales. No hay entrega a userland --no existe
 * sigaction ni frame de senal--, asi que lo que hay es:
 *
 *   - kill(pid, n) en la traduccion POSIX: para SIGINT/SIGKILL/SIGTERM
 *     termina el proceso con 128+n, y SIGCHLD es un no-op que devuelve exito.
 *   - El aviso de que un hijo termino, para el padre que espera, es
 *     waitpid, que el subsistema nativo tambien usa (SXN_SYS_WAITPID).
 *
 * Antes el kernel llevaba pending_signals/last_signal y un mapa
 * senal-numero -> bit. Se escribian y NADIE los leia: eran estado muerto
 * cuya unica funcion era nombrar senales POSIX dentro del kernel.
 */

enum PosixSignalFlags : uint32_t {
    posix_signal_none = 0,
    posix_signal_sigint = 1u << 0,
    posix_signal_sigkill = 1u << 1,
    posix_signal_sigpipe = 1u << 2,
    posix_signal_sigterm = 1u << 3,
    posix_signal_sigchld = 1u << 4,
};