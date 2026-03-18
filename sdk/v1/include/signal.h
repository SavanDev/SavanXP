#pragma once

#include "sys/types.h"

#define SIGINT 2
#define SIGKILL 9
#define SIGPIPE 13
#define SIGTERM 15
#define SIGCHLD 17

#define kill sx_kill
#define raise sx_raise

int sx_kill(pid_t pid, int signal_number);
int sx_raise(int signal_number);
