/*
 * wformat.c -- The wide printf engine (wformat.h).
 *
 * Floating point is converted exactly (below), as msvcrt.dll does.
 */
#include "wformat.h"

#include "codepage.h"

typedef unsigned int u32;
typedef unsigned short u16;
typedef unsigned long long u64;

typedef struct sink {
    u16 *out;
    u32 capacity;
    u32 length;
} sink;

static void put(sink *s, u16 c) {
    if (s->length < s->capacity) {
        s->out[s->length] = c;
    }
    ++s->length;
}

static void pad(sink *s, u16 c, int count) {
    for (int i = 0; i < count; ++i) {
        put(s, c);
    }
}

enum { LEFT = 1, PLUS = 2, SPACE = 4, ALT = 8, ZERO = 16 };

/* Writes body (count units, from a wide or narrow source) inside width, with
 * an optional sign or prefix before zero padding. */
static void field(sink *s, const u16 *prefix, int prefix_length, const u16 *body, int count, int width, int flags) {
    int total = prefix_length + count;
    if ((flags & LEFT) == 0 && (flags & ZERO) == 0) {
        pad(s, ' ', width - total);
    }
    for (int i = 0; i < prefix_length; ++i) {
        put(s, prefix[i]);
    }
    if ((flags & LEFT) == 0 && (flags & ZERO) != 0) {
        pad(s, '0', width - total);
    }
    for (int i = 0; i < count; ++i) {
        put(s, body[i]);
    }
    if ((flags & LEFT) != 0) {
        pad(s, ' ', width - total);
    }
}

/* *value / base, returning the remainder, without the C runtime's 64-bit
 * division helpers: two 32-bit divisions. */
static u32 divide_small(u64 *value, u32 base) {
    u32 high = (u32)(*value >> 32);
    u32 low = (u32)*value;
    u32 high_quotient = high / base;
    u32 rest = high % base;
    u32 low_quotient;
    u32 remainder;
    __asm__("divl %4" : "=a"(low_quotient), "=d"(remainder) : "a"(low), "d"(rest), "rm"(base));
    *value = ((u64)high_quotient << 32) | low_quotient;
    return remainder;
}

static int integer_digits(u64 value, u32 base, int upper, u16 *out) {
    u16 tmp[24];
    int n = 0;
    const char *set = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    do {
        tmp[n++] = (u16)set[divide_small(&value, base)];
    } while (value != 0);
    for (int i = 0; i < n; ++i) {
        out[i] = tmp[n - 1 - i];
    }
    return n;
}

static void format_integer(sink *s, u64 magnitude, int negative, u32 base, int upper, int flags, int width,
                           int precision) {
    u16 digits[96];
    int n = 0;
    if (!(precision == 0 && magnitude == 0)) {
        n = integer_digits(magnitude, base, upper, digits);
    }
    u16 body[96];
    int count = 0;
    for (int i = n; i < precision && count < 64; ++i) {
        body[count++] = '0';
    }
    if ((flags & ALT) != 0 && base == 8 && (count + n == 0 || (n > 0 && digits[0] != '0' && count == 0))) {
        body[count++] = '0';
    }
    for (int i = 0; i < n; ++i) {
        body[count++] = digits[i];
    }
    u16 prefix[3];
    int prefix_length = 0;
    if (negative) {
        prefix[prefix_length++] = '-';
    } else if ((flags & PLUS) != 0) {
        prefix[prefix_length++] = '+';
    } else if ((flags & SPACE) != 0) {
        prefix[prefix_length++] = ' ';
    }
    if ((flags & ALT) != 0 && base == 16 && magnitude != 0) {
        prefix[prefix_length++] = '0';
        prefix[prefix_length++] = upper ? 'X' : 'x';
    }
    field(s, prefix, prefix_length, body, count, width, precision >= 0 ? flags & ~ZERO : flags);
}

/* ---- Floating point ------------------------------------------------------------- */
/*
 * A double is m * 2^e exactly, so its decimal expansion is finite. It is
 * written out digit by digit with integers of up to 1,152 bits, then rounded
 * once, half up, where the conversion asks, but never past 17 significant
 * digits: msvcrt.dll keeps 17 and writes zeros after them. So 2.675
 * (2.67499999999999982236431605997495353221893310546875) gives "2.67" at two
 * decimals, 0.125 gives "0.13", and 1e300 with %.0f ends in zeros. A zero
 * shows no sign, negative or not, as msvcrt.dll writes it.
 */

