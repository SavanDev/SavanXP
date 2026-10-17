#pragma once

/*
 * The POSIX userland surface: system-call numbers, open flags, mode bits,
 * seek, fcntl, poll events, signals and socket options, plus everything the
 * SDK declares on top of them. The data contract shared with the kernel is
 * abi/savanxp_abi.h, which is included below.
 *
 * Nothing in kernel/ may include this file.
 */

#include "abi/savanxp_abi.h"

enum savanxp_syscall_number {
    SAVANXP_SYS_READ = 0,
    SAVANXP_SYS_WRITE = 1,
    SAVANXP_SYS_OPEN = 2,
    SAVANXP_SYS_CLOSE = 3,
    SAVANXP_SYS_READDIR = 4,
    SAVANXP_SYS_SPAWN = 5,
    SAVANXP_SYS_WAITPID = 6,
    SAVANXP_SYS_EXIT = 7,
    SAVANXP_SYS_UPTIME_MS = 9,
    SAVANXP_SYS_CLEAR = 10,
    SAVANXP_SYS_SLEEP_MS = 11,
    SAVANXP_SYS_PIPE = 12,
    SAVANXP_SYS_SPAWN_FD = 13,
    SAVANXP_SYS_DUP = 14,
    SAVANXP_SYS_DUP2 = 15,
    SAVANXP_SYS_PROC_INFO = 16,
    SAVANXP_SYS_SEEK = 17,
    SAVANXP_SYS_UNLINK = 18,
    SAVANXP_SYS_EXEC = 19,
    SAVANXP_SYS_MKDIR = 20,
    SAVANXP_SYS_RMDIR = 21,
    SAVANXP_SYS_TRUNCATE = 22,
    SAVANXP_SYS_RENAME = 23,
    SAVANXP_SYS_IOCTL = 24,
    SAVANXP_SYS_SOCKET = 25,
    SAVANXP_SYS_BIND = 26,
    SAVANXP_SYS_SENDTO = 27,
    SAVANXP_SYS_RECVFROM = 28,
    SAVANXP_SYS_CONNECT = 29,
    SAVANXP_SYS_GETPID = 30,
    SAVANXP_SYS_STAT = 31,
    SAVANXP_SYS_FSTAT = 32,
    SAVANXP_SYS_CHDIR = 33,
    SAVANXP_SYS_GETCWD = 34,
    SAVANXP_SYS_SYSTEM_INFO = 35,
    SAVANXP_SYS_SYNC = 36,
    SAVANXP_SYS_REALTIME = 37,
    SAVANXP_SYS_FCNTL = 38,
    SAVANXP_SYS_POLL = 39,
    SAVANXP_SYS_FORK = 40,
    SAVANXP_SYS_KILL = 41,
    SAVANXP_SYS_EVENT_CREATE = 42,
    SAVANXP_SYS_EVENT_SET = 43,
    SAVANXP_SYS_EVENT_RESET = 44,
    SAVANXP_SYS_WAIT_ONE = 45,
    SAVANXP_SYS_WAIT_MANY = 46,
    SAVANXP_SYS_TIMER_CREATE = 47,
    SAVANXP_SYS_TIMER_SET = 48,
    SAVANXP_SYS_TIMER_CANCEL = 49,
    SAVANXP_SYS_SECTION_CREATE = 50,
    SAVANXP_SYS_MAP_VIEW = 51,
    SAVANXP_SYS_UNMAP_VIEW = 52,
    SAVANXP_SYS_SEMAPHORE_CREATE = 53,
    SAVANXP_SYS_SEMAPHORE_RELEASE = 54,
    /* Reloj monotono en nanosegundos desde el boot, por TSC. A diferencia de
     * UPTIME_MS avanza con las interrupciones deshabilitadas y tiene
     * resolucion suficiente para medir un frame. */
    SAVANXP_SYS_MONOTONIC_NS = 55,
    /* Opciones de socket. El valor es un escalar y no un puntero porque las
     * unicas que el kernel entiende son los dos plazos, en milisegundos. */
    SAVANXP_SYS_SETSOCKOPT = 56,
    /* Paso de handles por pipe (estilo I_SENDFD): el extremo de escritura deja
     * una seccion, un evento, un semaforo o un timer en una cola chica del pipe,
     * y el de lectura lo recibe como un fd nuevo con el mismo acceso. No
     * viajan fds de archivos, pipes ni sockets. Recibir nunca bloquea: con la
     * cola vacia devuelve EAGAIN. Ver docs/OWNED_WINDOWS.md. */
    SAVANXP_SYS_PIPE_SEND_HANDLE = 57,
    SAVANXP_SYS_PIPE_RECEIVE_HANDLE = 58,
    /* Informe de consistencia del volumen SxFS: solo lectura, sin reparacion.
     * Ver struct savanxp_fscheck_report. */
    SAVANXP_SYS_FSCHECK = 59,
    SAVANXP_SYS_SECTION_OPEN = 60,
    SAVANXP_SYS_MAP_VIEW_AT = 61,
    SAVANXP_SYS_SECTION_OPEN_RANGE = 62,
    /* Set the CMOS real-time clock from a struct savanxp_realtime (UTC, same
     * layout as REALTIME; the valid flag is ignored on input). Years are
     * 2000..2099, the day must exist in its month. Returns 0, -EINVAL for a
     * bad date or address, -EIO if the chip never goes idle. */
    SAVANXP_SYS_SET_REALTIME = 63,
};
enum savanxp_mode_bits {
    SAVANXP_S_IFMT = 0170000u,
    SAVANXP_S_IFREG = 0100000u,
    SAVANXP_S_IFDIR = 0040000u,
    SAVANXP_S_IFCHR = 0020000u,
    SAVANXP_S_IFIFO = 0010000u,
    SAVANXP_S_IFSOCK = 0140000u,
};
enum savanxp_seek_whence {
    SAVANXP_SEEK_SET = 0,
    SAVANXP_SEEK_CUR = 1,
    SAVANXP_SEEK_END = 2,
};
enum savanxp_fcntl_command {
    SAVANXP_F_GETFL = 1,
    SAVANXP_F_SETFL = 2,
};
