#include "savanxp/libc.h"

#include <stdarg.h>

static long syscall3(unsigned long number, unsigned long a, unsigned long b, unsigned long c) {
    long result;
    asm volatile("int $0x80" : "=a"(result) : "a"(number), "D"(a), "S"(b), "d"(c) : "memory");
    return result;
}

static long syscall5(unsigned long number, unsigned long a, unsigned long b, unsigned long c, unsigned long d, unsigned long e) {
    register unsigned long r10 asm("r10") = d;
    register unsigned long r8 asm("r8") = e;
    long result;
    asm volatile(
        "int $0x80"
        : "=a"(result)
        : "a"(number), "D"(a), "S"(b), "d"(c), "r"(r10), "r"(r8)
        : "memory");
    return result;
}

static long syscall2(unsigned long number, unsigned long a, unsigned long b) {
    return syscall3(number, a, b, 0);
}

static long syscall1(unsigned long number, unsigned long a) {
    return syscall3(number, a, 0, 0);
}

static long syscall0(unsigned long number) {
    return syscall3(number, 0, 0, 0);
}

long savanxp_read(int fd, void* buffer, size_t count) {
    return syscall3(SAVANXP_SYS_READ, (unsigned long)fd, (unsigned long)buffer, (unsigned long)count);
}

long savanxp_write(int fd, const void* buffer, size_t count) {
    return syscall3(SAVANXP_SYS_WRITE, (unsigned long)fd, (unsigned long)buffer, (unsigned long)count);
}

long savanxp_open(const char* path) {
    return savanxp_open_mode(path, SAVANXP_OPEN_READ);
}

long savanxp_open_mode(const char* path, unsigned long flags) {
    return syscall2(SAVANXP_SYS_OPEN, (unsigned long)path, flags);
}

long savanxp_close(int fd) {
    return syscall1(SAVANXP_SYS_CLOSE, (unsigned long)fd);
}

long savanxp_readdir(int fd, char* buffer, size_t count) {
    return syscall3(SAVANXP_SYS_READDIR, (unsigned long)fd, (unsigned long)buffer, (unsigned long)count);
}

long spawn(const char* path, const char* const* argv, int argc) {
    return syscall3(SAVANXP_SYS_SPAWN, (unsigned long)path, (unsigned long)argv, (unsigned long)argc);
}

long spawn_fd(const char* path, const char* const* argv, int argc, int stdin_fd, int stdout_fd) {
    long saved_stdin = savanxp_dup(0);
    long saved_stdout = savanxp_dup(1);
    long pid = -1;

    if (saved_stdin < 0 || saved_stdout < 0) {
        if (saved_stdin >= 0) {
            savanxp_close((int)saved_stdin);
        }
        if (saved_stdout >= 0) {
            savanxp_close((int)saved_stdout);
        }
        return -1;
    }

    if (savanxp_dup2(stdin_fd, 0) < 0 || savanxp_dup2(stdout_fd, 1) < 0) {
        (void)savanxp_dup2((int)saved_stdin, 0);
        (void)savanxp_dup2((int)saved_stdout, 1);
        savanxp_close((int)saved_stdin);
        savanxp_close((int)saved_stdout);
        return -1;
    }

    pid = spawn(path, argv, argc);
    (void)savanxp_dup2((int)saved_stdin, 0);
    (void)savanxp_dup2((int)saved_stdout, 1);
    savanxp_close((int)saved_stdin);
    savanxp_close((int)saved_stdout);
    return pid;
}

long spawn_fds(const char* path, const char* const* argv, int argc, int stdin_fd, int stdout_fd, int stderr_fd) {
    long saved_stderr = savanxp_dup(2);
    if (saved_stderr < 0) {
        return saved_stderr;
    }
    if (savanxp_dup2(stderr_fd, 2) < 0) {
        savanxp_close((int)saved_stderr);
        return -1;
    }

    long pid = spawn_fd(path, argv, argc, stdin_fd, stdout_fd);
    (void)savanxp_dup2((int)saved_stderr, 2);
    savanxp_close((int)saved_stderr);
    return pid;
}

long exec(const char* path, const char* const* argv, int argc) {
    return syscall3(SAVANXP_SYS_EXEC, (unsigned long)path, (unsigned long)argv, (unsigned long)argc);
}

