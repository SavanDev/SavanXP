#include "libc.h"

int main(int argc, char** argv) {
    char buffer[128];
    long fd = 0;

    if (argc >= 2) {
        fd = open(argv[1]);
        if (fd < 0) {
            puts("cat: file not found\n");
            return 1;
        }
    }

    for (;;) {
        long count = read((int)fd, buffer, sizeof(buffer));
        if (count <= 0) {
            break;
        }
        write(1, buffer, (size_t)count);
    }

    if (fd > 0) {
        close((int)fd);
    }
    return 0;
}
