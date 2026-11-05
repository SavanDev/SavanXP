/*
 * The <string.h> FatFs includes, for the freestanding kernel build only.
 *
 * The compiler ships no string.h for a freestanding target. The mem*
 * functions FatFs calls already exist in the kernel with C linkage
 * (arch/x86_64/memory.cpp, kernel/runtime.cpp). strchr is here, inline: ff.c
 * uses it to reject characters FAT does not allow in a name, and the
 * kernel's own string functions have C++ linkage (kernel/string.hpp).
 */
#ifndef SAVANXP_FATFS_STRING_H
#define SAVANXP_FATFS_STRING_H

#include <stddef.h>

void* memcpy(void* destination, const void* source, size_t count);
void* memset(void* destination, int value, size_t count);
int memcmp(const void* left, const void* right, size_t count);
void* memmove(void* destination, const void* source, size_t count);

static inline char* strchr(const char* text, int value) {
    const char wanted = (char)value;
    for (;; ++text) {
        if (*text == wanted) {
            return (char*)text;
        }
        if (*text == 0) {
            return NULL;
        }
    }
}

#endif