#define SIGNIFICANT_DIGITS 17

#define LIMBS 36           /* 1,152 bits: 2^1024 and 1,074 fraction bits fit */
#define MAX_DIGITS 760     /* 309 integer digits, 330 leading zeros and the rest */
#define MAX_PRECISION 100

typedef struct big {
    u32 limb[LIMBS]; /* least significant first */
} big;

static void big_zero(big *b) {
    for (u32 i = 0; i < LIMBS; ++i) {
        b->limb[i] = 0;
    }
}

static int big_is_zero(const big *b) {
    for (u32 i = 0; i < LIMBS; ++i) {
        if (b->limb[i] != 0u) {
            return 0;
        }
    }
    return 1;
}

/* b = value << shift, value below 2^53. */
static void big_set_shifted(big *b, u64 value, u32 shift) {
    big_zero(b);
    u32 word = shift / 32u;
    u32 bit = shift % 32u;
    u32 parts[3] = {(u32)(value << bit), 0, 0};
    u64 high = bit != 0u ? value >> (32u - bit) : 0u;
    parts[1] = bit != 0u ? (u32)(high) : (u32)(value >> 32);
    parts[2] = bit != 0u ? (u32)(high >> 32) : 0u;
    for (u32 i = 0; i < 3u && word + i < LIMBS; ++i) {
        b->limb[word + i] = parts[i];
    }
}

/* b /= divisor (below 2^32), returning the remainder. */
static u32 big_divide(big *b, u32 divisor) {
    u32 remainder = 0;
    for (int i = LIMBS - 1; i >= 0; --i) {
        u32 quotient;
        __asm__("divl %4" : "=a"(quotient), "=d"(remainder) : "a"(b->limb[i]), "d"(remainder), "rm"(divisor));
        b->limb[i] = quotient;
    }
    return remainder;
}

/* b = b * 10; returns what overflowed past bit `bits` (the next decimal digit
 * of a fraction b / 2^bits) and keeps only the low `bits` bits. */
static u32 big_times_ten_digit(big *b, u32 bits) {
    u32 carry = 0;
    for (u32 i = 0; i < LIMBS; ++i) {
        u64 product = (u64)b->limb[i] * 10u + carry;
        b->limb[i] = (u32)product;
        carry = (u32)(product >> 32);
    }
    u32 word = bits / 32u;
    u32 bit = bits % 32u;
    u32 digit = word < LIMBS ? b->limb[word] >> bit : 0u;
    if (word + 1u < LIMBS && bit != 0u) {
        digit |= b->limb[word + 1u] << (32u - bit);
    }
    if (word < LIMBS) {
        b->limb[word] &= bit != 0u ? (1u << bit) - 1u : 0u;
        for (u32 i = word + 1u; i < LIMBS; ++i) {
            b->limb[i] = 0;
        }
    }
    return digit;
}

typedef struct decimal {
    char digit[MAX_DIGITS]; /* '0'..'9' */
    int count;
    int point; /* digits before the decimal point */
} decimal;

/* The exact digits of value (finite, >= 0): every integer digit, then
 * fraction digits up to `fraction` of them (or until the fraction ends). */
