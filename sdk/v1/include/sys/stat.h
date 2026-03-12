#pragma once

#include "sys/types.h"

struct stat {
    unsigned int st_mode;
    unsigned int st_size;
};

#define S_IFMT 0170000u
#define S_IFREG 0100000u
#define S_IFDIR 0040000u
#define S_IFCHR 0020000u
#define S_IFIFO 0010000u
#define S_IFSOCK 0140000u

#define S_ISREG(mode) (((mode) & S_IFMT) == S_IFREG)
#define S_ISDIR(mode) (((mode) & S_IFMT) == S_IFDIR)
#define S_ISCHR(mode) (((mode) & S_IFMT) == S_IFCHR)
#define S_ISFIFO(mode) (((mode) & S_IFMT) == S_IFIFO)
#define S_ISSOCK(mode) (((mode) & S_IFMT) == S_IFSOCK)

int sx_stat(const char* path, struct stat* info);
int sx_fstat(int fd, struct stat* info);

static inline int stat(const char* path, struct stat* info) {
    return sx_stat(path, info);
}

static inline int fstat(int fd, struct stat* info) {
    return sx_fstat(fd, info);
}
