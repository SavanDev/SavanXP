/*
 * memory.c -- msvcrt's heap functions and the mem family, over ntdll's heap
 * (the same one kernel32's LocalAlloc uses, as msvcrt.dll's heap is the
 * process heap on Windows XP).
 */
#include <windows.h>

typedef unsigned int u32;
typedef unsigned int size_t;

__declspec(dllimport) void *WINAPI SxwHeapAllocate(u32 size, u32 zero);
__declspec(dllimport) int WINAPI SxwHeapFree(void *pointer);
__declspec(dllimport) u32 WINAPI SxwHeapSize(const void *pointer);
__declspec(dllimport) int WINAPI SxwHeapResize(void *pointer, u32 size);

void *__cdecl memset(void *destination, int value, size_t count) {
    unsigned char *d = destination;
    for (size_t i = 0; i < count; ++i) {
        d[i] = (unsigned char)value;
    }
    return destination;
}

void *__cdecl memcpy(void *destination, const void *source, size_t count) {
    unsigned char *d = destination;
    const unsigned char *s = source;
    for (size_t i = 0; i < count; ++i) {
        d[i] = s[i];
    }
    return destination;
}

/* memcpy in msvcrt.dll copies overlapping blocks correctly too; so does
 * memmove, backwards when the destination is above the source. */
void *__cdecl memmove(void *destination, const void *source, size_t count) {
    unsigned char *d = destination;
    const unsigned char *s = source;
    if (d > s && d < s + count) {
        for (size_t i = count; i > 0; --i) {
            d[i - 1] = s[i - 1];
        }
    } else {
        for (size_t i = 0; i < count; ++i) {
            d[i] = s[i];
        }
    }
    return destination;
}

int __cdecl memcmp(const void *a, const void *b, size_t count) {
    const unsigned char *x = a;
    const unsigned char *y = b;
    for (size_t i = 0; i < count; ++i) {
        if (x[i] != y[i]) {
            return x[i] < y[i] ? -1 : 1;
        }
    }
    return 0;
}

void *__cdecl malloc(size_t size) {
    return SxwHeapAllocate(size, 0);
}

void *__cdecl calloc(size_t count, size_t size) {
    if (size != 0u && count > 0xffffffffu / size) {
        return 0;
    }
    return SxwHeapAllocate(count * size, 1);
}

void __cdecl free(void *block) {
    if (block != 0) {
        SxwHeapFree(block);
    }
}

size_t __cdecl _msize(void *block) {
    return SxwHeapSize(block);
}

/* Grows or shrinks a block without moving it: only within the space it has. */
void *__cdecl _expand(void *block, size_t size) {
    return block != 0 && SxwHeapResize(block, size) ? block : 0;
}

void *__cdecl realloc(void *block, size_t size) {
    if (block == 0) {
        return malloc(size);
    }
    if (size == 0u) {
        free(block);
        return 0;
    }
    size_t old = SxwHeapSize(block);
    if (SxwHeapResize(block, size)) {
        return block;
    }
    void *moved = malloc(size);
    if (moved == 0) {
        return 0;
    }
    memcpy(moved, block, old);
    free(block);
    return moved;
}
