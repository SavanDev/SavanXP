#include "libc.h"

static unsigned long parse_number(const char* text) {
    unsigned long value = 0;
    for (size_t index = 0; text[index] != '\0'; ++index) {
        if (text[index] < '0' || text[index] > '9') {
            return 0;
        }
        value = value * 10 + (unsigned long)(text[index] - '0');
    }
    return value;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        puts("sleep: missing milliseconds\n");
        return 1;
    }

    sleep_ms(parse_number(argv[1]));
    return 0;
}
