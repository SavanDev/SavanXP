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
    const char* label = argc > 1 ? argv[1] : "tick";
    unsigned long count = argc > 2 ? parse_number(argv[2]) : 5;
    unsigned long delay = argc > 3 ? parse_number(argv[3]) : 120;

    for (unsigned long index = 0; index < count; ++index) {
        printf("%s %u\n", label, (unsigned int)index);
        sleep_ms(delay);
    }

    return 0;
}
