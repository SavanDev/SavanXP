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
    /* rdi = NUL-terminated string (read, cut to 255 bytes). The kernel writes
     * it to the system log as one line, prefixed by the caller's subsystem and
     * pid ("native[4]: ...", "posix[7]: ..."). Returns 0. Open to every
     * subsystem: POSIX's syslog(3) is built on it, and the shared-library
     * loader records through it what a program should know it got. */
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
    /* rdi = handle, rsi = buffer, rdx = byte count. Regular files, pipes, devices and TCP sockets.
     * Socket I/O is nonblocking and may return a partial count or -EAGAIN.
     * File reads advance the shared offset and return 0 at EOF.
     * A pipe read blocks until data arrives (unless the handle is
     * non-blocking) and returns 0 at end of stream. Returns bytes or -errno. */
    SXN_SYS_READ = 0x100e,
    /* rdi = handle, rsi = buffer, rdx = byte count. Pipes, devices and TCP sockets.
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
    /* rdi = pid, rsi = exit status. Ends a process, which its parent's
     * waitpid then reaps with that status. Allowed for a descendant of the
     * caller (having started the process tree is the right to end any process
     * in it, as a job object gives Windows), for a process of the caller's
     * own uid, and for any process when the caller acts as the system --
     * kill's rule (docs/kernel/USERS_AND_PERMISSIONS.md#what-requires-
     * elevation). Returns 0 (also for a process that already ended), -ENOENT
     * if no such process, -EACCES for the idle processes and init, which
     * nobody ends, -EINVAL for the caller itself (that is SXN_SYS_EXIT), and
     * -EPERM otherwise. Native only. */
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
     * may ask about any other: it is what a task list shows anyway. A process
     * ended by SXN_SYS_TERMINATE reads as ended at once, with that status,
     * even while the core it was running on is still tearing it down. */
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
    /* rdi = int32_t[2] (written): [0] the read end, [1] the write end of a new
     * anonymous pipe. Returns 0 or -errno. CreatePipe. An object call, so
     * every subsystem may make it. */
    SXN_SYS_PIPE_CREATE = 0x1021,
    /* rdi = const struct sxn_spawn_request*. Starts a child with exactly the
     * handles the request names: std_handles as its 0, 1 and 2 (-1 = the
     * caller's own 0, 1 or 2) and handles[0..handle_count) as its 3, 4, ...
     * Nothing else crosses. CreateProcess with STARTUPINFO's standard handles
     * and an inherit list. Returns the pid or -errno (-EBADF when a named
     * handle is not open). Native only. */
    SXN_SYS_SPAWN_EX = 0x1022,
    /* rdi = entry point, rsi = argument (the new thread's rdi), rdx = top of
     * a stack the caller allocated, r10 = uint32_t* for the thread id (0 =
     * not wanted). Starts a thread of the calling process at the entry, as if
     * called: rsp is 8 below a 16-byte boundary under the given top. The
     * entry never returns; the thread ends with SXN_SYS_THREAD_EXIT. Returns a
     * handle to the thread (synchronize + query), signaled when it ends, or
     * -errno. CreateThread. Open to every subsystem: POSIX builds pthreads
     * on it (docs/roadmaps/done/THREADS_ROADMAP.md).
     * Threads of one process run on several cores at once. */
    SXN_SYS_THREAD_CREATE = 0x1023,
    /* rdi = exit code. Ends the calling thread; does not return. The last
     * thread of a process ends the process with that code, as SXN_SYS_EXIT
     * would. SXN_SYS_EXIT from any thread ends them all. ExitThread. */
    SXN_SYS_THREAD_EXIT = 0x1024,
    /* rdi = thread handle, rsi = int32_t* (written). The exit code of an ended
     * thread: returns 0, -EAGAIN while it still runs, -EBADF when the handle
     * is not a thread. GetExitCodeThread. */
    SXN_SYS_THREAD_EXIT_CODE = 0x1025,
    /* rdi = which base (SXN_THREAD_BASE_FS or SXN_THREAD_BASE_GS), rsi = an
     * address in user space, or 0. Sets that segment base of the calling
     * thread: %fs- or %gs-relative accesses read from there, on whichever
     * core the thread runs. Every thread starts with both at 0, exec resets
     * them and fork copies them. Returns 0, or -EINVAL for another `which` or
     * an address outside user space. arch_prctl(ARCH_SET_FS) for POSIX TLS;
     * Win64 keeps its TEB at %gs. Open to every subsystem. */
    SXN_SYS_SET_THREAD_BASE = 0x1026,
    /* rdi = address of a word of rdx bytes (1, 2, 4 or 8, aligned to its
     * size), rsi = the value it is expected to hold, r10 = timeout in ms
     * (negative = none, 0 = do not park). Parks the calling thread while the
     * word holds that value, until SXN_SYS_WAKE_ADDRESS names it. Returns 0
     * once woken, -EAGAIN when the word already differs, -ETIMEDOUT, or
     * -EINVAL for a bad size, alignment or address. A woken thread re-reads
     * the word: being woken says the word may have changed, not that it did.
     * WaitOnAddress, futex(FUTEX_WAIT). Open to every subsystem. */
    SXN_SYS_WAIT_ADDRESS = 0x1027,
    /* rdi = address, rsi = how many waiters to release (SXN_WAKE_ALL for
     * every one). Releases threads of the calling process parked on that
     * address, oldest first, and returns how many. Waiters are keyed by
     * process and virtual address: a thread of another process is never
     * reached, wherever it maps the word. WakeByAddressSingle/All,
     * futex(FUTEX_WAKE). Open to every subsystem. */
    SXN_SYS_WAKE_ADDRESS = 0x1028,
    /* Like SXN_SYS_PORT_CONNECT, plus rdx = struct sxn_identity* (written):
     * who created the port, as they were when they created it. A client that
     * trusts an answer -- the elevation broker asking windowd for consent --
     * checks that the port is the system's, since names are first come,
     * first served (docs/kernel/USERS_AND_PERMISSIONS.md#elevation). */
    SXN_SYS_PORT_CONNECT_EX = 0x1029,
    /* Like SXN_SYS_PORT_ACCEPT, with rsi = struct sxn_port_connection_ex:
     * also who the client was when it connected. SO_PEERCRED. */
    SXN_SYS_PORT_ACCEPT_EX = 0x102a,
    /* rdi = index, rsi = struct savanxp_process_info (written), rdx = its
     * size. The index-th live or ended process, in table order, as a task
     * list shows it: counters, threads and owner included. Writes at most
     * `size` bytes, so a caller built against a shorter struct gets its
     * prefix. Returns 1, 0 past the last process, or -EINVAL if the size is
     * below SAVANXP_PROCESS_INFO_V1_SIZE. Any process may list every other.
     * Toolhelp's snapshot, one entry per call. */
    SXN_SYS_PROCESS_SNAPSHOT = 0x102b,
    /* rdi = struct savanxp_system_info (written), rsi = its size. The machine
     * as the boot found it plus what is live now: free memory, the global
     * tick counter, the cores that schedule. Writes at most `size` bytes.
     * Returns 0 or -EINVAL. GlobalMemoryStatus and GetSystemInfo in one. */
    SXN_SYS_SYSTEM_INFO = 0x102c,
    /* rdi = index, rsi = struct sxn_storage_device (written), rdx = its size.
     * The index-th storage device (disks, their partitions, the LiveCD's
     * memory volume), what is on it and where it is mounted. Writes at most
     * `size` bytes. Returns 1, 0 past the last device, or -EINVAL if the size
     * is below SXN_STORAGE_DEVICE_V1_SIZE. Any process may list them; reaching
     * their sectors is the system's (docs/userland/DISK_MANAGEMENT.md).
     * SetupDiGetClassDevs and IOCTL_DISK_GET_DRIVE_LAYOUT_EX, one entry per
     * call. */
    SXN_SYS_STORAGE_SNAPSHOT = 0x102d,
    /* rdi = device id, rsi = first sector, rdx = sector count (1 to
     * SXN_STORAGE_MAX_TRANSFER), r10 = buffer. Reads 512-byte sectors of the
     * device. The system only (-EPERM). 0, -ENODEV for an unknown id, -EINVAL
     * for a range off the device, -EIO. */
    SXN_SYS_STORAGE_READ = 0x102e,
    /* As STORAGE_READ, writing. Durable when it returns. -EBUSY when the range
     * overlaps a mounted volume on the same disk: the device itself, the disk
     * it is on, or a partition inside it. */
    SXN_SYS_STORAGE_WRITE = 0x102f,
    /* rdi = disk id. Reads the disk's partition table again: the old
     * partitions go (their ids are gone for good) and new ones appear. The
     * system only. -EBUSY if the disk or a partition of it is mounted,
     * -EINVAL for a partition. */
    SXN_SYS_STORAGE_RESCAN = 0x1030,
    /* rdi = device id, rsi = const char* path, rdx = flags (0). Mounts the
     * device at /mnt/<name> ([A-Za-z0-9._-], 1 to 24 characters, not there
     * yet; /mnt is made if missing). The system only. 0, -ENODEV, -EINVAL (the
     * path, the flags, or a partitioned disk), -EEXIST (the path exists),
     * -EBUSY (it or an overlapping device is mounted), -EIO (not a volume any
     * driver knows, or no slot left). SetVolumeMountPoint. */
    SXN_SYS_STORAGE_MOUNT = 0x1031,
    /* rdi = const char* mount point. Syncs and releases the volume. The
     * system only. -EINVAL for "/" or nothing mounted there, -EBUSY while a
     * file of it is open or a program of it is mapped. DeleteVolumeMountPoint. */
    SXN_SYS_STORAGE_UNMOUNT = 0x1032,
    /* rdi = absolute path, rsi = SXN_FILE_READ, SXN_FILE_WRITE, both, or 0
     * (query only). Opens an existing regular file at offset 0, never
     * emptying it; directories return -EISDIR, devices
     * and invalid flags/paths -EINVAL. Checks VFS credentials. READ/CLOSE
     * use the returned handle; DUPLICATE shares its current offset. Open
     * to every subsystem: a POSIX process gets a descriptor it can read,
     * close and back a section with (SECTION_OPEN_RANGE), which is what a
     * loader shared by SUN and POSIX opens a library with. */
    SXN_SYS_OPEN_FILE = 0x1033,
    /* rdi = absolute path, rsi = bytes, rdx = length (0..4096).
     * Creates (0600) or replaces a regular file and syncs its volume.
     * Parent must exist; VFS credentials apply. Returns 0 or -errno.
     * An I/O failure may leave incomplete contents. */
    SXN_SYS_WRITE_FILE = 0x1034,
    /* Native IPv4 sockets. TCP data/ACK waits return -EAGAIN; retry after
     * readiness. NIC initialization and ARP may still wait in the core.
     * No POSIX process state is allocated. */
    SXN_SYS_SOCKET_CREATE = 0x1035, /* rdi = SXN_SOCKET_TCP or UDP */
    SXN_SYS_SOCKET_BIND = 0x1036, /* rdi = handle, rsi = sxn_ipv4_endpoint* */
    SXN_SYS_SOCKET_CONNECT = 0x1037, /* same; -EAGAIN while SYN is pending */
    SXN_SYS_SOCKET_STATUS = 0x1038, /* rdi = handle; 0 connected, -EAGAIN pending */
    /* rdi = handle, rsi = sxn_datagram*. Returns bytes or -errno. */
    SXN_SYS_SOCKET_SENDTO = 0x1039,
    SXN_SYS_SOCKET_RECVFROM = 0x103a,
    /* rdi = sxn_io_interest[], rsi = count, rdx = timeout ms (negative
     * infinite). Returns the first ready index or -ETIMEDOUT. Events use
     * interests=0 and are consumed just as by WAIT_MANY. */
    SXN_SYS_WAIT_IO = 0x103b,
    /* rdi = first sequence wanted, rsi = struct sxn_log_record[] (written),
     * rdx = how many records fit, r10 = sizeof(struct sxn_log_record).
     * Copies the system log's complete lines, oldest first, starting at the
     * first retained record whose sequence is >= rdi; an overwritten sequence
     * starts at the oldest record. The kernel's and the system's lines are
     * open to everyone, another user's programs' lines are skipped, so a gap
     * in the sequences is not necessarily a loss. Writes at most
     * r10 bytes per record. Returns the count (0 = nothing newer), or -EINVAL
     * for a count of 0 or a size below SXN_LOG_RECORD_V1_SIZE.
     * docs/kernel/SYSTEM_LOG.md. */
    SXN_SYS_LOG_READ = 0x103c,
    /* rdi = absolute path, rsi = struct sxn_file_info (written), rdx = its
     * size. What is at the path: its type, size, date, owner and permission
     * bits. Needs only to reach it (search on every directory above), as
     * POSIX stat. Writes at most `size` bytes. 0, -ENOENT, -EACCES, or
     * -EINVAL for a relative path or a size below SXN_FILE_INFO_V1_SIZE.
     * GetFileAttributesEx. */
    SXN_SYS_QUERY_PATH = 0x103d,
    /* rdi = absolute path. Opens a directory for listing with
     * READ_DIRECTORY; needs read permission on it. A handle, -ENOTDIR for
     * anything else, -ENOENT, -EACCES, -EINVAL. FindFirstFile's open half.
     * Open to every subsystem, with READ_DIRECTORY: the shared-library loader
     * lists /lib with them to find a newer library than the one a program
     * names, in a POSIX process as in a native one. */
    SXN_SYS_OPEN_DIRECTORY = 0x103e,
    /* rdi = directory handle, rsi = struct sxn_directory_entry (written),
     * rdx = its size. The next entry, in no particular order and without
     * "." or "..". An entry removed while listing is never returned after
     * its removal and does not make the listing skip another; an entry added
     * meanwhile may or may not appear. 1, 0 past the last entry, -EBADF for a
     * handle that is not a directory, -EINVAL for a size below
     * SXN_DIRECTORY_ENTRY_V1_SIZE. FindNextFile. */
    SXN_SYS_READ_DIRECTORY = 0x103f,
    /* rdi = absolute path, rsi = SXN_CREATE_NEW or SXN_CREATE_ALWAYS, with
     * SXN_CREATE_EXECUTABLE or not.
     * Opens a regular file for writing from offset 0: a missing one is
     * created (0600, the parent must exist; 0700 with CREATE_EXECUTABLE, a
     * program its owner can run), an existing one is emptied
     * (CREATE_ALWAYS, keeping its owner and mode) or refused (CREATE_NEW,
     * -EEXIST). WRITE appends what it is given; FLUSH_FILE makes it durable.
     * A handle, -EISDIR, -EINVAL (a device, a relative path, bad flags),
     * -ENOENT (no parent), -EACCES, -EROFS. CreateFile. */
    SXN_SYS_CREATE_FILE = 0x1040,
    /* rdi = handle of a file open for writing. Writes the file's volume
     * through to the disk before returning. 0, -EBADF, -EIO.
     * FlushFileBuffers. */
    SXN_SYS_FLUSH_FILE = 0x1041,
    /* rdi = section handle, rsi = base address, rdx = flags (as MAP_VIEW).
     * Maps the view with its first byte at base: what a loader needs to put
     * an image where its headers say. The base is page-aligned and the whole
     * range free, inside the user half and below the stack guard; base 0
     * lets the kernel choose, as MAP_VIEW. Returns base, or -EINVAL
     * (misaligned or outside the user half, or writable and executable),
     * -EEXIST (something is mapped in the range), -EACCES, -EBADF, -ENOMEM.
     * Same call as the POSIX map_view_at. MapViewOfFileEx. */
    SXN_SYS_MAP_VIEW_AT = 0x1042,
    /* No arguments. The date: seconds since 1970-01-01 UTC, read from the
     * RTC once and carried by the monotonic clock since, or 0 when the
     * machine never gave a date. There are no time zones (docs/kernel/
     * TIME.md). The same clock as POSIX realtime() and file times.
     * GetSystemTimeAsFileTime. */
    SXN_SYS_SYSTEM_TIME = 0x1043,
    /* rdi = handle of a regular file open for reading, rsi = offset,
     * rdx = length (0 = to the end of the file), r10 = flags (enum
     * sxn_section_flags; READ and EXEC, never WRITE). A section backed by
     * that range of the file, read whole when created. A range of a file on
     * a volume is one section system-wide: a second open of the same file,
     * offset and length, from any process, gets the section already alive,
     * which is how a loader shares a library's text. Returns the section
     * handle, or -EBADF (not a file handle, or not open for reading),
     * -EINVAL (not a regular file, WRITE asked, or a range that is empty
     * or falls outside the file), -ENOMEM. Same call as the POSIX
     * section_open_range. CreateFileMapping over a file. */
    SXN_SYS_SECTION_OPEN_RANGE = 0x1044,
    /* rdi = absolute path. Makes a directory, 0700 like the files SUN
     * creates, and syncs its volume. 0, -EEXIST, -ENOENT (no parent),
     * -EINVAL (a relative path), -EACCES, -EROFS. CreateDirectory. */
    SXN_SYS_MAKE_DIRECTORY = 0x1045,
    /* rdi = absolute old path, rsi = absolute new path. Renames a file or a
     * directory within its volume, replacing a file already at the new
     * path, and syncs the volume: written whole under a temporary name and
     * then renamed over the old one, a file is never left half saved. 0,
     * -ENOENT, -EXDEV (another volume), -EISDIR or -ENOTDIR (a file over a
     * directory or the other way), -EINVAL, -EACCES, -EROFS, -EBUSY (FAT, a
     * file open). MoveFileEx with REPLACE_EXISTING and WRITE_THROUGH. */
    SXN_SYS_RENAME_PATH = 0x1046,
    /* rdi = struct sxn_identity* (written): who the calling process is, its
     * real uid (the person), effective uid (0 acts as the system) and gid,
     * as POSIX's GET_IDENTITY reports them. 0, or -EINVAL for a bad pointer.
     * OpenProcessToken + GetTokenInformation(TokenUser). */
    SXN_SYS_QUERY_IDENTITY = 0x1047,
    /* rdi = handle of a regular file open for reading or writing, rsi =
     * offset, rdx = length (not 0), r10 = flags (SXN_LOCK_*). Locks that
     * byte range for the open file: the handle, its duplicates and whoever
     * inherited it (docs/kernel/FILE_LOCKS.md). Shared unless
     * SXN_LOCK_EXCLUSIVE; an exclusive range conflicts with any other lock
     * over it, the same open file's included. Waits for a conflicting range
     * to free, or returns -EAGAIN at once with SXN_LOCK_FAIL_IMMEDIATELY.
     * Advisory: reads and writes do not look at it. 0, -EAGAIN, -EBADF,
     * -EINVAL (no length, a range past 2^64, unknown flags), -ENOLCK. The
     * lock goes with the open file's last handle. LockFileEx. */
    SXN_SYS_LOCK_FILE = 0x1048,
    /* rdi = handle, rsi = offset, rdx = length: exactly a range the open
     * file locked. 0, -ENOENT (it holds none there), -EBADF, -EINVAL.
     * UnlockFileEx. */
    SXN_SYS_UNLOCK_FILE = 0x1049,
    /* rdi = buffer (written), rsi = length (at most SXN_RANDOM_MAX), rdx =
     * flags (0). Fills it from the kernel's cryptographic generator
     * (docs/kernel/RANDOM.md). Returns the length, -EAGAIN while the
     * generator is not seeded -- no entropy source has given 256 bits yet,
     * and on a machine without one it never will -- or -EINVAL for a bad
     * buffer, a length past the maximum or unknown flags. Nothing weaker is
     * ever handed out instead. BCryptGenRandom. */
    SXN_SYS_RANDOM_BYTES = 0x104A,
    /* rdi = struct sxn_random_status* (written): whether the generator is
     * seeded, from which sources, and how many times it reseeded. 0, or
     * -EINVAL for a bad pointer. */
    SXN_SYS_RANDOM_STATUS = 0x104B,
    /* rdi = struct sxn_clock_status* (written): whether the date can be
     * believed -- what certificate dates are checked against -- where it
     * came from, and why not when it is not (docs/kernel/TIME.md). 0, or
     * -EINVAL for a bad pointer. */
    SXN_SYS_CLOCK_STATUS = 0x104C,
    /* rdi = absolute path. Deletes a file -- not a directory -- and syncs its
     * volume; a handle still open to it keeps reading and writing what it
     * had. 0, -ENOENT, -EISDIR, -EINVAL (a relative path), -EACCES, -EROFS,
     * -EBUSY (FAT, the file open). DeleteFile. */
    SXN_SYS_DELETE_PATH = 0x104D,
    /* rdi = struct sxn_device_info[] (written), rsi = how many fit, rdx =
     * sizeof(struct sxn_device_info). The hardware inventory in one
     * consistent view (docs/kernel/DEVICE_INVENTORY.md): the computer, every
     * PCI function, the platform devices, every USB device and interface,
     * parents before their children, all with the same generation. Writes at
     * most rdx bytes per record and at most rsi records. Returns how many
     * instances there are, which is more than rsi when they did not fit (call
     * again with room); rsi = 0 only counts. -EINVAL for a size below
     * SXN_DEVICE_INFO_V1_SIZE or a bad buffer. Any process may read it.
     * SetupDiGetClassDevs + SetupDiGetDeviceRegistryProperty. */
    SXN_SYS_DEVICE_SNAPSHOT = 0x104E,
    /* rdi = file handle, rsi = offset (signed), rdx = SXN_SEEK_START,
     * SXN_SEEK_CURRENT or SXN_SEEK_END. Moves the handle's position, which
     * its duplicates share; past the end is allowed, and a write there fills
     * the gap with zeros. The new position, -EBADF (not a file handle),
     * -EISDIR, -EINVAL (before 0, past 2^63, a bad origin). SetFilePointerEx. */
    SXN_SYS_SEEK_FILE = 0x104F,
    /* rdi = handle of a file open for writing, rsi = size. Cuts the file
     * there or grows it with zeros; the position does not move. 0, -EBADF,
     * -ENOSPC, -EIO. SetEndOfFile. */
    SXN_SYS_SET_FILE_SIZE = 0x1050,
    /* rdi = file handle (any access, query included), rsi = struct
     * sxn_file_info (written), rdx = its size. QUERY_PATH's answer for an
     * open file, a file unlinked while open included. 0, -EBADF, -EINVAL.
     * GetFileInformationByHandle. */
    SXN_SYS_QUERY_FILE = 0x1051,
    /* rdi = absolute path. Deletes an empty directory and syncs its volume.
     * 0, -ENOENT, -ENOTDIR, -ENOTEMPTY, -EBUSY (open, or a process's
     * current directory), -EINVAL (a relative path or "/"), -EACCES,
     * -EROFS. RemoveDirectory. */
    SXN_SYS_DELETE_DIRECTORY = 0x1052,
};

