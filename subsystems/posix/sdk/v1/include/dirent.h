#pragma once

typedef struct sx_DIR DIR;

struct dirent {
    unsigned char d_type;
    char d_name[256];
};

#define DT_UNKNOWN 0
#define DT_FIFO 1
#define DT_CHR 2
#define DT_DIR 4
#define DT_REG 8
#define DT_SOCK 12

#define opendir sx_opendir
#define readdir sx_readdir
#define closedir sx_closedir
#define rewinddir sx_rewinddir

DIR* sx_opendir(const char* path);
struct dirent* sx_readdir(DIR* directory);
int sx_closedir(DIR* directory);
void sx_rewinddir(DIR* directory);
