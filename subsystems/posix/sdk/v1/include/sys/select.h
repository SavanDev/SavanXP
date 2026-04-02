#pragma once

#include "../poll.h"
#include "sys/types.h"

struct timeval {
    long tv_sec;
    long tv_usec;
};

#ifndef FD_SETSIZE
#define FD_SETSIZE 64
#endif

typedef struct fd_set {
    unsigned long bits[(FD_SETSIZE + (8 * sizeof(unsigned long)) - 1) / (8 * sizeof(unsigned long))];
} fd_set;

#define FD_ZERO(set) sx_fd_zero((set))
#define FD_SET(fd, set) sx_fd_set((fd), (set))
#define FD_CLR(fd, set) sx_fd_clr((fd), (set))
#define FD_ISSET(fd, set) sx_fd_isset((fd), (set))
#define select sx_select

void sx_fd_zero(fd_set* set);
void sx_fd_set(int fd, fd_set* set);
void sx_fd_clr(int fd, fd_set* set);
int sx_fd_isset(int fd, const fd_set* set);
int sx_select(int nfds, fd_set* readfds, fd_set* writefds, fd_set* exceptfds, struct timeval* timeout);