/* Where the date came from (sxn_clock_status.source). */
enum sxn_clock_source {
    SXN_CLOCK_SOURCE_NONE = 0, /* the machine never gave a date */
    SXN_CLOCK_SOURCE_RTC = 1,  /* the chip's, read at boot */
    SXN_CLOCK_SOURCE_SET = 2,  /* set since boot (date, ntpsync) */
};

/* Why the date is not valid (sxn_clock_status.reason). */
enum sxn_clock_reason {
    SXN_CLOCK_OK = 0,
    SXN_CLOCK_NO_DATE = 1,
    SXN_CLOCK_BEFORE_FLOOR = 2,  /* earlier than this system's release */
    SXN_CLOCK_AFTER_CEILING = 3, /* 2100 or later, past what the chip holds */
};

/* SXN_SYS_CLOCK_STATUS. The same layout as POSIX's savanxp_clock_status. */
struct sxn_clock_status {
    uint32_t valid;  /* 1 when reason is SXN_CLOCK_OK */
    uint32_t source; /* enum sxn_clock_source */
    uint32_t reason; /* enum sxn_clock_reason */
    uint32_t reserved;
    uint64_t now;    /* seconds since 1970 UTC, 0 without a date */
    uint64_t floor;  /* the earliest valid date, seconds since 1970 UTC */
};

