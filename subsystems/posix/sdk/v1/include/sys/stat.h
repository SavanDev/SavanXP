#pragma once

#include "sys/types.h"

#include <time.h>

/* Filled from struct savanxp_stat_ex: type, permissions, size, owner, group
 * and the modification time (0 when the file system recorded none). atime and
 * ctime repeat mtime; nothing keeps them apart. */
struct stat {
    unsigned long st_dev;
    unsigned long st_ino;
    unsigned int st_mode;
    unsigned int st_size;
    unsigned int st_uid;
    unsigned int st_gid;
    unsigned long st_nlink;
    unsigned long st_blksize;
    unsigned long st_blocks;
    time_t st_atime;
    time_t st_mtime;
    time_t st_ctime;
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

/* mode, less the umask, is what the directory gets
 * (docs/kernel/USERS_AND_PERMISSIONS.md). */
int mkdir(const char* path, mode_t mode);
int chmod(const char* path, mode_t mode);
int fchmod(int fd, mode_t mode);
int stat(const char* path, struct stat* info);
int fstat(int fd, struct stat* info);
int lstat(const char* path, struct stat* info);
