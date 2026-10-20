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

    /* --- Handles and I/O (docs/desktop/DESKTOP_ARCHITECTURE.md, phase 3) ---
     * What a platform service needs to live without POSIX: closing and
     * duplicating handles, reading and writing the pipes it inherits, talking
     * to devices, and a clock. They work on the same per-process handle table
     * as everything else, with the granted access checked at use. */

    /* rdi = handle. Releases it. Returns 0 or -EBADF. */
    SXN_SYS_CLOSE = 0x100c,
    /* rdi = handle. A second handle to the same object with the same granted
     * access. Returns the new handle or -errno. */
    SXN_SYS_DUPLICATE = 0x100d,
    /* rdi = handle, rsi = buffer, rdx = byte count. Pipes and devices only.
     * A pipe read blocks until data arrives (unless the handle is
     * non-blocking) and returns 0 at end of stream. Returns bytes or -errno. */
    SXN_SYS_READ = 0x100e,
    /* rdi = handle, rsi = buffer, rdx = byte count. Pipes and devices only.
     * A pipe write blocks until it fits; up to the pipe's capacity it is
     * written whole, never split. Returns bytes or -errno (-EPIPE with no
     * reader). */
    SXN_SYS_WRITE = 0x100f,

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

    /* --- Devices and time (bloque 0x1014) ---------------------------------
     * The GFX block above is the whole-screen path for a simple program. A
     * service that drives the hardware -- compositord with the GPU, windowd
     * with the input devices -- opens the device and sends it the same control
     * requests the POSIX side sends through ioctl, as NT's
     * NtDeviceIoControlFile does. */

    /* rdi = device name (user string, letters and digits only: "gpu0",
     * "input0", "mouse0"), rsi = access (SXN_DEVICE_READ | SXN_DEVICE_WRITE).
     * Returns a device handle or -errno (-ENOENT if there is no such device). */
    SXN_SYS_OPEN_DEVICE = 0x1014,
    /* rdi = device handle, rsi = control code (the device's *_IOC_*), rdx =
     * argument. Returns what the device returns: >= 0 or -errno. */
    SXN_SYS_DEVICE_CONTROL = 0x1015,
    /* No arguments. Monotonic time in nanoseconds since boot (0 before the
     * clock is calibrated). */
    SXN_SYS_CLOCK = 0x1016,

    /* --- Ports (bloque 0x1017) --------------------------------------------
     * How a process finds a service without inheriting a channel from it, as
     * CSRSS's \\Windows\\ApiPort does. The service creates a named port; a
     * client connects by name and gets two pipes (one to write requests, one
     * to read replies and receive handles); the service accepts and gets the
     * other ends plus the client's pid. POSIX processes may make these calls
     * too: the native object API is the floor every subsystem stands on. */

    /* rdi = name (user string, 1..31 bytes). Returns a port handle or -errno
     * (-EEXIST if the name is taken). The name lives until the last handle to
     * the port is closed. The port is waitable: signaled while connections
     * are pending. */
    SXN_SYS_PORT_CREATE = 0x1017,
    /* rdi = name, rsi = int32_t[2] (written): [0] the write end of the
     * client->service pipe, [1] the read end of the service->client pipe.
     * Returns 0 or -errno (-ENOENT if no such port). Does not wait for the
     * service: reading [1] does. A port that goes away unaccepted shows as
     * end of stream on [1]. */
    SXN_SYS_PORT_CONNECT = 0x1018,
    /* rdi = port handle, rsi = struct sxn_port_connection (written). Takes the
     * oldest pending connection. Returns 0 or -EAGAIN if none; wait on the
     * port to know when one arrives. */
    SXN_SYS_PORT_ACCEPT = 0x1019,
    /* rdi = pipe write end, rsi = handle. Queues a reference to the handle's
     * object, with the sender's granted access, for whoever reads the pipe
     * (sections, events, semaphores, timers, ports: never a stream). Returns
     * 0, -EAGAIN with the queue full, -EPIPE with no reader. Same queue as
     * the POSIX pipe_send_handle. */
    SXN_SYS_PIPE_SEND_HANDLE = 0x101a,
    /* rdi = pipe read end. Pops the oldest queued handle into the caller's
     * table. Returns the handle or -EAGAIN if none; never blocks. */
    SXN_SYS_PIPE_RECEIVE_HANDLE = 0x101b,
    /* rdi = pid, rsi = exit status. Ends a child of the caller, which its
     * waitpid then reaps with that status. Children only: having created the
     * process is the right to end it, as NT asks for a handle with terminate
     * access. Returns 0 (also for a child that already ended), -ENOENT if no
     * such process, -ECHILD if it is not the caller's child. Native only. */
    SXN_SYS_TERMINATE = 0x101c,
    /* rdi = const int32_t handles[], rsi = count (1..16), rdx = timeout in ms
     * (0 = poll, negative = forever). Waits until ANY of them is signaled and
     * returns its index, or -ETIMEDOUT. A handle is an event, timer,
     * semaphore or port -- consumed as WAIT_ONE consumes it -- or a stream
     * opened for reading (pipe, device, socket), signaled while a read would
     * not block: data, or end of stream. Streams consume nothing. */
    SXN_SYS_WAIT_MANY = 0x101d,
    /* rdi = pid, rsi = struct sxn_process_query (written). Returns 0 or
     * -ENOENT if no process has that pid (a reaped one included). Any process
     * may ask about any other: it is what a task list shows anyway. */
    SXN_SYS_PROCESS_QUERY = 0x101e,
    /* Like SXN_SYS_SPAWN, but the child gets only the caller's stdio (0, 1,
     * 2), as the POSIX spawn does. What a service uses to start a program it
     * has no handles to give: anything else it holds -- a client's channels,
     * above all -- would stay alive in the child and outlive the service's
     * own close. */
    SXN_SYS_SPAWN_STDIO = 0x101f,
    /* rdi = handle of a stream, rsi = 1 or 0. Reads and writes on it stop
     * blocking: a read with nothing to read and a write with no room return
     * -EAGAIN (a write that fits the pipe goes whole or not at all). The mode
     * belongs to the stream, so every handle to it shares it, as POSIX
     * O_NONBLOCK does. */
    SXN_SYS_SET_NONBLOCKING = 0x1020,
};

enum sxn_process_state {
    SXN_PROCESS_RUNNING = 1, /* alive: running, ready or blocked */
    SXN_PROCESS_ENDED = 2,   /* exited or ended, waiting for its parent's waitpid */
};

struct sxn_process_query {
    uint32_t pid;
    uint32_t parent_pid;
    uint32_t state;        /* enum sxn_process_state */
    int32_t exit_code;     /* valid when SXN_PROCESS_ENDED */
    uint32_t handle_count; /* open handles */
    uint32_t reserved;
    char name[32];         /* file name of its image */
};

struct sxn_port_connection {
    uint32_t client_pid;
    int32_t read_handle;  /* client -> service pipe, read end */
    int32_t write_handle; /* service -> client pipe, write end */
    uint32_t reserved;
};

/* Access for SXN_SYS_OPEN_DEVICE. */
enum sxn_device_access {
    SXN_DEVICE_READ = 1u << 0,
    SXN_DEVICE_WRITE = 1u << 1,
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
