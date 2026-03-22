#pragma once

#define ESRCH 3
#define EINTR 4
#define ENOENT 2
#define EIO 5
#define ENOEXEC 8
#define EBADF 9
#define ECHILD 10
#define EAGAIN 11
#define ENOMEM 12
#define EACCES 13
#define EBUSY 16
#define EEXIST 17
#define ENODEV 19
#define ENOTDIR 20
#define EISDIR 21
#define EINVAL 22
#define ENOTTY 25
#define ENOSPC 28
#define EPIPE 32
#define ERANGE 34
#define ENAMETOOLONG 36
#define ENOSYS 38
#define ELOOP 40
#define ENOTEMPTY 39
#define ETIMEDOUT 110

#define errno (*sx_errno_location())

int* sx_errno_location(void);
