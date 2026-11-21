/*
 * wformat.h -- The wide printf engine of msvcrt's _vsnwprintf family and
 * user32's wvsprintfW. Compiled into each DLL that formats; not exported.
 */
#ifndef SXW_DLLS_WFORMAT_H
#define SXW_DLLS_WFORMAT_H

#include <stdarg.h>

/* Floating point conversions (%e, %f, %g): msvcrt's, not wsprintf's. */
#define SXW_FORMAT_FLOATS 1u

/* Formats into out[capacity] without a terminator and returns the length of
 * the whole result, which may be more than capacity (the rest is dropped).
 * The conversions are msvcrt's wide ones: %s and %ls a wide string, %S and
 * %hs a narrow one (Windows-1252), %c and %C likewise for a character, d i u
 * o x X p, the flags - + space # 0, width and precision with *, and the
 * lengths h, l, I64 and L; "ll" is l, as in msvcrt.dll of Windows XP. %n is
 * refused (writes nothing). */
unsigned int sxw_wformat(unsigned short *out, unsigned int capacity, const unsigned short *format,
                         va_list args, unsigned int flags);

#endif
