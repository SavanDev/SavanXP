#pragma once

#include <stdint.h>

enum savanxp_syscall_number {
    SAVANXP_SYS_READ = 0,
    SAVANXP_SYS_WRITE = 1,
    SAVANXP_SYS_OPEN = 2,
    SAVANXP_SYS_CLOSE = 3,
    SAVANXP_SYS_READDIR = 4,
    SAVANXP_SYS_SPAWN = 5,
    SAVANXP_SYS_WAITPID = 6,
    SAVANXP_SYS_EXIT = 7,
    SAVANXP_SYS_YIELD = 8,
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
};

enum savanxp_open_flags {
    SAVANXP_OPEN_READ = 1u << 0,
    SAVANXP_OPEN_WRITE = 1u << 1,
    SAVANXP_OPEN_CREATE = 1u << 2,
    SAVANXP_OPEN_TRUNCATE = 1u << 3,
    SAVANXP_OPEN_APPEND = 1u << 4,
};

enum savanxp_error_code {
    SAVANXP_EINVAL = 22,
    SAVANXP_EBADF = 9,
    SAVANXP_ENOENT = 2,
    SAVANXP_ENOMEM = 12,
    SAVANXP_EBUSY = 16,
    SAVANXP_EEXIST = 17,
    SAVANXP_EISDIR = 21,
    SAVANXP_ENOTDIR = 20,
    SAVANXP_ENOSPC = 28,
    SAVANXP_EPIPE = 32,
    SAVANXP_ENOSYS = 38,
    SAVANXP_ENOTEMPTY = 39,
    SAVANXP_ECHILD = 10,
};

enum savanxp_seek_whence {
    SAVANXP_SEEK_SET = 0,
    SAVANXP_SEEK_CUR = 1,
    SAVANXP_SEEK_END = 2,
};

enum savanxp_process_state {
    SAVANXP_PROC_UNUSED = 0,
    SAVANXP_PROC_READY = 1,
    SAVANXP_PROC_RUNNING = 2,
    SAVANXP_PROC_BLOCKED_READ = 3,
    SAVANXP_PROC_BLOCKED_WRITE = 4,
    SAVANXP_PROC_BLOCKED_WAIT = 5,
    SAVANXP_PROC_SLEEPING = 6,
    SAVANXP_PROC_ZOMBIE = 7,
};

struct savanxp_process_info {
    uint32_t pid;
    uint32_t parent_pid;
    int32_t exit_code;
    uint32_t state;
    char name[32];
};
