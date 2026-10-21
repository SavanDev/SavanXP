#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "coreutils.h"

/* Creates every missing component of path, like mkdir -p. */
static int make_parents(const char* path) {
    char buffer[256];
    const size_t length = strlen(path);

    if (length >= sizeof(buffer)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    memcpy(buffer, path, length + 1);
    for (size_t index = 1; index <= length; ++index) {
        if (buffer[index] != '/' && buffer[index] != '\0') {
            continue;
        }
        const char saved = buffer[index];
        buffer[index] = '\0';
        if (!cu_is_directory(buffer) && mkdir(buffer, 0755) != 0 && errno != EEXIST) {
            return -1;
        }
        buffer[index] = saved;
    }
    return 0;
}

/* mkdir [-p] dir... */
int main(int argc, char** argv) {
    int parents = 0;
    int status = 0;
    int index = 1;

    if (index < argc && strcmp(argv[index], "-p") == 0) {
        parents = 1;
        ++index;
    }
    if (index >= argc) {
        fputs("usage: mkdir [-p] <dir>...\n", stderr);
        return 1;
    }
    for (; index < argc; ++index) {
        const int result = parents ? make_parents(argv[index]) : mkdir(argv[index], 0755);
        if (result != 0) {
            cu_fail("mkdir", "can't create directory", argv[index]);
            status = 1;
        }
    }
    return status;
}
