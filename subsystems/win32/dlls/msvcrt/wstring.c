/*
 * wstring.c -- msvcrt's wide-string functions, number conversions and
 * character classes, and the two narrow-string ones the profile imports.
 *
 * The C locale only, as msvcrt.dll in it: case conversion and the
 * case-insensitive comparisons fold ASCII letters alone (L"Ñ" and L"ñ"
 * differ), wcscoll compares by code unit.
 */
#include <windows.h>

typedef unsigned int u32;
typedef unsigned int size_t;
typedef unsigned short wchar_t16;

void *__cdecl malloc(size_t size);

/* ---- Lengths, copies, comparisons --------------------------------------------- */

size_t __cdecl wcslen(const WCHAR *s) {
    size_t n = 0;
    while (s[n] != 0) {
        ++n;
    }
    return n;
}

WCHAR *__cdecl wcscpy(WCHAR *destination, const WCHAR *source) {
    size_t i = 0;
    do {
        destination[i] = source[i];
    } while (source[i++] != 0);
    return destination;
}

/* Copies up to count units and pads with zeros; no terminator when the
 * source is at least count long, as the C library specifies. */
WCHAR *__cdecl wcsncpy(WCHAR *destination, const WCHAR *source, size_t count) {
    size_t i = 0;
    for (; i < count && source[i] != 0; ++i) {
        destination[i] = source[i];
    }
    for (; i < count; ++i) {
        destination[i] = 0;
    }
    return destination;
}

int __cdecl wcscmp(const WCHAR *a, const WCHAR *b) {
    for (size_t i = 0;; ++i) {
        if (a[i] != b[i]) {
            return a[i] < b[i] ? -1 : 1;
        }
        if (a[i] == 0) {
            return 0;
        }
    }
}

int __cdecl wcsncmp(const WCHAR *a, const WCHAR *b, size_t count) {
    for (size_t i = 0; i < count; ++i) {
        if (a[i] != b[i]) {
            return a[i] < b[i] ? -1 : 1;
        }
        if (a[i] == 0) {
            return 0;
        }
    }
    return 0;
}

int __cdecl wcscoll(const WCHAR *a, const WCHAR *b) {
    return wcscmp(a, b);
}

static u32 lower(u32 c) {
    return c >= 'A' && c <= 'Z' ? c + 32u : c;
}

static u32 upper(u32 c) {
    return c >= 'a' && c <= 'z' ? c - 32u : c;
}

int __cdecl _wcsnicmp(const WCHAR *a, const WCHAR *b, size_t count) {
    for (size_t i = 0; i < count; ++i) {
        u32 x = lower(a[i]);
        u32 y = lower(b[i]);
        if (x != y) {
            return x < y ? -1 : 1;
        }
        if (x == 0) {
            return 0;
        }
    }
    return 0;
}

int __cdecl _wcsicmp(const WCHAR *a, const WCHAR *b) {
    return _wcsnicmp(a, b, 0xffffffffu);
}

WCHAR *__cdecl _wcslwr(WCHAR *s) {
    for (size_t i = 0; s[i] != 0; ++i) {
        s[i] = (WCHAR)lower(s[i]);
    }
    return s;
}

WCHAR *__cdecl _wcsupr(WCHAR *s) {
    for (size_t i = 0; s[i] != 0; ++i) {
        s[i] = (WCHAR)upper(s[i]);
    }
    return s;
}

WCHAR *__cdecl _wcsrev(WCHAR *s) {
    size_t n = wcslen(s);
    for (size_t i = 0; i < n / 2u; ++i) {
        WCHAR t = s[i];
        s[i] = s[n - 1u - i];
        s[n - 1u - i] = t;
    }
    return s;
}

WCHAR *__cdecl _wcsdup(const WCHAR *s) {
    if (s == 0) {
        return 0;
    }
    size_t n = wcslen(s) + 1u;
    WCHAR *copy = malloc(n * sizeof(WCHAR));
    if (copy != 0) {
        for (size_t i = 0; i < n; ++i) {
            copy[i] = s[i];
        }
    }
    return copy;
}

/* ---- Searches ---------------------------------------------------------------------- */

