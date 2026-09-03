#include "libc.h"

int main(int argc, char** argv) {
    for (int index = 1; index < argc; ++index) {
        puts_out(argv[index]);
        if (index + 1 < argc) {
            putchar_fd(1, ' ');
        }
    }
    putchar_fd(1, '\n');
    return 0;
}
