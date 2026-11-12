/* One line to standard output, for the init-order fixture's three images. */
#ifndef SXW_FIXTURE_SAY_H
#define SXW_FIXTURE_SAY_H

#include <windows.h>

static inline void say(const char *text) {
    DWORD length = 0;
    while (text[length] != 0) {
        ++length;
    }
    DWORD written;
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), text, length, &written, NULL);
}

static inline void say_digit(const char *before, unsigned value, const char *after) {
    char digit[2] = {(char)('0' + value % 10u), 0};
    say(before);
    say(digit);
    say(after);
}

#endif