long savanxp_pipe(int fds[2]) {
    return syscall1(SAVANXP_SYS_PIPE, (unsigned long)fds);
}

long savanxp_dup(int fd) {
    return syscall1(SAVANXP_SYS_DUP, (unsigned long)fd);
}

long savanxp_dup2(int oldfd, int newfd) {
    return syscall2(SAVANXP_SYS_DUP2, (unsigned long)oldfd, (unsigned long)newfd);
}

long seek(int fd, long offset, int whence) {
    return syscall3(SAVANXP_SYS_SEEK, (unsigned long)fd, (unsigned long)offset, (unsigned long)whence);
}

long savanxp_unlink(const char* path) {
    return syscall1(SAVANXP_SYS_UNLINK, (unsigned long)path);
}

long savanxp_mkdir(const char* path) {
    return syscall1(SAVANXP_SYS_MKDIR, (unsigned long)path);
}

long savanxp_rmdir(const char* path) {
    return syscall1(SAVANXP_SYS_RMDIR, (unsigned long)path);
}

long savanxp_truncate(const char* path, unsigned long size) {
    return syscall2(SAVANXP_SYS_TRUNCATE, (unsigned long)path, size);
}

long savanxp_rename(const char* old_path, const char* new_path) {
    return syscall2(SAVANXP_SYS_RENAME, (unsigned long)old_path, (unsigned long)new_path);
}

long savanxp_fcntl(int fd, unsigned long command, unsigned long value) {
    return syscall3(SAVANXP_SYS_FCNTL, (unsigned long)fd, command, value);
}

long savanxp_ioctl(int fd, unsigned long request, unsigned long arg) {
    return syscall3(SAVANXP_SYS_IOCTL, (unsigned long)fd, request, arg);
}

long savanxp_poll(struct savanxp_pollfd* fds, unsigned long count, long timeout_ms) {
    return syscall3(SAVANXP_SYS_POLL, (unsigned long)fds, count, (unsigned long)timeout_ms);
}

long savanxp_socket(unsigned long domain, unsigned long type, unsigned long protocol) {
    return syscall3(SAVANXP_SYS_SOCKET, domain, type, protocol);
}

long savanxp_bind(int fd, const struct savanxp_sockaddr_in* address) {
    return syscall2(SAVANXP_SYS_BIND, (unsigned long)fd, (unsigned long)address);
}

long savanxp_sendto(int fd, const void* buffer, size_t count, const struct savanxp_sockaddr_in* address) {
    return syscall5(SAVANXP_SYS_SENDTO, (unsigned long)fd, (unsigned long)buffer, (unsigned long)count, (unsigned long)address, 0);
}

long savanxp_recvfrom(int fd, void* buffer, size_t count, struct savanxp_sockaddr_in* address, unsigned long timeout_ms) {
    return syscall5(SAVANXP_SYS_RECVFROM, (unsigned long)fd, (unsigned long)buffer, (unsigned long)count, (unsigned long)address, timeout_ms);
}

long savanxp_connect(int fd, const struct savanxp_sockaddr_in* address, unsigned long timeout_ms) {
    return syscall3(SAVANXP_SYS_CONNECT, (unsigned long)fd, (unsigned long)address, timeout_ms);
}

long savanxp_waitpid(int pid, int* status) {
    return syscall2(SAVANXP_SYS_WAITPID, (unsigned long)pid, (unsigned long)status);
}

long savanxp_fork(void) {
    return syscall0(SAVANXP_SYS_FORK);
}

long savanxp_kill(int pid, int signal_number) {
    return syscall2(SAVANXP_SYS_KILL, (unsigned long)pid, (unsigned long)signal_number);
}

long event_create(unsigned long flags) {
    return syscall1(SAVANXP_SYS_EVENT_CREATE, flags);
}

long event_set(int handle) {
    return syscall1(SAVANXP_SYS_EVENT_SET, (unsigned long)handle);
}

long event_reset(int handle) {
    return syscall1(SAVANXP_SYS_EVENT_RESET, (unsigned long)handle);
}

long wait_one(int handle, long timeout_ms) {
    return syscall2(SAVANXP_SYS_WAIT_ONE, (unsigned long)handle, (unsigned long)timeout_ms);
}

