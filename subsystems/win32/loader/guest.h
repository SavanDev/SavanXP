/*
 * guest.h -- Reading and writing guest memory for the host's services, each
 * access checked against the regions the loader mapped. Shared by
 * services.c, files.c and threads.c.
 */
#ifndef SAVANXP_WIN32_GUEST_H
#define SAVANXP_WIN32_GUEST_H

#include <stdint.h>

#include "loader.h"

int sxw_guest_range(const sxw_process *process, uint32_t address, uint32_t length);

static inline uint32_t sxw_get32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static inline void sxw_set32(uint8_t *p, uint32_t value) {
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
    p[2] = (uint8_t)(value >> 16);
    p[3] = (uint8_t)(value >> 24);
}

static inline int sxw_read_words(const sxw_process *process, uint32_t address, uint32_t count, uint32_t *out) {
    if (!sxw_guest_range(process, address, 4u * count)) {
        return 0;
    }
    const uint8_t *p = (const uint8_t *)(uintptr_t)address;
    for (uint32_t i = 0; i < count; ++i) {
        out[i] = sxw_get32(p + 4u * i);
    }
    return 1;
}

static inline int sxw_write_word(const sxw_process *process, uint32_t address, uint32_t value) {
    if (!sxw_guest_range(process, address, 4u)) {
        return 0;
    }
    sxw_set32((uint8_t *)(uintptr_t)address, value);
    return 1;
}

/* A NUL-terminated UTF-16 string of the guest as UTF-8, at most capacity
 * bytes with its NUL; surrogates become '?'. Address 0 is the empty string.
 * 0 when it is not guest memory or does not fit. */
static inline int sxw_read_utf16(const sxw_process *process, uint32_t address, char *out, uint32_t capacity) {
    uint32_t n = 0;
    out[0] = 0;
    if (address == 0u) {
        return 1;
    }
    for (uint32_t i = 0;; ++i) {
        if (!sxw_guest_range(process, address + 2u * i, 2u)) {
            return 0;
        }
        const uint8_t *p = (const uint8_t *)(uintptr_t)(address + 2u * i);
        uint32_t c = (uint32_t)p[0] | ((uint32_t)p[1] << 8);
        if (c == 0u) {
            out[n] = 0;
            return 1;
        }
        if (c >= 0xd800u && c <= 0xdfffu) {
            c = '?';
        }
        const uint32_t need = c < 0x80u ? 1u : c < 0x800u ? 2u : 3u;
        if (n + need + 1u > capacity) {
            return 0;
        }
        if (need == 1u) {
            out[n++] = (char)c;
        } else if (need == 2u) {
            out[n++] = (char)(0xc0u | (c >> 6));
            out[n++] = (char)(0x80u | (c & 0x3fu));
        } else {
            out[n++] = (char)(0xe0u | (c >> 12));
            out[n++] = (char)(0x80u | ((c >> 6) & 0x3fu));
            out[n++] = (char)(0x80u | (c & 0x3fu));
        }
    }
}

/* UTF-8 text into a guest UTF-16 buffer of capacity units, NUL-terminated
 * and truncated to fit; code points above U+FFFF become '?'. The units
 * written without the NUL, or -1 when the buffer is not guest memory. */
static inline int sxw_write_utf16(const sxw_process *process, uint32_t address, uint32_t capacity, const char *text) {
    if (capacity == 0u || !sxw_guest_range(process, address, 2u * capacity)) {
        return -1;
    }
    uint8_t *q = (uint8_t *)(uintptr_t)address;
    const uint8_t *s = (const uint8_t *)text;
    uint32_t at = 0;
    while (*s != 0 && at + 1u < capacity) {
        uint32_t c = *s++;
        if (c >= 0x80u) {
            const uint32_t extra = c >= 0xf0u ? 3u : c >= 0xe0u ? 2u : c >= 0xc0u ? 1u : 0u;
            c = extra == 0u ? '?' : c & (0x3fu >> extra);
            for (uint32_t k = 0; k < extra; ++k) {
                if ((*s & 0xc0u) != 0x80u) {
                    c = '?';
                    break;
                }
                c = (c << 6) | (*s++ & 0x3fu);
            }
            if (c > 0xffffu) {
                c = '?';
            }
        }
        q[2u * at] = (uint8_t)c;
        q[2u * at + 1u] = (uint8_t)(c >> 8);
        ++at;
    }
    q[2u * at] = 0;
    q[2u * at + 1u] = 0;
    return (int)at;
}

#endif