WCHAR *__cdecl wcschr(const WCHAR *s, WCHAR c) {
    for (;; ++s) {
        if (*s == c) {
            return (WCHAR *)s;
        }
        if (*s == 0) {
            return 0;
        }
    }
}

WCHAR *__cdecl wcsrchr(const WCHAR *s, WCHAR c) {
    const WCHAR *found = 0;
    for (;; ++s) {
        if (*s == c) {
            found = s;
        }
        if (*s == 0) {
            return (WCHAR *)found;
        }
    }
}

WCHAR *__cdecl wcsstr(const WCHAR *haystack, const WCHAR *needle) {
    if (needle[0] == 0) {
        return (WCHAR *)haystack;
    }
    for (; *haystack != 0; ++haystack) {
        size_t i = 0;
        while (needle[i] != 0 && haystack[i] == needle[i]) {
            ++i;
        }
        if (needle[i] == 0) {
            return (WCHAR *)haystack;
        }
    }
    return 0;
}

static int in_set(WCHAR c, const WCHAR *set) {
    for (; *set != 0; ++set) {
        if (*set == c) {
            return 1;
        }
    }
    return 0;
}

size_t __cdecl wcsspn(const WCHAR *s, const WCHAR *set) {
    size_t n = 0;
    while (s[n] != 0 && in_set(s[n], set)) {
        ++n;
    }
    return n;
}

size_t __cdecl wcscspn(const WCHAR *s, const WCHAR *set) {
    size_t n = 0;
    while (s[n] != 0 && !in_set(s[n], set)) {
        ++n;
    }
    return n;
}

WCHAR *__cdecl wcspbrk(const WCHAR *s, const WCHAR *set) {
    for (; *s != 0; ++s) {
        if (in_set(*s, set)) {
            return (WCHAR *)s;
        }
    }
    return 0;
}

/* ---- Character classes ---------------------------------------------------------- */

int __cdecl iswdigit(unsigned short c) {
    return c >= '0' && c <= '9' ? 4 : 0; /* _DIGIT */
}

int __cdecl iswspace(unsigned short c) {
    return (c >= 9 && c <= 13) || c == ' ' || c == 0xa0u || (c >= 0x2000u && c <= 0x200bu) || c == 0x3000u ? 8 : 0;
}

/* ---- Numbers ----------------------------------------------------------------------- */

static int digit_value(u32 c) {
    if (c >= '0' && c <= '9') {
        return (int)(c - '0');
    }
    c = lower(c);
    if (c >= 'a' && c <= 'z') {
        return (int)(c - 'a') + 10;
    }
    return 99;
}

/* The shared parser of wcstol and wcstoul: sign, an optional 0x or 0 prefix
 * when base is 0 or 16, digits; *overflow when the magnitude passes limit. */
static unsigned long parse_unsigned(const WCHAR *s, WCHAR **end, int base, int *negative, unsigned long limit,
                                    int *overflow) {
    const WCHAR *p = s;
    while (iswspace(*p)) {
        ++p;
    }
    *negative = 0;
    if (*p == '+' || *p == '-') {
        *negative = *p == '-';
        ++p;
    }
    if ((base == 0 || base == 16) && p[0] == '0' && (p[1] == 'x' || p[1] == 'X') && digit_value(p[2]) < 16) {
        p += 2;
        base = 16;
    } else if (base == 0) {
        base = p[0] == '0' ? 8 : 10;
    }
    unsigned long value = 0;
    const WCHAR *first = p;
    *overflow = 0;
    if (base >= 2 && base <= 36) {
        for (; digit_value(*p) < base; ++p) {
            unsigned long d = (unsigned long)digit_value(*p);
            if (value > (limit - d) / (unsigned long)base) {
                *overflow = 1;
            } else {
                value = value * (unsigned long)base + d;
            }
        }
    }
    if (end != 0) {
        *end = (WCHAR *)(p == first ? s : p);
    }
    return value;
}

long __cdecl wcstol(const WCHAR *s, WCHAR **end, int base) {
    int negative, overflow;
    unsigned long v = parse_unsigned(s, end, base, &negative, 0x80000000ul, &overflow);
    if (!negative && v > 0x7ffffffful) {
        overflow = 1;
    }
    if (overflow) {
        return negative ? (long)0x80000000ul : 0x7fffffffl;
    }
    return negative ? -(long)v : (long)v;
}

