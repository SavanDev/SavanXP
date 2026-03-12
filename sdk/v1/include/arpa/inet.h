#pragma once

#include "netinet/in.h"

#define htonl sx_htonl
#define htons sx_htons
#define ntohl sx_ntohl
#define ntohs sx_ntohs
#define inet_addr sx_inet_addr
#define inet_ntoa sx_inet_ntoa
#define inet_pton sx_inet_pton
#define inet_ntop sx_inet_ntop

unsigned long sx_htonl(unsigned long value);
unsigned short sx_htons(unsigned short value);
unsigned long sx_ntohl(unsigned long value);
unsigned short sx_ntohs(unsigned short value);
in_addr_t sx_inet_addr(const char* text);
char* sx_inet_ntoa(struct in_addr address);
int sx_inet_pton(int family, const char* source, void* destination);
const char* sx_inet_ntop(int family, const void* source, char* destination, unsigned long size);