long wait_many(const int* handles, unsigned long count, unsigned long flags, long timeout_ms) {
    return syscall5(
        SAVANXP_SYS_WAIT_MANY,
        (unsigned long)handles,
        count,
        flags,
        (unsigned long)timeout_ms,
        0);
}

long timer_create(unsigned long flags) {
    return syscall1(SAVANXP_SYS_TIMER_CREATE, flags);
}

long timer_set(int handle, unsigned long due_ms, unsigned long period_ms) {
    return syscall3(SAVANXP_SYS_TIMER_SET, (unsigned long)handle, due_ms, period_ms);
}

long timer_cancel(int handle) {
    return syscall1(SAVANXP_SYS_TIMER_CANCEL, (unsigned long)handle);
}

long section_create(unsigned long size, unsigned long flags) {
    return syscall2(SAVANXP_SYS_SECTION_CREATE, size, flags);
}

void* map_view(int handle, unsigned long flags) {
    return (void*)syscall2(SAVANXP_SYS_MAP_VIEW, (unsigned long)handle, flags);
}

long unmap_view(void* base) {
    return syscall1(SAVANXP_SYS_UNMAP_VIEW, (unsigned long)base);
}

long semaphore_create(long initial_count, long max_count) {
    return syscall2(SAVANXP_SYS_SEMAPHORE_CREATE, (unsigned long)initial_count, (unsigned long)max_count);
}

long semaphore_release(int handle, unsigned long release_count) {
    return syscall2(SAVANXP_SYS_SEMAPHORE_RELEASE, (unsigned long)handle, release_count);
}

long yield(void) {
    return syscall0(SAVANXP_SYS_YIELD);
}

long sleep_ms(unsigned long milliseconds) {
    return syscall1(SAVANXP_SYS_SLEEP_MS, milliseconds);
}

unsigned long uptime_ms(void) {
    return (unsigned long)syscall0(SAVANXP_SYS_UPTIME_MS);
}

long clear_screen(void) {
    return syscall0(SAVANXP_SYS_CLEAR);
}

long proc_info(unsigned long index, struct savanxp_process_info* info) {
    return syscall2(SAVANXP_SYS_PROC_INFO, index, (unsigned long)info);
}

long system_info(struct savanxp_system_info* info) {
    return syscall1(SAVANXP_SYS_SYSTEM_INFO, (unsigned long)info);
}

long realtime(struct savanxp_realtime* value) {
    return syscall1(SAVANXP_SYS_REALTIME, (unsigned long)value);
}

long savanxp_sync(void) {
    return syscall0(SAVANXP_SYS_SYNC);
}

long mouse_open(void) {
    long duplicated = savanxp_dup(5);
    if (duplicated >= 0) {
        return duplicated;
    }
    return savanxp_open_mode("/dev/mouse0", SAVANXP_OPEN_READ);
}

int mouse_poll_event(int fd, struct savanxp_mouse_event* event) {
    struct savanxp_pollfd pollfd = {
        .fd = fd,
        .events = SAVANXP_POLLIN | SAVANXP_POLLHUP,
        .revents = 0,
    };
    if (fd < 0 || event == 0) {
        return -SAVANXP_EINVAL;
    }
    if (savanxp_poll(&pollfd, 1, 0) <= 0 || (pollfd.revents & SAVANXP_POLLIN) == 0) {
        return 0;
    }
    {
        const long result = savanxp_read(fd, event, sizeof(*event));
        if (result < 0) {
            return (int)result;
        }
        if (result != (long)sizeof(*event)) {
            return 0;
        }
    }
    return 1;
}

/* Canal de puntero ruteado por el WM (SAVANXP_WM_FD_MOUSE). Devuelve -1 en
 * procesos que no se lanzaron como cliente del WM y por lo tanto no lo tienen. */
long gfx_pointer_open(void) {
    return savanxp_dup(SAVANXP_WM_FD_MOUSE);
}

