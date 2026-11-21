#pragma once

#define O_RDONLY 0x0000
#define O_WRONLY 0x0001
#define O_RDWR 0x0002
#define O_CREAT 0x0100
#define O_EXCL 0x0080
#define O_TRUNC 0x0200
#define O_APPEND 0x0400
#define O_NONBLOCK 0x0800

#define F_DUPFD 0
#define F_GETFL 1
#define F_SETFL 2
#define F_GETFD 3
#define F_SETFD 4
/* Record locks (docs/kernel/FILE_LOCKS.md): advisory, owned by the process,
 * released when it closes any descriptor of the file, and not inherited by
 * fork. */
#define F_GETLK 5
#define F_SETLK 6
#define F_SETLKW 7

#define F_RDLCK 0
#define F_WRLCK 1
#define F_UNLCK 2

/* l_whence, as <stdio.h> defines them. */
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

#include "sys/types.h"

struct flock {
    short l_type;
    short l_whence;
    off_t l_start;
    off_t l_len;
    pid_t l_pid;
};

/* No hay exec-on-close que aplicar -- el exec de SavanXP no hereda una tabla de
 * fds marcada --, pero el flag tiene que existir: un port lo setea y espera que
 * el fcntl no falle. */
#define FD_CLOEXEC 1


int open(const char* path, int flags, ...);
int fcntl(int fd, int command, ...);
