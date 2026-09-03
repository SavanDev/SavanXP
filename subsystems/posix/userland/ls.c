#include "libc.h"

int main(int argc, char** argv) {
    const char* path = argc > 1 ? argv[1] : ".";
    char entry[64];
    long fd = savanxp_open(path);
    if (fd < 0) {
        puts_out("ls: path not found\n");
        return 1;
    }

    for (;;) {
        long length = savanxp_readdir((int)fd, entry, sizeof(entry));
        if (length <= 0) {
            break;
        }
        puts_out(entry);
        putchar_fd(1, '\n');
    }

    savanxp_close((int)fd);
    return 0;
}