int gfx_poll_pointer(int fd, struct savanxp_gui_pointer_event* event) {
    struct savanxp_pollfd pollfd = {
        .fd = fd,
        .events = SAVANXP_POLLIN | SAVANXP_POLLHUP,
        .revents = 0,
    };
    if (fd < 0 || event == 0) {
        return -SAVANXP_EINVAL;
    }
    if (savanxp_poll(&pollfd, 1, 0) <= 0 || (pollfd.revents & SAVANXP_POLLIN) == 0) {
        return 0;
    }
    {
        const long result = savanxp_read(fd, event, sizeof(*event));
        if (result < 0) {
            return (int)result;
        }
        if (result != (long)sizeof(*event)) {
            return 0;
        }
    }
    return 1;
}

long audio_open(void) {
    return savanxp_open_mode("/dev/audio0", SAVANXP_OPEN_WRITE);
}

long audio_get_info(int fd, struct savanxp_audio_info* info) {
    return savanxp_ioctl(fd, AUDIO_IOC_GET_INFO, (unsigned long)info);
}

int power_shutdown(void) {
    long fd = savanxp_open_mode("/dev/power", SAVANXP_OPEN_WRITE);
    if (fd < 0) {
        return (int)fd;
    }
    long result = savanxp_ioctl((int)fd, POWER_IOC_SHUTDOWN, 0);
    savanxp_close((int)fd);
    return (int)result;
}

int power_reboot(void) {
    long fd = savanxp_open_mode("/dev/power", SAVANXP_OPEN_WRITE);
    if (fd < 0) {
        return (int)fd;
    }
    long result = savanxp_ioctl((int)fd, POWER_IOC_REBOOT, 0);
    savanxp_close((int)fd);
    return (int)result;
}

int clipboard_clear(void);

static long clipboard_open(unsigned int mode) {
    return savanxp_open_mode("/dev/clipboard", mode);
}

int clipboard_set_text(const char* text) {
    if (text == 0) {
        return -(int)SAVANXP_EINVAL;
    }
    int length = 0;
    while (text[length] != '\0') {
        length += 1;
    }
    /* Copiar la cadena vacia significa vaciar el portapapeles, pero no se puede
     * expresar con un write: write_handle corta en count == 0 antes de llegar
     * al device (kernel/process.cpp), asi que el pedido no llegaria nunca. El
     * ioctl dice lo mismo y ademas lo dice explicito. */
    if (length == 0) {
        return clipboard_clear();
    }

    long fd = clipboard_open(SAVANXP_OPEN_WRITE);
    if (fd < 0) {
        return (int)fd;
    }
    /* Un write reemplaza el contenido entero, asi que no hace falta limpiar
     * antes: es un valor, no un append. */
    long written = savanxp_write((int)fd, text, (unsigned long)length);
    savanxp_close((int)fd);
    if (written < 0) {
        return (int)written;
    }
    return written == length ? 0 : -(int)SAVANXP_EIO;
}

int clipboard_get_info(struct savanxp_clipboard_info* info) {
    if (info == 0) {
        return -(int)SAVANXP_EINVAL;
    }
    long fd = clipboard_open(SAVANXP_OPEN_READ);
    if (fd < 0) {
        return (int)fd;
    }
    long result = savanxp_ioctl((int)fd, CLIP_IOC_GET_INFO, (unsigned long)info);
    savanxp_close((int)fd);
    return (int)result;
}

int clipboard_get_text(char* buffer, int capacity) {
    if (buffer == 0 || capacity <= 0) {
        return -(int)SAVANXP_EINVAL;
    }
    buffer[0] = '\0';

    long fd = clipboard_open(SAVANXP_OPEN_READ);
    if (fd < 0) {
        return (int)fd;
    }

    /* El largo real se pide antes de leer para poder informar truncamiento: el
     * read solo devuelve lo que entro en el buffer y no distingue "eso era
     * todo" de "habia mas". */
    struct savanxp_clipboard_info info;
    long result = savanxp_ioctl((int)fd, CLIP_IOC_GET_INFO, (unsigned long)&info);
    if (result < 0) {
        savanxp_close((int)fd);
        return (int)result;
    }

    long copied = savanxp_read((int)fd, buffer, (unsigned long)(capacity - 1));
    savanxp_close((int)fd);
    if (copied < 0) {
        return (int)copied;
    }
    buffer[copied] = '\0';
    return (int)info.length;
}

