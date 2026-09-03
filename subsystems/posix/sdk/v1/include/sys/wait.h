#pragma once

#include "sys/types.h"

#define WNOHANG 1
#define WUNTRACED 2
#define WCONTINUED 4
#define WEXITSTATUS(status) (status)
#define WIFEXITED(status) ((status) >= 0 && (status) < 128)
#define WIFSIGNALED(status) ((status) >= 128)
#define WTERMSIG(status) ((status) >= 128 ? ((status) - 128) : 0)
#define WCOREDUMP(status) (0)

pid_t waitpid(pid_t pid, int* status, int options);