/* SXN_SYS_RANDOM_BYTES's largest request. */
#define SXN_RANDOM_MAX 4096u

/* Where the generator's entropy came from (sxn_random_status.sources). Only
 * VIRTIO_RNG and RDSEED are credited; RDRAND and TIMING are mixed in and
 * never counted toward the seed. */
enum sxn_random_source {
    SXN_RANDOM_SOURCE_VIRTIO_RNG = 1u << 0,
    SXN_RANDOM_SOURCE_RDSEED = 1u << 1,
    SXN_RANDOM_SOURCE_RDRAND = 1u << 2,
    SXN_RANDOM_SOURCE_TIMING = 1u << 3,
};

/* SXN_SYS_RANDOM_STATUS. The same layout as POSIX's savanxp_random_status. */
struct sxn_random_status {
    uint32_t seeded;    /* 1 once 256 credited bits arrived */
    uint32_t sources;   /* enum sxn_random_source bits */
    uint32_t pool_bits; /* credited bits waiting for the next reseed */
    uint32_t reseeds;
};

/* SXN_SYS_LOG_READ. One line of the system log as it left for the serial
 * port, without the line break. Grows at the end only. */
#define SXN_LOG_TEXT_MAX 200u
#define SXN_LOG_SOURCE_MAX 32u
/* Sequences 1 to SXN_LOG_BOOT_KEPT, the first lines since boot, are never
 * overwritten; the ring only recycles the ones after them. */
