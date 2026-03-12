#pragma once

#include <stdint.h>

#include "sys/socket.h"

typedef uint16_t in_port_t;
typedef uint32_t in_addr_t;

struct in_addr {
    in_addr_t s_addr;
};

struct sockaddr_in {
    unsigned short sin_family;
    in_port_t sin_port;
    struct in_addr sin_addr;
    unsigned char sin_zero[8];
};

#define INADDR_ANY ((in_addr_t)0x00000000u)

#define IPPROTO_TCP 6
#define IPPROTO_UDP 17
