/*
 * wprintf.c -- msvcrt's wide printf family over the shared engine
 * (common/wformat.h).
 */
#include <stdarg.h>
#include <windows.h>

#include "wformat.h"

typedef unsigned int u32;
typedef unsigned int size_t;

/* At most count units; a terminator only when there is room for it. -1 when
 * the result did not fit in count, as msvcrt.dll answers. */
int __cdecl _vsnwprintf(WCHAR *buffer, size_t count, const WCHAR *format, va_list args) {
    u32 length = sxw_wformat(buffer, count, format, args, SXW_FORMAT_FLOATS);
    if (length < count) {
        buffer[length] = 0;
    }
    return length <= count ? (int)length : -1;
}

int __cdecl _snwprintf(WCHAR *buffer, size_t count, const WCHAR *format, ...) {
    va_list args;
    va_start(args, format);
    int n = _vsnwprintf(buffer, count, format, args);
    va_end(args);
    return n;
}

/* The old forms with no count: the buffer is assumed large enough. */
int __cdecl vswprintf(WCHAR *buffer, const WCHAR *format, va_list args) {
    u32 length = sxw_wformat(buffer, 0x7fffffffu, format, args, SXW_FORMAT_FLOATS);
    buffer[length] = 0;
    return (int)length;
}

int __cdecl swprintf(WCHAR *buffer, const WCHAR *format, ...) {
    va_list args;
    va_start(args, format);
    int n = vswprintf(buffer, format, args);
    va_end(args);
    return n;
}
