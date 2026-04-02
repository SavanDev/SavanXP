#pragma once

#define O_RDONLY 0x0000
#define O_WRONLY 0x0001
#define O_RDWR 0x0002
#define O_CREAT 0x0100
#define O_EXCL 0x0080
#define O_TRUNC 0x0200
#define O_APPEND 0x0400
#define O_NONBLOCK 0x0800

#define F_DUPFD 0
#define F_GETFL 1
#define F_SETFL 2

#define open sx_open
#define fcntl sx_fcntl

int sx_open(const char* path, int flags, ...);
int sx_fcntl(int fd, int command, ...);