int clipboard_clear(void) {
    long fd = clipboard_open(SAVANXP_OPEN_WRITE);
    if (fd < 0) {
        return (int)fd;
    }
    long result = savanxp_ioctl((int)fd, CLIP_IOC_CLEAR, 0);
    savanxp_close((int)fd);
    return (int)result;
}

long gpu_open(void) {
    return savanxp_open_mode("/dev/gpu0", SAVANXP_OPEN_READ | SAVANXP_OPEN_WRITE);
}

long gpu_get_info(int fd, struct savanxp_gpu_info* info) {
    return savanxp_ioctl(fd, GPU_IOC_GET_INFO, (unsigned long)info);
}

long gpu_acquire(int fd) {
    return savanxp_ioctl(fd, GPU_IOC_ACQUIRE, 0);
}

long gpu_release(int fd) {
    return savanxp_ioctl(fd, GPU_IOC_RELEASE, 0);
}

long gpu_set_mode(int fd, struct savanxp_gpu_mode* mode) {
    return savanxp_ioctl(fd, GPU_IOC_SET_MODE, (unsigned long)mode);
}

long input_set_layout(int fd, int layout) {
    return savanxp_ioctl(fd, INPUT_IOC_SET_LAYOUT, (unsigned long)layout);
}

long input_get_layout(int fd) {
    return savanxp_ioctl(fd, INPUT_IOC_GET_LAYOUT, 0);
}

long gpu_import_section(int fd, struct savanxp_gpu_surface_import* import_request) {
    return savanxp_ioctl(fd, GPU_IOC_IMPORT_SECTION, (unsigned long)import_request);
}

long gpu_release_surface(int fd, uint32_t surface_id) {
    return savanxp_ioctl(fd, GPU_IOC_RELEASE_SURFACE, (unsigned long)surface_id);
}

long gpu_present_surface_region(int fd, const struct savanxp_gpu_surface_present* present_request) {
    return savanxp_ioctl(fd, GPU_IOC_PRESENT_SURFACE_REGION, (unsigned long)present_request);
}

long gpu_wait_idle(int fd) {
    return savanxp_ioctl(fd, GPU_IOC_WAIT_IDLE, 0);
}

long gpu_get_stats(int fd, struct savanxp_gpu_stats* stats) {
    return savanxp_ioctl(fd, GPU_IOC_GET_STATS, (unsigned long)stats);
}

long gpu_get_scanouts(int fd, struct savanxp_gpu_scanout_state* state) {
    return savanxp_ioctl(fd, GPU_IOC_GET_SCANOUTS, (unsigned long)state);
}

long gpu_refresh_scanouts(int fd) {
    return savanxp_ioctl(fd, GPU_IOC_REFRESH_SCANOUTS, 0);
}

long gpu_get_connector_properties(int fd, struct savanxp_gpu_connector_properties* properties) {
    return savanxp_ioctl(fd, GPU_IOC_GET_CONNECTOR_PROPERTIES, (unsigned long)properties);
}

long gpu_create_present_event(int fd) {
    return savanxp_ioctl(fd, GPU_IOC_CREATE_PRESENT_EVENT, 0);
}

long gpu_create_scanout_event(int fd) {
    return savanxp_ioctl(fd, GPU_IOC_CREATE_SCANOUT_EVENT, 0);
}

long gpu_set_cursor(int fd, const struct savanxp_gpu_cursor_image* image) {
    return savanxp_ioctl(fd, GPU_IOC_SET_CURSOR, (unsigned long)image);
}

long gpu_move_cursor(int fd, const struct savanxp_gpu_cursor_position* position) {
    return savanxp_ioctl(fd, GPU_IOC_MOVE_CURSOR, (unsigned long)position);
}

long gpu_get_present_timeline(int fd, struct savanxp_gpu_present_timeline* timeline) {
    return savanxp_ioctl(fd, GPU_IOC_GET_PRESENT_TIMELINE, (unsigned long)timeline);
}

long gpu_wait_present(int fd, struct savanxp_gpu_present_wait* wait_request) {
    return savanxp_ioctl(fd, GPU_IOC_WAIT_PRESENT, (unsigned long)wait_request);
}

long gpu_present_surface_batch(int fd, const struct savanxp_gpu_surface_present_batch* batch_request) {
    return savanxp_ioctl(fd, GPU_IOC_PRESENT_SURFACE_BATCH, (unsigned long)batch_request);
}