unsigned long __cdecl wcstoul(const WCHAR *s, WCHAR **end, int base) {
    int negative, overflow;
    unsigned long v = parse_unsigned(s, end, base, &negative, 0xfffffffful, &overflow);
    if (overflow) {
        return 0xfffffffful;
    }
    return negative ? 0ul - v : v;
}

long __cdecl _wtol(const WCHAR *s) {
    return wcstol(s, 0, 10);
}

int __cdecl _wtoi(const WCHAR *s) {
    return (int)wcstol(s, 0, 10);
}

/* Decimal digits, a fraction and an exponent. Accumulated in double, so the
 * last digit of a 17-digit number may differ from msvcrt.dll's. */
double __cdecl wcstod(const WCHAR *s, WCHAR **end) {
    const WCHAR *p = s;
    while (iswspace(*p)) {
        ++p;
    }
    int negative = 0;
    if (*p == '+' || *p == '-') {
        negative = *p == '-';
        ++p;
    }
    double value = 0.0;
    int digits = 0;
    for (; *p >= '0' && *p <= '9'; ++p, ++digits) {
        value = value * 10.0 + (double)(*p - '0');
    }
    if (*p == '.') {
        ++p;
        double scale = 0.1;
        for (; *p >= '0' && *p <= '9'; ++p, ++digits) {
            value += (double)(*p - '0') * scale;
            scale *= 0.1;
        }
    }
    if (digits == 0) {
        if (end != 0) {
            *end = (WCHAR *)s;
        }
        return 0.0;
    }
    if (*p == 'e' || *p == 'E') {
        const WCHAR *mark = p++;
        int exponent_negative = 0;
        if (*p == '+' || *p == '-') {
            exponent_negative = *p == '-';
            ++p;
        }
        if (*p >= '0' && *p <= '9') {
            int exponent = 0;
            for (; *p >= '0' && *p <= '9'; ++p) {
                exponent = exponent < 1000 ? exponent * 10 + (*p - '0') : exponent;
            }
            for (int i = 0; i < exponent; ++i) {
                value = exponent_negative ? value / 10.0 : value * 10.0;
            }
        } else {
            p = mark;
        }
    }
    if (end != 0) {
        *end = (WCHAR *)p;
    }
    return negative ? -value : value;
}

static WCHAR *unsigned_to_wide(unsigned long value, WCHAR *out, int radix, int negative) {
    WCHAR digits[34];
    int n = 0;
    if (radix < 2 || radix > 36) {
        out[0] = 0;
        return out;
    }
    do {
        u32 d = value % (unsigned long)radix;
        digits[n++] = (WCHAR)(d < 10 ? '0' + d : 'a' + d - 10);
        value /= (unsigned long)radix;
    } while (value != 0);
    int at = 0;
    if (negative) {
        out[at++] = '-';
    }
    while (n > 0) {
        out[at++] = digits[--n];
    }
    out[at] = 0;
    return out;
}

/* Negative only in base 10, as msvcrt.dll does; other bases print the bits. */
WCHAR *__cdecl _ltow(long value, WCHAR *out, int radix) {
    int negative = radix == 10 && value < 0;
    return unsigned_to_wide(negative ? 0ul - (unsigned long)value : (unsigned long)value, out, radix, negative);
}

WCHAR *__cdecl _itow(int value, WCHAR *out, int radix) {
    return _ltow(value, out, radix);
}

WCHAR *__cdecl _ultow(unsigned long value, WCHAR *out, int radix) {
    return unsigned_to_wide(value, out, radix, 0);
}

/* ---- Narrow strings ---------------------------------------------------------------- */

int __cdecl strcmp(const char *a, const char *b) {
    for (size_t i = 0;; ++i) {
        if (a[i] != b[i]) {
            return (unsigned char)a[i] < (unsigned char)b[i] ? -1 : 1;
        }
        if (a[i] == 0) {
            return 0;
        }
    }
}

char *__cdecl strcpy(char *destination, const char *source) {
    size_t i = 0;
    do {
        destination[i] = source[i];
    } while (source[i++] != 0);
    return destination;
}
