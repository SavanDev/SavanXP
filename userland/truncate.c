#include "libc.h"

static unsigned long parse_unsigned(const char* text) {
    unsigned long value = 0;
    for (size_t index = 0; text[index] != '\0'; ++index) {
        if (text[index] < '0' || text[index] > '9') {
            return (unsigned long)-1;
        }
        value = (value * 10) + (unsigned long)(text[index] - '0');
    }
    return value;
}

int main(int argc, const char* const* argv) {
    if (argc < 3) {
        puts_fd(2, "usage: truncate <path> <size>\n");
        return 1;
    }

    const unsigned long size = parse_unsigned(argv[2]);
    if (size == (unsigned long)-1) {
        puts_fd(2, "truncate: invalid size\n");
        return 1;
    }

    if (truncate(argv[1], size) < 0) {
        puts_fd(2, "truncate: failed\n");
        return 1;
    }

    return 0;
}
