#include "libc.h"

int main(int argc, char** argv) {
    if (argc <= 1) {
        puts_fd(2, "errtest: stderr works\n");
        return 0;
    }

    for (int index = 1; index < argc; ++index) {
        if (index > 1) {
            putchar_fd(2, ' ');
        }
        puts_fd(2, argv[index]);
    }
    putchar_fd(2, '\n');
    return 0;
}
