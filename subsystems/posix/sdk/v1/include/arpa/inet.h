#pragma once

#include "netinet/in.h"


unsigned long htonl(unsigned long value);
unsigned short htons(unsigned short value);
unsigned long ntohl(unsigned long value);
unsigned short ntohs(unsigned short value);
in_addr_t inet_addr(const char* text);
char* inet_ntoa(struct in_addr address);
int inet_pton(int family, const char* source, void* destination);
const char* inet_ntop(int family, const void* source, char* destination, unsigned long size);