#define SXN_LOG_BOOT_KEPT 512u

enum sxn_log_flags {
    /* The text continues the previous record of the same source: the line
     * was longer than SXN_LOG_TEXT_MAX - 1 characters. */
    SXN_LOG_CONTINUED = 1u << 0,
    /* The line was committed without its line break: its writer stopped
     * mid-line and the slot was needed by another writer. */
    SXN_LOG_UNTERMINATED = 1u << 1,
};

struct sxn_log_record {
    uint64_t sequence;  /* 1, 2, 3...; never reused while the system runs */
    uint64_t time_ns;   /* monotonic clock at the line's first character; 0 before it is calibrated */
    uint32_t pid;       /* 0 = the kernel */
    uint32_t flags;     /* enum sxn_log_flags */
    char source[SXN_LOG_SOURCE_MAX]; /* "kernel", or the program's name when it wrote */
    char text[SXN_LOG_TEXT_MAX];     /* NUL-terminated */
};

#define SXN_LOG_RECORD_V1_SIZE 256u

#define SXN_WRITE_FILE_MAX 4096u

/* Desired access for OPEN_FILE. Zero grants query access only. */
#define SXN_FILE_READ 1u
#define SXN_FILE_WRITE 2u

/* Origins for SEEK_FILE. */
#define SXN_SEEK_START 0u
#define SXN_SEEK_CURRENT 1u
#define SXN_SEEK_END 2u

