#pragma once

#include "sys/types.h"

struct pollfd {
    int fd;
    short events;
    short revents;
};

#define POLLIN 0x0001
#define POLLOUT 0x0004
#define POLLERR 0x0008
#define POLLHUP 0x0010
#define POLLNVAL 0x0020

#define poll sx_poll

int sx_poll(struct pollfd* fds, unsigned long count, int timeout_ms);
