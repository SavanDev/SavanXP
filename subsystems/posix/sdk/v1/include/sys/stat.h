#pragma once

#include "sys/types.h"

#include <time.h>

/* El kernel solo reporta tipo y tamano (struct savanxp_stat). El resto de los
 * campos existe porque el codigo de terceros los nombra, y sx_stat los deja en
 * cero: SVFS2 todavia no guarda duenio ni marcas de tiempo. Que esten en cero
 * es visible y consistente; no tenerlos rompia la compilacion. */
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

/* mode se acepta y se ignora: SVFS2 no tiene permisos. */
int mkdir(const char* path, mode_t mode);
int stat(const char* path, struct stat* info);
int fstat(int fd, struct stat* info);
int lstat(const char* path, struct stat* info);