/* SXN_SYS_QUERY_PATH and SXN_SYS_READ_DIRECTORY. Both grow at the end only. */
enum sxn_file_type {
    SXN_FILE_TYPE_FILE = 1,
    SXN_FILE_TYPE_DIRECTORY = 2,
    SXN_FILE_TYPE_DEVICE = 3,
};

struct sxn_file_info {
    uint32_t type;   /* enum sxn_file_type */
    uint32_t mode;   /* permission bits (07777); no write bits on a read-only volume */
    uint32_t uid;
    uint32_t gid;
    uint64_t size;   /* bytes; 0 for a directory or a device */
    uint64_t mtime;  /* seconds since 1970 UTC; 0 when nothing recorded one */
};

#define SXN_FILE_INFO_V1_SIZE 32u

/* The longest name an entry carries, with its NUL. */
#define SXN_NAME_MAX 256u

struct sxn_directory_entry {
    struct sxn_file_info info;
    char name[SXN_NAME_MAX];
};

#define SXN_DIRECTORY_ENTRY_V1_SIZE (SXN_FILE_INFO_V1_SIZE + SXN_NAME_MAX)

/* Dispositions for CREATE_FILE. */
#define SXN_CREATE_NEW 1u     /* fail with -EEXIST if the file exists */
#define SXN_CREATE_ALWAYS 2u  /* empty an existing file */
#define SXN_CREATE_EXECUTABLE 0x100u /* or'd in: a new file is 0700, runnable */
#define SXN_CREATE_READ 0x200u       /* or'd in: the handle can also read */

