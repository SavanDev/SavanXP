/*
 * print.h -- Output for the WP3 and WP4 fixtures (WIN32_WORDPAD_ROADMAP.md):
 * lines on standard output with CRLF, numbers in decimal or hex, wide
 * strings in Windows-1252, and the last error. Everything printed must be
 * the same on Windows XP, in the harness and on SavanXP.
 */
#ifndef SAVANXP_WIN32_FIXTURE_PRINT_H
#define SAVANXP_WIN32_FIXTURE_PRINT_H

#include <windows.h>

DECLSPEC_IMPORT int WINAPI lstrlenA(LPCSTR s);

static __attribute__((unused)) void say(const char *text) {
    DWORD written;
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), text, (DWORD)lstrlenA(text), &written, NULL);
}

static __attribute__((unused)) void say_int(long value) {
    char digits[16];
    int n = 15;
    unsigned long magnitude = value < 0 ? 0ul - (unsigned long)value : (unsigned long)value;
    digits[n] = 0;
    do {
        digits[--n] = (char)('0' + magnitude % 10u);
        magnitude /= 10u;
    } while (magnitude != 0u);
    if (value < 0) {
        digits[--n] = '-';
    }
    say(digits + n);
}

static __attribute__((unused)) void say_hex(unsigned long value) {
    char digits[11] = "0x";
    for (int i = 0; i < 8; ++i) {
        const unsigned nibble = (unsigned)(value >> (28 - 4 * i)) & 0xfu;
        digits[2 + i] = (char)(nibble < 10u ? '0' + nibble : 'a' + nibble - 10u);
    }
    digits[10] = 0;
    say(digits);
}

/* A wide string in Windows-1252, in brackets. */
static __attribute__((unused)) void say_w(const WCHAR *text) {
    char narrow[600];
    const int n = WideCharToMultiByte(1252, 0, text, -1, narrow, sizeof narrow, NULL, NULL);
    say("[");
    say(n > 0 ? narrow : "?conversion failed?");
    say("]");
}

static __attribute__((unused)) void say_yes(int yes) {
    say(yes ? "yes" : "no");
}

/* A new line and its label. */
static __attribute__((unused)) void line(const char *label) {
    say("\r\n");
    say(label);
    say(": ");
}

static __attribute__((unused)) void say_error(void) {
    say(" error ");
    say_int((long)GetLastError());
}

/* A call's BOOL result and, when it failed, its error. */
static __attribute__((unused)) void say_result(BOOL ok) {
    say(ok ? "ok" : "failed");
    if (!ok) {
        say_error();
    }
}

#endif
