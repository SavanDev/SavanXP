#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

int main(void) {
    struct stat info;
    DIR* directory;
    struct dirent* entry;
    int fd;
    char buffer[64];
    char* first;
    char* second;
    char* resized;

    if (getcwd(buffer, sizeof(buffer)) == 0) {
        printf("getcwd failed\n");
        return 1;
    }
    printf("cwd=%s pid=%d\n", buffer, (int)getpid());

    if (stat("/README", &info) != 0) {
        printf("stat failed\n");
        return 1;
    }

    fd = open("/README", O_RDONLY);
    if (fd < 0) {
        printf("open failed\n");
        return 1;
    }

    memset(buffer, 0, sizeof(buffer));
    if (read(fd, buffer, sizeof(buffer) - 1) < 0) {
        printf("read failed\n");
        close(fd);
        return 1;
    }
    close(fd);
    printf("read=%s\n", buffer);

    first = (char*)malloc(24);
    if (first == 0) {
        printf("malloc failed\n");
        return 1;
    }
    strcpy(first, "heap-ok");
    resized = (char*)realloc(first, 64);
    if (resized == 0) {
        printf("realloc failed\n");
        free(first);
        return 1;
    }
    strcpy(resized + 7, "-grow");
    printf("heap=%s\n", resized);
    free(resized);

    first = (char*)malloc(24);
    second = (char*)malloc(24);
    if (first == 0 || second == 0) {
        printf("malloc reuse failed\n");
        free(first);
        free(second);
        return 1;
    }
    free(first);
    first = (char*)malloc(24);
    if (first == 0) {
        printf("malloc recycle failed\n");
        free(second);
        return 1;
    }
    printf("recycled=%p second=%p\n", first, second);
    free(first);
    free(second);

    directory = opendir("/");
    if (directory == 0) {
        printf("opendir failed\n");
        return 1;
    }
    while ((entry = readdir(directory)) != 0) {
        printf("dir:%s\n", entry->d_name);
    }
    closedir(directory);
    return 0;
}