/* SXN_SYS_LOCK_FILE. */
#define SXN_LOCK_EXCLUSIVE 1u        /* LOCKFILE_EXCLUSIVE_LOCK */
#define SXN_LOCK_FAIL_IMMEDIATELY 2u /* LOCKFILE_FAIL_IMMEDIATELY */

/* The most sectors one STORAGE_READ or STORAGE_WRITE moves (64 KiB). */
#define SXN_STORAGE_MAX_TRANSFER 128u

/* SXN_SYS_WAKE_ADDRESS: release every waiter. */
#define SXN_WAKE_ALL 0xffffffffu

/* Bases of SXN_SYS_SET_THREAD_BASE. */
#define SXN_THREAD_BASE_FS 1u
#define SXN_THREAD_BASE_GS 2u

/* Most handles SXN_SYS_SPAWN_EX places after stdio (at 3, 4, 5 and 6). */
#define SXN_SPAWN_MAX_HANDLES 4u

struct sxn_spawn_request {
    uint64_t path;            /* const char*, an absolute path */
    uint64_t argv;            /* char* const*, argc entries */
    int32_t argc;
    int32_t std_handles[3];   /* the child's 0, 1, 2; -1 = the caller's own */
    int32_t handles[SXN_SPAWN_MAX_HANDLES]; /* the child's 3, 4, ... */
    uint32_t handle_count;    /* entries used in handles[] */
    uint32_t flags;           /* enum sxn_spawn_flags; unknown bits are -EINVAL */
};

enum sxn_spawn_flags {
    /* rsi = const struct sxn_identity*: the child's identity instead of the
     * caller's. Only a caller acting as the system may (-EPERM otherwise);
     * the elevation broker is the one that does. The caller still needs the
     * right to run the image. */
    SXN_SPAWN_FLAG_IDENTITY = 1u << 0,
    /* rdx = const struct sxn_spawn_subsystem*: start the image as a program
     * of another subsystem's ABI, with the environment the block gives. The
     * kernel validates the image for that ABI or fails the spawn
     * (-ENOEXEC); nothing is guessed from the file. For Linux, `path` is a
     * path in the guest tree, under /system/linux (docs/userland/LINUX.md).
     * -EINVAL for an unreadable block or environment, or an unknown
     * subsystem. */
    SXN_SPAWN_FLAG_SUBSYSTEM = 1u << 1,
};

enum sxn_subsystem_id {
    SXN_SUBSYSTEM_LINUX = 2,
};

struct sxn_spawn_subsystem {
    uint32_t subsystem;       /* enum sxn_subsystem_id */
    int32_t envc;             /* entries in envp, at most 128 */
    uint64_t envp;            /* char* const*, "NAME=VALUE" strings */
};

/* Who a process is (docs/kernel/USERS_AND_PERMISSIONS.md#the-token). The
 * layout of savanxp_identity, which POSIX's GET_IDENTITY reports. */
struct sxn_identity {
    uint32_t uid;  /* who the person is */
    uint32_t euid; /* whom it acts as: 0 is the system */
    uint32_t gid;
    uint32_t reserved;
};

/* What a storage device is (sxn_storage_device.kind). */
enum sxn_storage_kind {
    SXN_STORAGE_DISK = 1,      /* a whole physical or virtual disk */
    SXN_STORAGE_PARTITION = 2, /* a slice of a disk; parent_id names it */
    SXN_STORAGE_MEMORY = 3,    /* a volume in RAM: the LiveCD's */
};

enum sxn_storage_flags {
    SXN_STORAGE_WRITABLE = 1u << 0,
    SXN_STORAGE_MOUNTED = 1u << 1,       /* this device is mounted */
    SXN_STORAGE_CHILD_MOUNTED = 1u << 2, /* a partition of this disk is */
    SXN_STORAGE_SYSTEM = 1u << 3,        /* holds "/", itself or a partition */
    SXN_STORAGE_ESP = 1u << 4,           /* an EFI System Partition */
    SXN_STORAGE_TRUNCATED = 1u << 5,     /* device_sectors > sector_count */
    SXN_STORAGE_GPT_BACKUP = 1u << 6,    /* disk: primary GPT damaged, backup used */
    SXN_STORAGE_READ_ONLY = 1u << 7,     /* mounted, and takes no changes */
};

/* SXN_SYS_STORAGE_MOUNT flags. */
enum sxn_storage_mount_flags {
    /* Nothing is written to the device, at mount or later; every change is
     * -EROFS. A volume that needed recovery is shown as recovered. */
    SXN_STORAGE_MOUNT_READ_ONLY = 1u << 0,
};

/* How a disk is partitioned; for a partition, the table it came from. */
enum sxn_storage_scheme {
    SXN_STORAGE_SCHEME_NONE = 0,
    SXN_STORAGE_SCHEME_MBR = 1,
    SXN_STORAGE_SCHEME_GPT = 2,
    SXN_STORAGE_SCHEME_DAMAGED = 3,  /* "EFI PART" with no valid copy */
};

