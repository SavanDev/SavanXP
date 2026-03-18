#pragma once

#include <stddef.h>

#include "sys/types.h"

#define F_OK 0
#define R_OK 4
#define W_OK 2
#define X_OK 1

#define read sx_read
#define write sx_write
#define close sx_close
#define lseek sx_lseek
#define unlink sx_unlink
#define rmdir sx_rmdir
#define access sx_access
#define isatty sx_isatty
#define chdir sx_chdir
#define getcwd sx_getcwd
#define getpid sx_getpid
#define sync sx_sync
#define dup sx_dup
#define dup2 sx_dup2
#define pipe sx_pipe
#define fork sx_fork
#define sleep sx_sleep
#define usleep sx_usleep

ssize_t sx_read(int fd, void* buffer, size_t count);
ssize_t sx_write(int fd, const void* buffer, size_t count);
int sx_close(int fd);
off_t sx_lseek(int fd, off_t offset, int whence);
int sx_unlink(const char* path);
int sx_rmdir(const char* path);
int sx_access(const char* path, int mode);
int sx_isatty(int fd);
int sx_chdir(const char* path);
char* sx_getcwd(char* buffer, size_t size);
pid_t sx_getpid(void);
int sx_sync(void);
int sx_dup(int fd);
int sx_dup2(int oldfd, int newfd);
int sx_pipe(int fds[2]);
pid_t sx_fork(void);
unsigned sx_sleep(unsigned seconds);
int sx_usleep(unsigned long microseconds);
