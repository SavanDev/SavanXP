#include "libc.h"

int main(int argc, const char* const* argv) {
    if (argc < 2) {
        puts_fd(2, "usage: rmdir <path>\n");
        return 1;
    }

    if (rmdir(argv[1]) < 0) {
        puts_fd(2, "rmdir: failed\n");
        return 1;
    }

    return 0;
}