static void exact_digits(double value, int fraction, decimal *d) {
    union {
        double v;
        u64 bits;
    } u;
    u.v = value;
    u32 biased = (u32)(u.bits >> 52) & 0x7ffu;
    u64 mantissa = u.bits & 0xfffffffffffffull;
    int exponent;
    if (biased == 0u) {
        exponent = -1074;
    } else {
        mantissa |= 1ull << 52;
        exponent = (int)biased - 1075;
    }
    big whole;
    big part;
    u32 part_bits = 0;
    big_zero(&part);
    if (exponent >= 0) {
        big_set_shifted(&whole, mantissa, (u32)exponent);
    } else if (exponent > -53) {
        big_set_shifted(&whole, mantissa >> (u32)-exponent, 0);
        big_set_shifted(&part, mantissa & ((1ull << (u32)-exponent) - 1u), 0);
        part_bits = (u32)-exponent;
    } else {
        big_zero(&whole);
        big_set_shifted(&part, mantissa, 0);
        part_bits = (u32)-exponent;
    }
    /* The integer part, nine digits at a time from the bottom. */
    char reversed[320];
    int n = 0;
    while (!big_is_zero(&whole) && n < 312) {
        u32 chunk = big_divide(&whole, 1000000000u);
        int last = big_is_zero(&whole);
        for (int i = 0; i < 9 && (!last || chunk != 0u); ++i) {
            reversed[n++] = (char)('0' + chunk % 10u);
            chunk /= 10u;
        }
    }
    d->count = 0;
    for (int i = n - 1; i >= 0; --i) {
        d->digit[d->count++] = reversed[i];
    }
    d->point = d->count;
    for (int i = 0; i < fraction && d->count < MAX_DIGITS && part_bits != 0u; ++i) {
        d->digit[d->count++] = (char)('0' + big_times_ten_digit(&part, part_bits));
    }
}

/* The digit at position i (0 past the end). */
static char digit_at(const decimal *d, int i) {
    return i >= 0 && i < d->count ? d->digit[i] : '0';
}

/* Keeps the first `keep` digits (padding with zeros), rounding half up on the
 * one after. A carry out of the first digit adds a leading 1 and moves the
 * point. */
static void round_to(decimal *d, int keep) {
    if (keep < 0) {
        d->count = 0;
        return;
    }
    int up = digit_at(d, keep) >= '5';
    while (d->count < keep && d->count < MAX_DIGITS) {
        d->digit[d->count++] = '0';
    }
    d->count = keep;
    for (int i = keep - 1; up && i >= 0; --i) {
        if (d->digit[i] == '9') {
            d->digit[i] = '0';
        } else {
            ++d->digit[i];
            up = 0;
        }
    }
    if (up && d->count < MAX_DIGITS) {
        for (int i = d->count; i > 0; --i) {
            d->digit[i] = d->digit[i - 1];
        }
        d->digit[0] = '1';
        ++d->count;
        ++d->point;
    }
}

/* The index of the first nonzero digit, or -1 for zero. */
static int first_significant(const decimal *d) {
    for (int i = 0; i < d->count; ++i) {
        if (d->digit[i] != '0') {
            return i;
        }
    }
    return -1;
}

/* round_to, but no further than 17 significant digits; zeros after them. */
static void round_limited(decimal *d, int keep) {
    int first = first_significant(d);
    if (first >= 0 && keep > first + SIGNIFICANT_DIGITS) {
        /* A carry moves the point; keep is measured from it. */
        int after_point = keep - d->point;
        round_to(d, first + SIGNIFICANT_DIGITS);
        round_to(d, d->point + after_point);
        return;
    }
    round_to(d, keep);
}

static int put_fixed(u16 *out, int at, const decimal *d, int precision, int alternate) {
    if (d->point <= 0) {
        out[at++] = '0';
    } else {
        for (int i = 0; i < d->point; ++i) {
            out[at++] = (u16)digit_at(d, i);
        }
    }
    if (precision > 0 || alternate) {
        out[at++] = '.';
    }
    for (int i = 0; i < precision; ++i) {
        out[at++] = (u16)digit_at(d, d->point + i);
    }
    return at;
}

/* Mantissa digits from `first`, a point after the first, an exponent of at
 * least three digits, as msvcrt.dll writes it. */
static int put_exponent_form(u16 *out, int at, const decimal *d, int first, int precision, int alternate,
                             int exponent, u16 letter) {
    out[at++] = (u16)digit_at(d, first);
    if (precision > 0 || alternate) {
        out[at++] = '.';
    }
    for (int i = 1; i <= precision; ++i) {
        out[at++] = (u16)digit_at(d, first + i);
    }
    out[at++] = letter;
    out[at++] = exponent < 0 ? '-' : '+';
    int magnitude = exponent < 0 ? -exponent : exponent;
    out[at++] = (u16)('0' + magnitude / 100);
    out[at++] = (u16)('0' + magnitude / 10 % 10);
    out[at++] = (u16)('0' + magnitude % 10);
    return at;
}

/* The buffers are static, too large for the stack of a callback: one thread
 * formats at a time until there are threads (WP3). */
