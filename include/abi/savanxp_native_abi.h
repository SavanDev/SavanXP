/*
 * SavanXP - native subsystem ABI contract, v1.
 *
 * This header is the single source of the kernel <-> userland contract of
 * the native subsystem: it is included by both the kernel dispatcher
 * (kernel/native_dispatch.cpp) and native userland.
 *
 * Calling convention (shared with posix for now):
 *   int $0x80, numero en rax, args en rdi/rsi/rdx, resultado en rax
 *   (negativo = -errno de savanxp_error_code).
 *
 * Number space:
 *   [0x0000, 0x0fff]  reserved. A native process only speaks SXN: invoking
 *                     any of these numbers returns ENOSYS.
 *   [0x1000, ...)     syscalls PROPIAS del subsistema nativo. Un proceso posix
 *                     que las invoque recibe ENOSYS: solo existen detras de
 *                     dispatch_native_syscall.
 */
#pragma once

#include <stdint.h>

/* e_ident[EI_OSABI] value for SavanXP native executables. */
#define SXN_ELF_OSABI_NATIVE 0x53u /* 'S' de SavanXP */

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
    /* rdi = codigo de salida. Termina el proceso actual; no retorna. Es el
     * ciclo de vida minimo standalone: con INFO/LOG/GFX mas EXIT un proceso
     * nativo ya no necesita la tabla posix para nada. */
    SXN_SYS_EXIT = 0x1002,
    /* rdi = path (string de usuario), rsi = argv (arreglo de strings de
     * usuario), rdx = argc. Misma convencion que el spawn POSIX. Devuelve
     * el pid del hijo o -errno. La imagen del hijo decide su subsistema por
     * EI_OSABI, asi que un proceso nativo engendra hijos POSIX y nativos; a
     * diferencia del spawn POSIX, el hijo hereda TODA la tabla de handles
     * del padre (sin un fork nativo es el unico canal que los lleva). */
    SXN_SYS_SPAWN = 0x1003,
    /* rdi = pid o -1 (cualquier hijo), rsi = direccion de estado (0 ignora).
     * Devuelve el pid cosechado o -errno; bloquea como el waitpid POSIX. */
    SXN_SYS_WAITPID = 0x1004,
    /* rdi = banderas (enum sxn_event_flags). Devuelve el handle o -errno.
     * Los handles salen de la misma tabla por proceso que los fds POSIX,
     * con granted access: un hijo los hereda al hacer spawn. */
    SXN_SYS_EVENT_CREATE = 0x1005,
    /* rdi = handle de evento. Devuelven 0 o -errno. */
    SXN_SYS_EVENT_SET = 0x1006,
    SXN_SYS_EVENT_RESET = 0x1007,
    /* rdi = handle, rsi = plazo en ms (0 = sondeo, negativo = infinito).
     * Devuelve 0, -ETIMEDOUT o -errno; bloquea como el wait POSIX. */
    SXN_SYS_WAIT_ONE = 0x1008,
    /* rdi = tamano en bytes, rsi = banderas (enum sxn_section_flags).
     * Devuelve el handle de seccion o -errno. */
    SXN_SYS_SECTION_CREATE = 0x1009,
    /* rdi = handle de seccion, rsi = banderas (acceso + SXN_VIEW_PRIVATE).
     * Devuelve la direccion base o -errno. */
    SXN_SYS_MAP_VIEW = 0x100a,
    /* rdi = direccion base. Devuelve 0 o -errno. */
    SXN_SYS_UNMAP_VIEW = 0x100b,

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

/* Banderas de SXN_SYS_EVENT_CREATE. Los valores coinciden con los del lado
 * POSIX a proposito (el kernel los consume por los mismos bits); el contrato
 * nativo es dueno de estos nombres. */
enum sxn_event_flags {
    SXN_EVENT_MANUAL_RESET = 1u << 0,
    SXN_EVENT_INITIAL_SET = 1u << 1,
};

/* Banderas de SXN_SYS_SECTION_CREATE y SXN_SYS_MAP_VIEW. Idem: mismos valores
 * que el lado POSIX, nombres propios. SXN_VIEW_PRIVATE pide vista privada;
 * sin el, la vista se comparte en el fork. */
enum sxn_section_flags {
    SXN_SECTION_READ = 1u << 0,
    SXN_SECTION_WRITE = 1u << 1,
    SXN_SECTION_EXEC = 1u << 2,
};

enum sxn_view_flags {
    SXN_VIEW_PRIVATE = 1u << 8,
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
