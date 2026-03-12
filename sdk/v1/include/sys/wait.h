#pragma once

#include "sys/types.h"

#define waitpid sx_waitpid
#define WEXITSTATUS(status) (status)
#define WIFEXITED(status) (1)

pid_t sx_waitpid(pid_t pid, int* status, int options);
