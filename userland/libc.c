#include "libc.h"

#include <stdarg.h>

#include "shared/syscall.h"

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

void exit(int code) {
    syscall1(SAVANXP_SYS_EXIT, (unsigned long)code);
    for (;;) {
        asm volatile("hlt");
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

void puts(const char* text) {
    puts_fd(1, text);
}

static void write_unsigned(unsigned long value, unsigned long base) {
    char buffer[32];
    size_t index = 0;

    if (value == 0) {
        putchar(1, '0');
        return;
    }

    while (value != 0) {
        unsigned long digit = value % base;
        buffer[index++] = (char)(digit < 10 ? ('0' + digit) : ('a' + (digit - 10)));
        value /= base;
    }

    while (index > 0) {
        putchar(1, buffer[--index]);
    }
}

static void write_signed(long value) {
    if (value < 0) {
        putchar(1, '-');
        write_unsigned((unsigned long)(-value), 10);
        return;
    }
    write_unsigned((unsigned long)value, 10);
}

void printf(const char* format, ...) {
    va_list args;
    va_start(args, format);

    for (const char* cursor = format; *cursor != '\0'; ++cursor) {
        if (*cursor != '%') {
            putchar(1, *cursor);
            continue;
        }

        ++cursor;
        switch (*cursor) {
            case '%':
                putchar(1, '%');
                break;
            case 's':
                puts_fd(1, va_arg(args, const char*));
                break;
            case 'd':
            case 'i':
                write_signed(va_arg(args, int));
                break;
            case 'u':
                write_unsigned(va_arg(args, unsigned int), 10);
                break;
            case 'x':
                write_unsigned(va_arg(args, unsigned int), 16);
                break;
            default:
                puts_fd(1, "%?");
                break;
        }
    }

    va_end(args);
}
