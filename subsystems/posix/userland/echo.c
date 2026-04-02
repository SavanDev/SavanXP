#include "libc.h"

int main(int argc, char** argv) {
    for (int index = 1; index < argc; ++index) {
        puts(argv[index]);
        if (index + 1 < argc) {
            putchar(1, ' ');
        }
    }
    putchar(1, '\n');
    return 0;
}