long gpu_present(int fd, const uint32_t* pixels) {
    return savanxp_ioctl(fd, GPU_IOC_PRESENT, (unsigned long)pixels);
}

long gpu_present_region(int fd, const uint32_t* pixels, uint32_t source_pitch, uint32_t x, uint32_t y, uint32_t width, uint32_t height) {
    struct savanxp_gpu_present_region region = {
        .pixels = (uint64_t)(unsigned long)pixels,
        .source_pitch = source_pitch,
        .x = x,
        .y = y,
        .width = width,
        .height = height,
    };
    return savanxp_ioctl(fd, GPU_IOC_PRESENT_REGION, (unsigned long)&region);
}

long savanxp_getpid(void) {
    return syscall0(SAVANXP_SYS_GETPID);
}

long savanxp_stat(const char* path, struct savanxp_stat* info) {
    return syscall2(SAVANXP_SYS_STAT, (unsigned long)path, (unsigned long)info);
}

long savanxp_fstat(int fd, struct savanxp_stat* info) {
    return syscall2(SAVANXP_SYS_FSTAT, (unsigned long)fd, (unsigned long)info);
}

long savanxp_chdir(const char* path) {
    return syscall1(SAVANXP_SYS_CHDIR, (unsigned long)path);
}

long savanxp_getcwd(char* buffer, size_t count) {
    return syscall2(SAVANXP_SYS_GETCWD, (unsigned long)buffer, (unsigned long)count);
}

void exit(int code) {
    syscall1(SAVANXP_SYS_EXIT, (unsigned long)code);
    for (;;) {
        asm volatile("hlt");
    }
}

int result_is_error(long result) {
    return result < 0;
}

int result_error_code(long result) {
    return result < 0 ? (int)(-result) : 0;
}

const char* error_string(int error_code) {
    switch (error_code) {
        case 0:
            return "ok";
        case SAVANXP_EIO:
            return "io error";
        case SAVANXP_ENOEXEC:
            return "not an executable image";
        case SAVANXP_EACCES:
            return "permission denied";
        case SAVANXP_EAGAIN:
            return "try again";
        case SAVANXP_EINVAL:
            return "invalid argument";
        case SAVANXP_EBADF:
            return "bad file descriptor";
        case SAVANXP_ENOENT:
            return "no such file or directory";
        case SAVANXP_ENOMEM:
            return "out of memory";
        case SAVANXP_EBUSY:
            return "resource busy";
        case SAVANXP_EEXIST:
            return "already exists";
        case SAVANXP_ENODEV:
            return "no such device";
        case SAVANXP_ENOTDIR:
            return "not a directory";
        case SAVANXP_EISDIR:
            return "is a directory";
        case SAVANXP_ENOSPC:
            return "no space left";
        case SAVANXP_ENOTTY:
            return "not a tty";
        case SAVANXP_EPIPE:
            return "broken pipe";
        case SAVANXP_ENOSYS:
            return "not implemented";
        case SAVANXP_ENOTEMPTY:
            return "directory not empty";
        case SAVANXP_E2BIG:
            return "argument list too long";
        case SAVANXP_ECHILD:
            return "no child process";
        case SAVANXP_ETIMEDOUT:
            return "timed out";
        default:
            return "unknown error";
    }
}

const char* result_error_string(long result) {
    return error_string(result_error_code(result));
}

const char* process_state_string(unsigned long state) {
    switch (state) {
        case SAVANXP_PROC_UNUSED:
            return "unused";
        case SAVANXP_PROC_READY:
            return "ready";
        case SAVANXP_PROC_RUNNING:
            return "running";
        case SAVANXP_PROC_BLOCKED_READ:
            return "blocked_read";
        case SAVANXP_PROC_BLOCKED_WRITE:
            return "blocked_write";
        case SAVANXP_PROC_BLOCKED_WAIT:
            return "blocked_wait";
        case SAVANXP_PROC_SLEEPING:
            return "sleeping";
        case SAVANXP_PROC_ZOMBIE:
            return "zombie";
        default:
            return "unknown";
    }
}

