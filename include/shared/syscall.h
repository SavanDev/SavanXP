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
};

enum savanxp_open_flags {
    SAVANXP_OPEN_READ = 1u << 0,
    SAVANXP_OPEN_WRITE = 1u << 1,
    SAVANXP_OPEN_CREATE = 1u << 2,
    SAVANXP_OPEN_TRUNCATE = 1u << 3,
};