static void format_float(sink *s, double value, u16 conversion, int flags, int width, int precision) {
    static u16 body[MAX_DIGITS + MAX_PRECISION + 16];
    static decimal d;
    int count = 0;
    union {
        double v;
        u64 bits;
    } u;
    u.v = value;
    u32 biased = (u32)(u.bits >> 52) & 0x7ffu;
    int negative = (u.bits >> 63) != 0u && (u.bits << 1) != 0u;
    if ((u.bits >> 63) != 0u) {
        value = -value;
    }
    const int upper = conversion == 'E' || conversion == 'G';
    const int alternate = (flags & ALT) != 0;
    if (precision < 0) {
        precision = 6;
    }
    if (precision > MAX_PRECISION) {
        precision = MAX_PRECISION;
    }
    if (biased == 0x7ffu) {
        const char *text = (u.bits & 0xfffffffffffffull) != 0u ? "1.#QNAN0" : "1.#INF00";
        for (int i = 0; text[i] != 0; ++i) {
            body[count++] = (u16)text[i];
        }
    } else if (conversion == 'f') {
        exact_digits(value, precision + 1 + 330, &d);
        round_limited(&d, d.point + precision);
        count = put_fixed(body, 0, &d, precision, alternate);
    } else {
        /* Enough fraction digits for 330 leading zeros and the precision. */
        exact_digits(value, 330 + precision + 2, &d);
        int first = first_significant(&d);
        int significant = conversion == 'g' || conversion == 'G' ? (precision == 0 ? 1 : precision) : precision + 1;
        int exponent = 0;
        if (first >= 0) {
            round_limited(&d, first + significant);
            first = first_significant(&d);
            exponent = d.point - first - 1;
        } else {
            first = 0;
        }
        u16 letter = upper ? 'E' : 'e';
        if (conversion == 'e' || conversion == 'E') {
            count = put_exponent_form(body, 0, &d, first, precision, alternate, exponent, letter);
        } else {
            int strip = !alternate;
            if (exponent < -4 || exponent >= significant) {
                count = put_exponent_form(body, 0, &d, first, significant - 1, alternate, exponent, letter);
                if (strip) {
                    /* Trailing zeros of the mantissa go, the exponent stays. */
                    int e = count - 5;
                    int m = e;
                    while (m > 0 && body[m - 1] == '0') {
                        --m;
                    }
                    if (m > 0 && body[m - 1] == '.') {
                        --m;
                    }
                    for (int i = 0; i < 5; ++i) {
                        body[m + i] = body[e + i];
                    }
                    count = m + 5;
                }
            } else {
                count = put_fixed(body, 0, &d, significant - 1 - exponent, alternate);
                if (strip) {
                    int has_point = 0;
                    for (int i = 0; i < count; ++i) {
                        has_point |= body[i] == '.';
                    }
                    while (has_point && count > 0 && body[count - 1] == '0') {
                        --count;
                    }
                    if (count > 0 && body[count - 1] == '.') {
                        --count;
                    }
                }
            }
        }
    }
    u16 prefix[1];
    int prefix_length = 0;
    if (negative) {
        prefix[prefix_length++] = '-';
    } else if ((flags & PLUS) != 0) {
        prefix[prefix_length++] = '+';
    } else if ((flags & SPACE) != 0) {
        prefix[prefix_length++] = ' ';
    }
    field(s, prefix, prefix_length, body, count, width, flags);
}

/* ---- The engine --------------------------------------------------------------------- */