#define SXN_STORAGE_NAME_CAPACITY 24u
#define SXN_STORAGE_MOUNT_CAPACITY 32u

/* SXN_SYS_STORAGE_SNAPSHOT. Grows at the end only. */
struct sxn_storage_device {
    uint32_t id;          /* stable for the device's life; never reused */
    uint32_t generation;  /* bumps whenever any device or mount changes */
    uint32_t kind;        /* enum sxn_storage_kind */
    uint32_t flags;       /* enum sxn_storage_flags */
    uint32_t parent_id;   /* partitions: the disk's id; 0 otherwise */
    uint32_t scheme;      /* enum sxn_storage_scheme */
    uint32_t partition_number; /* 1-based table entry; 0 for a disk */
    uint32_t mbr_type;    /* MBR partitions only */
    uint64_t sector_count;   /* 512-byte sectors, as reachable */
    uint64_t start_lba;      /* partitions: offset in the parent */
    uint64_t device_sectors; /* the size the device reports */
    uint8_t type_guid[16];   /* GPT partitions: as stored on disk */
    uint8_t unique_guid[16]; /* GPT partitions; GPT disks: the disk GUID */
    char name[SXN_STORAGE_NAME_CAPACITY];   /* "ahci1", "ahci1p2" */
    char model[48];
    char serial[24];
    char label[40];          /* the GPT partition name, ASCII */
    char fs_name[16];        /* "sxfs", "fat" or "" when unknown */
    char mount_point[SXN_STORAGE_MOUNT_CAPACITY]; /* "" when not mounted */
    uint64_t total_bytes;    /* mounted volumes only */
    uint64_t used_bytes;
    uint64_t free_bytes;
    uint32_t file_count;
    uint32_t reserved;
};

#define SXN_STORAGE_DEVICE_V1_SIZE ((uint32_t)sizeof(struct sxn_storage_device))

/* SXN_SYS_DEVICE_SNAPSHOT: one hardware instance. docs/kernel/DEVICE_INVENTORY.md. */

/* Where an instance sits (sxn_device_info.bus). */
enum sxn_device_bus {
    SXN_DEVICE_BUS_ROOT = 1,          /* the computer itself; parent_id 0 */
    SXN_DEVICE_BUS_PCI = 2,           /* a PCI function */
    SXN_DEVICE_BUS_PLATFORM = 3,      /* found by a driver, not on a bus: PS/2, the boot framebuffer */
    SXN_DEVICE_BUS_USB = 4,           /* a USB device; its functions are its interfaces */
    SXN_DEVICE_BUS_USB_INTERFACE = 5, /* one interface of a USB device */
};

/* What kind of hardware it is, from its class codes or from the driver
 * that took it. A hint for grouping: an instance with no driver is still
 * classified by what it says it is. */
enum sxn_device_class {
    SXN_DEVICE_CLASS_UNKNOWN = 0,
    SXN_DEVICE_CLASS_COMPUTER = 1,
    SXN_DEVICE_CLASS_SYSTEM = 2,      /* bridges, SMBus, IOMMU, entropy sources */
    SXN_DEVICE_CLASS_DISPLAY = 3,
    SXN_DEVICE_CLASS_SOUND = 4,       /* sound, video and game controllers */
    SXN_DEVICE_CLASS_NETWORK = 5,
    SXN_DEVICE_CLASS_KEYBOARD = 6,
    SXN_DEVICE_CLASS_POINTER = 7,     /* mice, tablets, touch pads */
    SXN_DEVICE_CLASS_INPUT = 8,       /* other human interface devices */
    SXN_DEVICE_CLASS_STORAGE = 9,     /* disk controllers: IDE, AHCI, NVMe, virtio-blk */
    SXN_DEVICE_CLASS_DISK = 10,       /* a disk itself: a USB stick, the LiveCD's RAM disk */
    SXN_DEVICE_CLASS_USB = 11,        /* USB host controllers and hubs */
    SXN_DEVICE_CLASS_USB_DEVICE = 12, /* a USB device as a whole */
};

/* The lifecycle states (docs/kernel/DEVICE_INVENTORY.md#states). The kernel
 * reports UNBOUND, STARTED and FAILED today; the others are named now so the
 * numbers stay when D3 brings them. REMOVED never appears in a snapshot: a
 * removed instance is absent, and its id is never used again. */
enum sxn_device_state {
    SXN_DEVICE_DISCOVERED = 1, /* found, not yet offered to the drivers */
    SXN_DEVICE_UNBOUND = 2,    /* offered; no driver is using it */
    SXN_DEVICE_STARTING = 3,
    SXN_DEVICE_STARTED = 4,    /* a driver took it and it works */
    SXN_DEVICE_QUIESCING = 5,
    SXN_DEVICE_DISABLED = 6,
    SXN_DEVICE_FAILED = 7,     /* a driver took it and could not start it */
    SXN_DEVICE_REMOVED = 8,
};

/* Why it is in that state; NONE when STARTED. */
enum sxn_device_reason {
    SXN_DEVICE_REASON_NONE = 0,
    SXN_DEVICE_REASON_NOT_CLAIMED = 1,  /* no built-in driver took it: none matches,
                                         * or its class chose another device */
    SXN_DEVICE_REASON_START_FAILED = 2, /* the driver gave up while starting it */
    SXN_DEVICE_REASON_BOOT_OPTION = 3,  /* a boot option kept the drivers away (nousb) */
    SXN_DEVICE_REASON_UNSUPPORTED = 4,  /* the driver took it and found nothing it
                                         * supports on it (HD Audio with no analog codec) */
};

