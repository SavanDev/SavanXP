#pragma once

#include <stddef.h>

#include "sys/types.h"

#define F_OK 0
#define R_OK 4
#define W_OK 2
#define X_OK 1


ssize_t read(int fd, void* buffer, size_t count);
ssize_t write(int fd, const void* buffer, size_t count);
int close(int fd);
off_t lseek(int fd, off_t offset, int whence);
int unlink(const char* path);
int rmdir(const char* path);
int access(const char* path, int mode);
int isatty(int fd);
int chdir(const char* path);
char* getcwd(char* buffer, size_t size);
pid_t getpid(void);
pid_t getppid(void);
pid_t getpgrp(void);
int setpgid(pid_t pid, pid_t pgrp);
pid_t setsid(void);
int sync(void);
int dup(int fd);
int dup2(int oldfd, int newfd);
int pipe(int fds[2]);
pid_t fork(void);
pid_t vfork(void);
int execv(const char* path, char* const argv[]);
int execve(const char* path, char* const argv[], char* const envp[]);
int execvp(const char* file, char* const argv[]);
void _exit(int code) __attribute__((noreturn));
uid_t getuid(void);
uid_t geteuid(void);
gid_t getgid(void);
gid_t getegid(void);
mode_t umask(mode_t mask);
unsigned sleep(unsigned seconds);
int usleep(unsigned long microseconds);
