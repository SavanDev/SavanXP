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

    /* --- Graficos (bloque 0x1010) -----------------------------------------
     * Acceso de primera clase al display: un proceso nativo no abre /dev/gpu0
     * ni hace ioctls; el display es parte del ABI. Kernel-side comparte los
     * mismos internals (display::*) y la misma sesion exclusiva por pid que
     * los ioctls GPU del mundo posix: un solo dueño de pantalla por vez, y la
     * sesion se libera sola si el proceso muere. */

    /* rdi = puntero a struct sxn_gfx_info (escritura). Devuelve 0 o
     * -ENODEV si no hay display. No requiere sesion. */
    SXN_SYS_GFX_INFO = 0x1010,
    /* Toma la sesion exclusiva de display para este proceso. Devuelve 0 o
     * -EBUSY si otro proceso (p. ej. el compositor) la tiene. */
    SXN_SYS_GFX_ACQUIRE = 0x1011,
    /* Libera la sesion de display de este proceso. Devuelve 0 o -EBUSY si
     * no era el dueno. */
    SXN_SYS_GFX_RELEASE = 0x1012,
    /* rdi = puntero a struct sxn_gfx_present (lectura). Copia la region
     * indicada del buffer del proceso a la pantalla. Requiere sesion.
     * Devuelve 0, -EBUSY sin sesion, -EINVAL con region/buffer invalidos. */
    SXN_SYS_GFX_PRESENT = 0x1013,
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

/* Geometria del display activo, reportada por SXN_SYS_GFX_INFO. Los pixeles
 * son de 32 bits (XRGB, poco endian). */
struct sxn_gfx_info {
    uint32_t width;
    uint32_t height;
    uint32_t pitch;       /* bytes por fila del framebuffer de destino */
    uint32_t bpp;         /* siempre 32 hoy */
    uint32_t buffer_size; /* bytes del framebuffer completo */
    uint32_t reserved0;
};

/* Region a presentar via SXN_SYS_GFX_PRESENT. `pixels` apunta a un frame del
 * proceso con el MISMO layout que la pantalla (filas de `source_pitch` bytes);
 * (x, y, width, height) es el rectangulo sucio que se copia, leyendo del
 * origen en ese mismo offset. Para un frame completo: x=0, y=0, width/height
 * de la pantalla y source_pitch = width * 4. */
struct sxn_gfx_present {
    uint64_t pixels;       /* direccion del buffer origen en el proceso */
    uint32_t source_pitch; /* bytes por fila del buffer origen */
    uint32_t x;
    uint32_t y;
    uint32_t width;
    uint32_t height;
    uint32_t reserved0;
};
