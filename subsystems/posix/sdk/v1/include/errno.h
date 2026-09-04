#pragma once

#define ESRCH 3
#define EINTR 4
#define ENOENT 2
#define EIO 5
#define ENOEXEC 8
#define EBADF 9
#define ECHILD 10
#define EAGAIN 11
#define ENOMEM 12
#define EACCES 13
#define EBUSY 16
#define EEXIST 17
#define ENODEV 19
#define ENOTDIR 20
#define EISDIR 21
#define EINVAL 22
#define ENOTTY 25
#define ENOSPC 28
#define EPIPE 32
#define ERANGE 34
#define ENAMETOOLONG 36
#define ENOSYS 38
#define ELOOP 40
#define ENOTEMPTY 39
#define ETIMEDOUT 110

/* Los que faltaban. No es cosmetico: libavutil/error.c arma su tabla de
 * errores con #if EDOM > 0, y un codigo sin definir evalua a 0 y deja la tabla
 * vacia -- el sintoma no es "falta EDOM" sino un sizeof sobre un arreglo
 * incompleto varias lineas mas abajo. */
#define EPERM 1
#define ENXIO 6
#define E2BIG 7
#define EDEADLK 35
#define ENOLCK 37
#define EDOM 33
#define EFAULT 14
#define EXDEV 18
#define EMFILE 24
#define ENFILE 23
#define EFBIG 27
#define ESPIPE 29
#define EROFS 30
#define EMLINK 31
#define EILSEQ 84
#define ENOBUFS 105
#define EPROTO 71

#define errno (*sx_errno_location())

int* sx_errno_location(void);
