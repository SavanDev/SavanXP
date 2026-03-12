#pragma once

#include <stddef.h>

#include "sys/types.h"

struct sockaddr {
    unsigned short sa_family;
    char sa_data[14];
};

#define AF_INET 1

#define SOCK_DGRAM 1
#define SOCK_STREAM 2

#define SOL_SOCKET 1
#define SO_REUSEADDR 2
#define SO_BROADCAST 6
#define SO_RCVTIMEO 20
#define SO_SNDTIMEO 21

#define SHUT_RD 0
#define SHUT_WR 1
#define SHUT_RDWR 2

#define socket sx_socket
#define bind sx_bind
#define connect sx_connect
#define sendto sx_sendto
#define recvfrom sx_recvfrom
#define setsockopt sx_setsockopt
#define getsockopt sx_getsockopt
#define shutdown sx_shutdown

int sx_socket(int domain, int type, int protocol);
int sx_bind(int fd, const struct sockaddr* address, socklen_t address_length);
int sx_connect(int fd, const struct sockaddr* address, socklen_t address_length);
ssize_t sx_sendto(int fd, const void* buffer, size_t count, int flags, const struct sockaddr* address, socklen_t address_length);
ssize_t sx_recvfrom(int fd, void* buffer, size_t count, int flags, struct sockaddr* address, socklen_t* address_length);
int sx_setsockopt(int fd, int level, int option_name, const void* option_value, socklen_t option_length);
int sx_getsockopt(int fd, int level, int option_name, void* option_value, socklen_t* option_length);
int sx_shutdown(int fd, int how);
