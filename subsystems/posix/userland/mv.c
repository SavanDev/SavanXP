#include "libc.h"

int main(int argc, const char* const* argv) {
    if (argc < 3) {
        puts_fd(2, "usage: mv <old> <new>\n");
        return 1;
    }

    if (rename(argv[1], argv[2]) < 0) {
        puts_fd(2, "mv: failed\n");
        return 1;
    }

    return 0;
}