enum sxn_device_flags {
    SXN_DEVICE_SYNTHETIC = 1u << 0, /* no hardware behind it: the LiveCD's RAM disk */
    SXN_DEVICE_FIRMWARE = 1u << 1,  /* set up by the firmware and used as it left it */
    SXN_DEVICE_REMOVABLE = 1u << 2, /* can go away while the system runs (USB) */
};

enum sxn_device_resource_kind {
    SXN_DEVICE_RESOURCE_MEMORY = 1,
    SXN_DEVICE_RESOURCE_IO_PORT = 2,
    SXN_DEVICE_RESOURCE_IRQ = 3,    /* start = the line; length 1 */
};

enum sxn_device_resource_flags {
    SXN_DEVICE_RESOURCE_64BIT = 1u << 0,
    SXN_DEVICE_RESOURCE_PREFETCHABLE = 1u << 1,
    SXN_DEVICE_RESOURCE_CLAIMED = 1u << 2, /* the driver claimed it: in use, length exact */
    SXN_DEVICE_RESOURCE_SHARED = 1u << 3,  /* used by other devices too: an INTx line, the 8042's ports */
};

/* An assignment as recorded: a PCI BAR's base as the firmware programmed it,
 * an interrupt line, a legacy port range. length 0 = not recorded (reading a
 * BAR's size writes to the device, which a listing never does); a CLAIMED
 * resource has the length its driver measured when it took it. */
struct sxn_device_resource {
    uint32_t kind;  /* enum sxn_device_resource_kind */
    uint32_t flags; /* enum sxn_device_resource_flags */
    uint64_t start;
    uint64_t length;
};

#define SXN_DEVICE_MAX_RESOURCES 8u
#define SXN_DEVICE_NAME_CAPACITY 48u
#define SXN_DEVICE_MANUFACTURER_CAPACITY 32u
#define SXN_DEVICE_DRIVER_CAPACITY 16u
#define SXN_DEVICE_LOCATION_CAPACITY 32u

/* SXN_SYS_DEVICE_SNAPSHOT. Grows at the end only. Fields that do not apply
 * to the bus are 0 or "". */
struct sxn_device_info {
    uint32_t id;           /* stable for the instance's life; never reused */
    uint32_t parent_id;    /* 0 for the computer */
    uint32_t generation;   /* bumps whenever any instance changes; one per snapshot */
    uint32_t bus;          /* enum sxn_device_bus */
    uint32_t device_class; /* enum sxn_device_class */
    uint32_t state;        /* enum sxn_device_state */
    uint32_t reason;       /* enum sxn_device_reason */
    uint32_t flags;        /* enum sxn_device_flags */
    /* Hardware IDs. PCI: vendor, device, subsystem vendor and subsystem.
     * USB device and interface: idVendor, idProduct, 0, 0. */
    uint16_t vendor_id;
    uint16_t product_id;
    uint16_t subsystem_vendor_id;
    uint16_t subsystem_id;
    /* PCI: class, subclass, programming interface, revision. USB device:
     * bDeviceClass, SubClass, Protocol; USB interface: the interface's. */
    uint8_t class_code;
    uint8_t subclass;
    uint8_t protocol;
    uint8_t revision;
    /* PCI: bus, device, function. USB device: the host controller's index,
     * the depth below its root port (0 on the root port itself) and the
     * port it hangs from. USB interface: bInterfaceNumber as the function. */
    uint8_t bus_number;
    uint8_t device_number;
    uint8_t function_number;
    uint8_t usb_speed;     /* USB: 1 full, 2 low, 3 high, 4 super, 5 super+ */
    uint16_t usb_version;  /* USB device: bcdUSB */
    uint16_t device_version; /* USB device: bcdDevice */
    uint32_t resource_count;
    /* What the hardware reports about itself (a USB product string, an NVMe
     * model) or, for a platform device, what its driver found. "" when
     * nothing said. */
    char name[SXN_DEVICE_NAME_CAPACITY];
    char manufacturer[SXN_DEVICE_MANUFACTURER_CAPACITY]; /* USB: iManufacturer */
    char driver[SXN_DEVICE_DRIVER_CAPACITY];   /* the built-in driver using it; "" when none */
    char location[SXN_DEVICE_LOCATION_CAPACITY]; /* "PCI 00:1f.2", "xhci0 port 1.3" */
    struct sxn_device_resource resources[SXN_DEVICE_MAX_RESOURCES];
};

#define SXN_DEVICE_INFO_V1_SIZE ((uint32_t)sizeof(struct sxn_device_info))

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

struct sxn_port_connection_ex {
    uint32_t client_pid;
    int32_t read_handle;  /* client -> service pipe, read end */
    int32_t write_handle; /* service -> client pipe, write end */
    uint32_t reserved;
    struct sxn_identity client; /* as it was when it connected */
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

/* IPv4 is represented as 0xAABBCCDD and port in host byte order. */
struct sxn_ipv4_endpoint {
    uint32_t ipv4;
    uint16_t port;
    uint16_t reserved;
};
enum sxn_socket_type { SXN_SOCKET_TCP = 1, SXN_SOCKET_UDP = 2 };
struct sxn_datagram {
    uint64_t buffer;
    uint64_t size;
    uint64_t endpoint; /* sxn_ipv4_endpoint*: destination/source; 0 on receive discards it */
};
enum sxn_io_flags {
    SXN_IO_READ = 1u,
    SXN_IO_WRITE = 2u,
    SXN_IO_ERROR = 4u,
};
struct sxn_io_interest {
    int32_t handle;
    uint32_t interests;
};