const char* net_status_string(unsigned long status) {
    switch (status) {
        case SAVANXP_NET_STATUS_UNKNOWN:
            return "unknown";
        case SAVANXP_NET_STATUS_IDLE:
            return "idle";
        case SAVANXP_NET_STATUS_READY:
            return "ready";
        case SAVANXP_NET_STATUS_NO_DEVICE:
            return "no device";
        case SAVANXP_NET_STATUS_BRING_UP_FAILED:
            return "bring-up failed";
        case SAVANXP_NET_STATUS_TX_FAILED:
            return "tx failed";
        case SAVANXP_NET_STATUS_TX_TIMEOUT:
            return "tx timeout";
        case SAVANXP_NET_STATUS_RX_INVALID:
            return "rx invalid";
        case SAVANXP_NET_STATUS_ARP_RESOLVING:
            return "arp resolving";
        case SAVANXP_NET_STATUS_ARP_RESOLVED:
            return "arp resolved";
        case SAVANXP_NET_STATUS_ARP_TIMEOUT:
            return "arp timeout";
        case SAVANXP_NET_STATUS_ICMP_SENT:
            return "icmp sent";
        case SAVANXP_NET_STATUS_ICMP_REPLY:
            return "icmp reply";
        case SAVANXP_NET_STATUS_ICMP_TIMEOUT:
            return "icmp timeout";
        case SAVANXP_NET_STATUS_TCP_SYN_SENT:
            return "tcp syn sent";
        case SAVANXP_NET_STATUS_TCP_ESTABLISHED:
            return "tcp established";
        case SAVANXP_NET_STATUS_TCP_FIN:
            return "tcp fin";
        case SAVANXP_NET_STATUS_TCP_TIMEOUT:
            return "tcp timeout";
        default:
            return "unknown";
    }
}

size_t strlen(const char* text) {
    size_t length = 0;
    while (text[length] != '\0') {
        ++length;
    }
    return length;
}

int strcmp(const char* left, const char* right) {
    size_t index = 0;
    while (left[index] != '\0' || right[index] != '\0') {
        if (left[index] != right[index]) {
            return left[index] < right[index] ? -1 : 1;
        }
        ++index;
    }
    return 0;
}

int strncmp(const char* left, const char* right, size_t count) {
    for (size_t index = 0; index < count; ++index) {
        if (left[index] != right[index]) {
            return left[index] < right[index] ? -1 : 1;
        }
        if (left[index] == '\0') {
            return 0;
        }
    }
    return 0;
}

char* strcpy(char* destination, const char* source) {
    size_t index = 0;
    while (source[index] != '\0') {
        destination[index] = source[index];
        ++index;
    }
    destination[index] = '\0';
    return destination;
}

// Userland tambien se compila con -mgeneral-regs-only (sin SSE), asi que la
// unidad mas ancha son 8 bytes. Las instrucciones de string mueven esos 8 bytes
// por iteracion sin overhead de lazo; un lazo en C sin optimizar cuesta media
// docena de instrucciones por byte. Importa mas de lo que parece: el blit por
// filas del compositor (sx_painter_blit en gfx2d.c) llama aca una vez por fila
// de cada rectangulo sucio de cada frame.
//
// Requisito: DF=0, que garantiza el crt0 con su cld al arrancar el proceso.
void* memcpy(void* destination, const void* source, size_t count) {
    void* cursor = destination;
    const void* origin = source;
    size_t words = count >> 3;
    size_t tail = count & 7u;

    __asm__ volatile("rep movsq" : "+D"(cursor), "+S"(origin), "+c"(words) : : "memory");
    __asm__ volatile("rep movsb" : "+D"(cursor), "+S"(origin), "+c"(tail) : : "memory");

    return destination;
}

void* memset(void* destination, int value, size_t count) {
    unsigned char byte = (unsigned char)value;
    /* Replica el byte en las 8 posiciones para que stosq escriba el mismo patron. */
    unsigned long long pattern = (unsigned long long)byte * 0x0101010101010101ULL;
    void* cursor = destination;
    size_t words = count >> 3;
    size_t tail = count & 7u;

    __asm__ volatile("rep stosq" : "+D"(cursor), "+c"(words) : "a"(pattern) : "memory");
    __asm__ volatile("rep stosb" : "+D"(cursor), "+c"(tail) : "a"(byte) : "memory");

    return destination;
}
