#include "libc.h"

int main(int argc, char** argv) {
    char buffer[128];
    long fd = 0;

    if (argc >= 2) {
        fd = savanxp_open(argv[1]);
        if (fd < 0) {
            puts_out("cat: file not found\n");
            return 1;
        }
    }

    for (;;) {
        long count = savanxp_read((int)fd, buffer, sizeof(buffer));
        if (count <= 0) {
            break;
        }
        savanxp_write(1, buffer, (size_t)count);
    }

    if (fd > 0) {
        savanxp_close((int)fd);
    }
    return 0;
}
