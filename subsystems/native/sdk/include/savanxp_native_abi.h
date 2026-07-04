/*
 * SavanXP - contrato ABI del subsistema nativo (Haxe), v1.
 *
 * Este header es la UNICA fuente del contrato kernel <-> userland del
 * subsistema nativo: lo incluyen tanto el dispatcher del kernel
 * (subsystems/native/kernel/syscall_dispatch.inc) como el runtime de userland
 * (savanxp_native.h). Es el contrato que heredara HashLink en la etapa VM:
 * AOT y HashLink apuntan a esta misma superficie.
 *
 * Convencion de llamada (compartida con posix por ahora):
 *   int $0x80, numero en rax, args en rdi/rsi/rdx, resultado en rax
 *   (negativo = -errno de savanxp_error_code).
 *
 * Particion del espacio de numeros:
 *   [0x0000, 0x0fff]  baseline transitorio: se delega en la tabla posix
 *                     (READ/WRITE/EXIT/etc. de savanxp/syscall.h). Un proceso
 *                     nativo puede usarlas mientras el ABI propio crece.
 *   [0x1000, ...)     syscalls PROPIAS del subsistema nativo. Un proceso posix
 *                     que las invoque recibe ENOSYS: solo existen detras de
 *                     dispatch_native_syscall.
 */
#pragma once

#include <stdint.h>

/* Version del contrato. El runtime la verifica contra el kernel via
 * SXN_SYS_INFO al arrancar; si no coincide, el proceso debe abortar. */
#define SXN_ABI_VERSION 1u

/* Base del rango de syscalls propias del subsistema nativo. */
#define SXN_SYS_BASE 0x1000u

enum sxn_syscall_number {
    /* rdi = puntero a struct sxn_native_info (escritura). Devuelve 0. */
    SXN_SYS_INFO = 0x1000,
    /* rdi = puntero a string NUL-terminado (lectura, se trunca a 255 bytes).
     * El kernel lo emite en su log con el pid como prefijo. Devuelve 0. */
    SXN_SYS_LOG = 0x1001,
};

/* Identidad del proceso nativo y del contrato, reportada por SXN_SYS_INFO. */
struct sxn_native_info {
    uint32_t abi_version;  /* SXN_ABI_VERSION del kernel */
    uint32_t subsystem_id; /* subsystem::Id del proceso (1 = native) */
    uint32_t pid;
    uint32_t reserved0;
    uint64_t reserved1;
    uint64_t reserved2;
};
