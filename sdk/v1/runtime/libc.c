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

long read(int fd, void* buffer, size_t count) {
    return syscall3(SAVANXP_SYS_READ, (unsigned long)fd, (unsigned long)buffer, (unsigned long)count);
}

long write(int fd, const void* buffer, size_t count) {
    return syscall3(SAVANXP_SYS_WRITE, (unsigned long)fd, (unsigned long)buffer, (unsigned long)count);
}

long open(const char* path) {
    return open_mode(path, SAVANXP_OPEN_READ);
}

long open_mode(const char* path, unsigned long flags) {
    return syscall2(SAVANXP_SYS_OPEN, (unsigned long)path, flags);
}

long close(int fd) {
    return syscall1(SAVANXP_SYS_CLOSE, (unsigned long)fd);
}

long readdir(int fd, char* buffer, size_t count) {
    return syscall3(SAVANXP_SYS_READDIR, (unsigned long)fd, (unsigned long)buffer, (unsigned long)count);
}

long spawn(const char* path, const char* const* argv, int argc) {
    return syscall3(SAVANXP_SYS_SPAWN, (unsigned long)path, (unsigned long)argv, (unsigned long)argc);
}

long spawn_fd(const char* path, const char* const* argv, int argc, int stdin_fd, int stdout_fd) {
    return syscall5(
        SAVANXP_SYS_SPAWN_FD,
        (unsigned long)path,
        (unsigned long)argv,
        (unsigned long)argc,
        (unsigned long)stdin_fd,
        (unsigned long)stdout_fd);
}

long spawn_fds(const char* path, const char* const* argv, int argc, int stdin_fd, int stdout_fd, int stderr_fd) {
    long saved = dup(2);
    if (saved < 0) {
        return saved;
    }
    if (dup2(stderr_fd, 2) < 0) {
        close((int)saved);
        return -1;
    }

    long pid = spawn_fd(path, argv, argc, stdin_fd, stdout_fd);
    (void)dup2((int)saved, 2);
    close((int)saved);
    return pid;
}

long exec(const char* path, const char* const* argv, int argc) {
    return syscall3(SAVANXP_SYS_EXEC, (unsigned long)path, (unsigned long)argv, (unsigned long)argc);
}

long pipe(int fds[2]) {
    return syscall1(SAVANXP_SYS_PIPE, (unsigned long)fds);
}

long dup(int fd) {
    return syscall1(SAVANXP_SYS_DUP, (unsigned long)fd);
}

long dup2(int oldfd, int newfd) {
    return syscall2(SAVANXP_SYS_DUP2, (unsigned long)oldfd, (unsigned long)newfd);
}

long seek(int fd, long offset, int whence) {
    return syscall3(SAVANXP_SYS_SEEK, (unsigned long)fd, (unsigned long)offset, (unsigned long)whence);
}

long unlink(const char* path) {
    return syscall1(SAVANXP_SYS_UNLINK, (unsigned long)path);
}

long mkdir(const char* path) {
    return syscall1(SAVANXP_SYS_MKDIR, (unsigned long)path);
}

long rmdir(const char* path) {
    return syscall1(SAVANXP_SYS_RMDIR, (unsigned long)path);
}

long truncate(const char* path, unsigned long size) {
    return syscall2(SAVANXP_SYS_TRUNCATE, (unsigned long)path, size);
}

long rename(const char* old_path, const char* new_path) {
    return syscall2(SAVANXP_SYS_RENAME, (unsigned long)old_path, (unsigned long)new_path);
}

long ioctl(int fd, unsigned long request, unsigned long arg) {
    return syscall3(SAVANXP_SYS_IOCTL, (unsigned long)fd, request, arg);
}

long socket(unsigned long domain, unsigned long type, unsigned long protocol) {
    return syscall3(SAVANXP_SYS_SOCKET, domain, type, protocol);
}

long bind(int fd, const struct savanxp_sockaddr_in* address) {
    return syscall2(SAVANXP_SYS_BIND, (unsigned long)fd, (unsigned long)address);
}

long sendto(int fd, const void* buffer, size_t count, const struct savanxp_sockaddr_in* address) {
    return syscall5(SAVANXP_SYS_SENDTO, (unsigned long)fd, (unsigned long)buffer, (unsigned long)count, (unsigned long)address, 0);
}