u32 sxw_wformat(u16 *out, u32 capacity, const u16 *format, va_list args, u32 options) {
    sink s = {out, capacity, 0};
    for (const u16 *f = format; *f != 0; ++f) {
        if (*f != '%') {
            put(&s, *f);
            continue;
        }
        ++f;
        int flags = 0;
        for (;; ++f) {
            if (*f == '-') flags |= LEFT;
            else if (*f == '+') flags |= PLUS;
            else if (*f == ' ') flags |= SPACE;
            else if (*f == '#') flags |= ALT;
            else if (*f == '0') flags |= ZERO;
            else break;
        }
        int width = 0;
        if (*f == '*') {
            width = va_arg(args, int);
            if (width < 0) {
                flags |= LEFT;
                width = -width;
            }
            ++f;
        } else {
            while (*f >= '0' && *f <= '9') {
                width = width * 10 + (*f++ - '0');
            }
        }
        int precision = -1;
        if (*f == '.') {
            ++f;
            precision = 0;
            if (*f == '*') {
                precision = va_arg(args, int);
                ++f;
            } else {
                while (*f >= '0' && *f <= '9') {
                    precision = precision * 10 + (*f++ - '0');
                }
            }
        }
        int size = 0; /* -1 short, 0 int, 1 long, 2 64-bit, 3 long double */
        int narrow = -1; /* for s and c: 1 narrow, 0 wide, -1 by the conversion's case */
        if (*f == 'h') {
            size = -1;
            narrow = 1;
            ++f;
        } else if (*f == 'l') {
            ++f;
            narrow = 0;
            size = 1;
            /* msvcrt.dll of Windows XP has no "ll": the second l is skipped
             * and the argument is 32 bits, as there. I64 is the 64-bit one. */
            if (*f == 'l') {
                ++f;
            }
        } else if (*f == 'w') {
            narrow = 0;
            ++f;
        } else if (*f == 'L') {
            size = 3;
            ++f;
        } else if (f[0] == 'I' && f[1] == '6' && f[2] == '4') {
            size = 2;
            f += 3;
        } else if (f[0] == 'I' && f[1] == '3' && f[2] == '2') {
            f += 3;
        }
        u16 c = *f;
        if (c == 0) {
            break;
        }
        switch (c) {
        case 'd':
        case 'i': {
            long long v = size == 2 ? va_arg(args, long long) : (long long)va_arg(args, int);
            if (size == -1) {
                v = (short)v;
            }
            format_integer(&s, v < 0 ? 0ull - (u64)v : (u64)v, v < 0, 10, 0, flags, width, precision);
            break;
        }
        case 'u':
        case 'o':
        case 'x':
        case 'X': {
            u64 v = size == 2 ? va_arg(args, u64) : (u64)va_arg(args, u32);
            if (size == -1) {
                v = (u16)v;
            }
            u32 base = c == 'u' ? 10u : c == 'o' ? 8u : 16u;
            format_integer(&s, v, 0, base, c == 'X', flags & ~(PLUS | SPACE), width, precision);
            break;
        }
        case 'p':
            format_integer(&s, (u64)va_arg(args, u32), 0, 16, 1, 0, width, 8);
            break;
        case 'c':
        case 'C': {
            u32 v = va_arg(args, u32);
            int is_narrow = narrow >= 0 ? narrow : c == 'C';
            u16 ch = is_narrow ? sxw_ansi_to_wide((unsigned char)v) : (u16)v;
            field(&s, 0, 0, &ch, 1, width, flags & LEFT);
            break;
        }
        case 's':
        case 'S': {
            const void *p = va_arg(args, const void *);
            int is_narrow = narrow >= 0 ? narrow : c == 'S';
            static const u16 null_text[] = {'(', 'n', 'u', 'l', 'l', ')', 0};
            if (p == 0) {
                p = null_text;
                is_narrow = 0;
            }
            int n = 0;
            if (is_narrow) {
                const unsigned char *t = p;
                while (t[n] != 0 && (precision < 0 || n < precision)) {
                    ++n;
                }
            } else {
                const u16 *t = p;
                while (t[n] != 0 && (precision < 0 || n < precision)) {
                    ++n;
                }
            }
            int padding = width > n ? width - n : 0;
            if ((flags & LEFT) == 0) {
                pad(&s, ' ', padding);
            }
            for (int i = 0; i < n; ++i) {
                put(&s, is_narrow ? sxw_ansi_to_wide(((const unsigned char *)p)[i]) : ((const u16 *)p)[i]);
            }
            if ((flags & LEFT) != 0) {
                pad(&s, ' ', padding);
            }
            break;
        }
        case 'e':
        case 'E':
        case 'f':
        case 'g':
        case 'G':
            if ((options & SXW_FORMAT_FLOATS) == 0u) {
                put(&s, c);
                break;
            }
            format_float(&s, va_arg(args, double), c, flags, width, precision);
            break;
        case 'n':
            (void)va_arg(args, void *);
            break;
        default:
            put(&s, c);
            break;
        }
    }
    return s.length;
}
