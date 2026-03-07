#pragma once

#include <stddef.h>
#include <stdint.h>

#include "shared/syscall.h"

long read(int fd, void* buffer, size_t count);
long write(int fd, const void* buffer, size_t count);
long open(const char* path);
long open_mode(const char* path, unsigned long flags);
long close(int fd);
long readdir(int fd, char* buffer, size_t count);
long spawn(const char* path, const char* const* argv, int argc);
long spawn_fd(const char* path, const char* const* argv, int argc, int stdin_fd, int stdout_fd);
long spawn_fds(const char* path, const char* const* argv, int argc, int stdin_fd, int stdout_fd, int stderr_fd);
long exec(const char* path, const char* const* argv, int argc);
long pipe(int fds[2]);
long dup(int fd);
long dup2(int oldfd, int newfd);
long seek(int fd, long offset, int whence);
long unlink(const char* path);
long mkdir(const char* path);
long rmdir(const char* path);
long truncate(const char* path, unsigned long size);
long rename(const char* old_path, const char* new_path);
long waitpid(int pid, int* status);
long yield(void);
long sleep_ms(unsigned long milliseconds);
unsigned long uptime_ms(void);
long clear_screen(void);
long proc_info(unsigned long index, struct savanxp_process_info* info);
void exit(int code) __attribute__((noreturn));

size_t strlen(const char* text);
int strcmp(const char* left, const char* right);
int strncmp(const char* left, const char* right, size_t count);
char* strcpy(char* destination, const char* source);
void* memcpy(void* destination, const void* source, size_t count);
void* memset(void* destination, int value, size_t count);

void putchar(int fd, char character);
void puts_fd(int fd, const char* text);
void puts(const char* text);
void printf(const char* format, ...);
