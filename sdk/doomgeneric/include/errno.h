#pragma once

#define ENOENT 2
#define EIO 5
#define ENOMEM 12
#define EACCES 13
#define EEXIST 17
#define EISDIR 21
#define EINVAL 22
#define ENOSPC 28

#define errno (*sx_errno_location())

int *sx_errno_location(void);
