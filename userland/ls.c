#include "libc.h"

int main(int argc, char** argv) {
    const char* path = argc > 1 ? argv[1] : "/";
    char entry[64];
    long fd = open(path);
    if (fd < 0) {
        puts("ls: path not found\n");
        return 1;
    }

    for (;;) {
        long length = readdir((int)fd, entry, sizeof(entry));
        if (length <= 0) {
            break;
        }
        puts(entry);
        putchar(1, '\n');
    }

    close((int)fd);
    return 0;
}
