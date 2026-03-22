#pragma once

#include "sys/types.h"

#define SX_WAITPID_SELECT(_1, _2, _3, NAME, ...) NAME
#define waitpid(...) SX_WAITPID_SELECT(__VA_ARGS__, sx_waitpid, sx_waitpid_default)(__VA_ARGS__)
#define WNOHANG 1
#define WUNTRACED 2
#define WCONTINUED 4
#define WEXITSTATUS(status) (status)
#define WIFEXITED(status) ((status) >= 0 && (status) < 128)
#define WIFSIGNALED(status) ((status) >= 128)
#define WTERMSIG(status) ((status) >= 128 ? ((status) - 128) : 0)
#define WCOREDUMP(status) (0)

pid_t sx_waitpid(pid_t pid, int* status, int options);

static inline pid_t sx_waitpid_default(pid_t pid, int* status) {
    return sx_waitpid(pid, status, 0);
}
