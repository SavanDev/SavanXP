#pragma once

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdint.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

/* Address records used internally by the engine's protocol and text parser. */
#define AF_UNSPEC 0
#define AF_INET6 10
#define INADDR_BROADCAST ((in_addr_t)0xffffffffu)

struct in6_addr {
    uint8_t s6_addr[16];
};

struct sockaddr_in6 {
    unsigned short sin6_family;
    in_port_t sin6_port;
    uint32_t sin6_flowinfo;
    struct in6_addr sin6_addr;
    uint32_t sin6_scope_id;
};

struct sockaddr_storage {
    _Alignas(uint64_t) unsigned short ss_family;
    uint8_t padding[126];
};

#define INVALID_SOCKET (-1)
#define closesocket close
#define WSAGetLastError() errno
#define WSAEWOULDBLOCK EAGAIN
#define WSAECONNRESET ECONNRESET
#define WSAECONNREFUSED ECONNREFUSED
#define WSAETIMEDOUT ETIMEDOUT
#define WSAENOBUFS ENOBUFS
typedef socklen_t WSAsize_t;
typedef int SOCKET;
