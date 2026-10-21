#include <string.h>

#include "coreutils.h"

/* echo [-n] [text...]: prints its arguments separated by spaces. */
int main(int argc, char** argv) {
    int newline = 1;
    int index = 1;

    if (index < argc && strcmp(argv[index], "-n") == 0) {
        newline = 0;
        ++index;
    }
    for (; index < argc; ++index) {
        if (cu_write_all(1, argv[index], strlen(argv[index])) != 0) {
            return 1;
        }
        if (index + 1 < argc && cu_write_all(1, " ", 1) != 0) {
            return 1;
        }
    }
    if (newline && cu_write_all(1, "\n", 1) != 0) {
        return 1;
    }
    return 0;
}
