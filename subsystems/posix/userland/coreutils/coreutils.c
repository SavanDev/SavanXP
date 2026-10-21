#include "coreutils.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

void cu_fail(const char* program, const char* what, const char* path) {
    fprintf(stderr, "%s: %s '%s': %s\n", program, what, path, strerror(errno));
}

const char* cu_basename(const char* path) {
    static char buffer[256];
    size_t length = strlen(path);

    while (length > 1 && path[length - 1] == '/') {
        --length;
    }
    size_t start = length;
    while (start > 0 && path[start - 1] != '/') {
        --start;
    }
    size_t size = length - start;
    if (size >= sizeof(buffer)) {
        size = sizeof(buffer) - 1;
    }
    memcpy(buffer, path + start, size);
    buffer[size] = '\0';
    return buffer;
}

int cu_join(char* out, size_t capacity, const char* directory, const char* name) {
    const size_t dir_length = strlen(directory);
    const int needs_slash = dir_length > 0 && directory[dir_length - 1] != '/';
    const int written = snprintf(out, capacity, needs_slash ? "%s/%s" : "%s%s", directory, name);

    if (written < 0 || (size_t)written >= capacity) {
        errno = ENAMETOOLONG;
        return -1;
    }
    return 0;
}

int cu_is_directory(const char* path) {
    struct stat info;

    return stat(path, &info) == 0 && S_ISDIR(info.st_mode);
}

int cu_write_all(int fd, const void* buffer, size_t length) {
    const char* cursor = (const char*)buffer;

    while (length > 0) {
        const ssize_t written = write(fd, cursor, length);
        if (written <= 0) {
            if (written == 0) {
                errno = EIO;
            }
            return -1;
        }
        cursor += written;
        length -= (size_t)written;
    }
    return 0;
}

int cu_copy_fd(int in_fd, int out_fd) {
    char buffer[4096];

    for (;;) {
        const ssize_t got = read(in_fd, buffer, sizeof(buffer));
        if (got == 0) {
            return 0;
        }
        if (got < 0 || cu_write_all(out_fd, buffer, (size_t)got) != 0) {
            return -1;
        }
    }
}