long recvfrom(int fd, void* buffer, size_t count, struct savanxp_sockaddr_in* address, unsigned long timeout_ms) {
    return syscall5(SAVANXP_SYS_RECVFROM, (unsigned long)fd, (unsigned long)buffer, (unsigned long)count, (unsigned long)address, timeout_ms);
}

long connect(int fd, const struct savanxp_sockaddr_in* address, unsigned long timeout_ms) {
    return syscall3(SAVANXP_SYS_CONNECT, (unsigned long)fd, (unsigned long)address, timeout_ms);
}

long waitpid(int pid, int* status) {
    return syscall2(SAVANXP_SYS_WAITPID, (unsigned long)pid, (unsigned long)status);
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

long sync(void) {
    return syscall0(SAVANXP_SYS_SYNC);
}

long mouse_open(void) {
    return open_mode("/dev/mouse0", SAVANXP_OPEN_READ);
}

int mouse_poll_event(int fd, struct savanxp_mouse_event* event) {
    const long result = read(fd, event, sizeof(*event));
    if (result < 0) {
        return (int)result;
    }
    if (result != (long)sizeof(*event)) {
        return 0;
    }
    return 1;
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

void* memcpy(void* destination, const void* source, size_t count) {
    unsigned char* dst = (unsigned char*)destination;
    const unsigned char* src = (const unsigned char*)source;
    for (size_t index = 0; index < count; ++index) {
        dst[index] = src[index];
    }
    return destination;
}

void* memset(void* destination, int value, size_t count) {
    unsigned char* dst = (unsigned char*)destination;
    for (size_t index = 0; index < count; ++index) {
        dst[index] = (unsigned char)value;
    }
    return destination;
}

void putchar(int fd, char character) {
    write(fd, &character, 1);
}

void puts_fd(int fd, const char* text) {
    write(fd, text, strlen(text));
}

void puts_err(const char* text) {
    puts_fd(SAVANXP_STDERR_FILENO, text);
}

void puts(const char* text) {
    puts_fd(SAVANXP_STDOUT_FILENO, text);
}

static void write_unsigned_fd(int fd, unsigned long value, unsigned long base) {
    char buffer[32];
    size_t index = 0;

    if (value == 0) {
        putchar(fd, '0');
        return;
    }

    while (value != 0) {
        unsigned long digit = value % base;
        buffer[index++] = (char)(digit < 10 ? ('0' + digit) : ('a' + (digit - 10)));
        value /= base;
    }

    while (index > 0) {
        putchar(fd, buffer[--index]);
    }
}

static void write_signed_fd(int fd, long value) {
    if (value < 0) {
        putchar(fd, '-');
        write_unsigned_fd(fd, (unsigned long)(-value), 10);
        return;
    }
    write_unsigned_fd(fd, (unsigned long)value, 10);
}

static void vprintf_fd(int fd, const char* format, va_list args) {
    for (const char* cursor = format; *cursor != '\0'; ++cursor) {
        if (*cursor != '%') {
            putchar(fd, *cursor);
            continue;
        }

        ++cursor;
        switch (*cursor) {
            case '%':
                putchar(fd, '%');
                break;
            case 's':
                puts_fd(fd, va_arg(args, const char*));
                break;
            case 'd':
            case 'i':
                write_signed_fd(fd, va_arg(args, int));
                break;
            case 'u':
                write_unsigned_fd(fd, va_arg(args, unsigned int), 10);
                break;
            case 'x':
                write_unsigned_fd(fd, va_arg(args, unsigned int), 16);
                break;
            default:
                puts_fd(fd, "%?");
                break;
        }
    }
}

void printf_fd(int fd, const char* format, ...) {
    va_list args;
    va_start(args, format);
    vprintf_fd(fd, format, args);
    va_end(args);
}

void eprintf(const char* format, ...) {
    va_list args;
    va_start(args, format);
    vprintf_fd(SAVANXP_STDERR_FILENO, format, args);
    va_end(args);
}

void printf(const char* format, ...) {
    va_list args;
    va_start(args, format);
    vprintf_fd(SAVANXP_STDOUT_FILENO, format, args);
    va_end(args);
}
