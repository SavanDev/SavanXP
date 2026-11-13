/*
 * codepage.c -- Windows-1252 <-> UTF-16. See codepage.h.
 */
#include "codepage.h"

/* 0x80..0x9F: where 1252 differs from Latin-1. 0 marks an undefined byte. */
static const unsigned short k_high[32] = {
    0x20ac, 0,      0x201a, 0x0192, 0x201e, 0x2026, 0x2020, 0x2021,
    0x02c6, 0x2030, 0x0160, 0x2039, 0x0152, 0,      0x017d, 0,
    0,      0x2018, 0x2019, 0x201c, 0x201d, 0x2022, 0x2013, 0x2014,
    0x02dc, 0x2122, 0x0161, 0x203a, 0x0153, 0,      0x017e, 0x0178,
};

unsigned short sxw_ansi_to_wide(unsigned char c) {
    if (c >= 0x80u && c <= 0x9fu && k_high[c - 0x80u] != 0u) {
        return k_high[c - 0x80u];
    }
    return c;
}

unsigned char sxw_wide_to_ansi(unsigned short c) {
    if (c < 0x80u || (c >= 0xa0u && c <= 0xffu)) {
        return (unsigned char)c;
    }
    for (unsigned i = 0; i < 32u; ++i) {
        if (k_high[i] == c && c != 0u) {
            return (unsigned char)(0x80u + i);
        }
    }
    if (c >= 0x80u && c <= 0x9fu && k_high[c - 0x80u] == 0u) {
        return (unsigned char)c; /* an undefined byte, back to itself */
    }
    return '?';
}
