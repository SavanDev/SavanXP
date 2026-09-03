#pragma once

#include <stddef.h>

#include "../poll.h"
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


int socket(int domain, int type, int protocol);
int bind(int fd, const struct sockaddr* address, socklen_t address_length);
int connect(int fd, const struct sockaddr* address, socklen_t address_length);
ssize_t sendto(int fd, const void* buffer, size_t count, int flags, const struct sockaddr* address, socklen_t address_length);
ssize_t recvfrom(int fd, void* buffer, size_t count, int flags, struct sockaddr* address, socklen_t* address_length);
int setsockopt(int fd, int level, int option_name, const void* option_value, socklen_t option_length);
int getsockopt(int fd, int level, int option_name, void* option_value, socklen_t* option_length);
int shutdown(int fd, int how);
