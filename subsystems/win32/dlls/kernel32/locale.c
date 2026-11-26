/*
 * locale.c -- kernel32's locale: one, English (United States), LCID 0x0409,
 * the language the rest of SavanXP speaks.
 *
 * SavanXP has no regional setting yet, so every locale query answers with
 * the locale of the English Windows XP the fixtures are checked on, and its
 * values are that Windows' own (the table below is what GetLocaleInfoW
 * answered there, locale.exe's output). Dates and times are formatted from the same
 * pictures with the same rules; CompareStringW sorts as that locale does,
 * by letters, then accents, then case, with the hyphen and the apostrophe
 * weighed last in a word sort. See docs/userland/WIN32.md, "The locale".
 *
 * win.ini is not there: GetProfile* answer every name with its default.
 */
#define SXW_BUILDING_KERNEL32
#include <windows.h>

#include "sxw_abi.h"

typedef unsigned int u32;
typedef unsigned char u8;

__declspec(dllimport) u32 WINAPI SxwService(u32 service, const u32 *arguments);

#define LOCALE_ENGLISH_US 0x0409u
#define LOCALE_SYSTEM_DEFAULT 0x0800u
#define LOCALE_USER_DEFAULT 0x0400u
#define LOCALE_NOUSEROVERRIDE 0x80000000u
#define LOCALE_USE_CP_ACP 0x40000000u
#define LOCALE_RETURN_NUMBER 0x20000000u
#define ERROR_INVALID_FLAGS 1004u

#define DATE_SHORTDATE 0x1u
#define DATE_LONGDATE 0x2u
#define DATE_YEARMONTH 0x8u
#define TIME_NOMINUTESORSECONDS 0x1u
#define TIME_NOSECONDS 0x2u
#define TIME_NOTIMEMARKER 0x4u
#define TIME_FORCE24HOURFORMAT 0x8u

#define NORM_IGNORECASE 0x1u
#define NORM_IGNORENONSPACE 0x2u
#define NORM_IGNORESYMBOLS 0x4u
#define NORM_IGNOREKANATYPE 0x10000u
#define NORM_IGNOREWIDTH 0x20000u
#define SORT_STRINGSORT 0x1000u

#define CSTR_LESS_THAN 1
#define CSTR_EQUAL 2
#define CSTR_GREATER_THAN 3

LCID WINAPI GetUserDefaultLCID(void) {
    return LOCALE_ENGLISH_US;
}

LCID WINAPI GetSystemDefaultLCID(void) {
    return LOCALE_ENGLISH_US;
}

LCID WINAPI GetThreadLocale(void) {
    return LOCALE_ENGLISH_US;
}

static int known_locale(LCID locale) {
    return locale == LOCALE_ENGLISH_US || locale == LOCALE_USER_DEFAULT || locale == LOCALE_SYSTEM_DEFAULT ||
           locale == 0;
}

static u32 wide_length(const WCHAR *s) {
    u32 n = 0;
    while (s[n] != 0) {
        ++n;
    }
    return n;
}

/* ---- GetLocaleInfoW ------------------------------------------------------------ */

#define LOCALE_SSHORTDATE 0x001fu
#define LOCALE_SLONGDATE 0x0020u
#define LOCALE_S1159 0x0028u
#define LOCALE_S2359 0x0029u
#define LOCALE_SDAYNAME1 0x002au
#define LOCALE_SABBREVDAYNAME1 0x0031u
#define LOCALE_SMONTHNAME1 0x0038u
#define LOCALE_SABBREVMONTHNAME1 0x0044u
#define LOCALE_STIMEFORMAT 0x1003u
#define LOCALE_SYEARMONTH 0x1006u

static const struct {
    u32 type;
    const WCHAR *value;
} k_values[] = {
    {0x0001u, L"0409"},
    {0x0002u, L"English (United States)"},
    {0x0003u, L"ENU"},
    {0x0004u, L"English"},
    {0x0005u, L"1"},
    {0x0006u, L"United States"},
    {0x0007u, L"USA"},
    {0x0008u, L"United States"},
    {0x0009u, L"0409"},
    {0x000au, L"1"},
    {0x000bu, L"437"},
    {0x000cu, L","},
    {0x000du, L"1"},
    {0x000eu, L"."},
    {0x000fu, L","},
    {0x0010u, L"3;0"},
    {0x0011u, L"2"},
    {0x0012u, L"1"},
    {0x0013u, L"0123456789"},
    {0x0014u, L"$"},
    {0x0015u, L"USD"},
    {0x0016u, L"."},
    {0x0017u, L","},
    {0x0018u, L"3;0"},
    {0x0019u, L"2"},
    {0x001au, L"2"},
    {0x001bu, L"0"},
    {0x001cu, L"0"},
    {0x001du, L"/"},
    {0x001eu, L":"},
    {0x001fu, L"M/d/yyyy"},
    {0x0020u, L"dddd, MMMM dd, yyyy"},
    {0x0021u, L"0"},
    {0x0022u, L"0"},
    {0x0023u, L"0"},
    {0x0024u, L"1"},
    {0x0025u, L"0"},
    {0x0026u, L"0"},
    {0x0027u, L"0"},
    {0x0028u, L"AM"},
    {0x0029u, L"PM"},
    {0x002au, L"Monday"},
    {0x002bu, L"Tuesday"},
    {0x002cu, L"Wednesday"},
    {0x002du, L"Thursday"},
    {0x002eu, L"Friday"},
    {0x002fu, L"Saturday"},
    {0x0030u, L"Sunday"},
    {0x0031u, L"Mon"},
    {0x0032u, L"Tue"},
    {0x0033u, L"Wed"},
    {0x0034u, L"Thu"},
    {0x0035u, L"Fri"},
    {0x0036u, L"Sat"},
    {0x0037u, L"Sun"},
    {0x0038u, L"January"},
    {0x0039u, L"February"},
    {0x003au, L"March"},
    {0x003bu, L"April"},
    {0x003cu, L"May"},
    {0x003du, L"June"},
    {0x003eu, L"July"},
    {0x003fu, L"August"},
    {0x0040u, L"September"},
    {0x0041u, L"October"},
    {0x0042u, L"November"},
    {0x0043u, L"December"},
    {0x0044u, L"Jan"},
    {0x0045u, L"Feb"},
    {0x0046u, L"Mar"},
    {0x0047u, L"Apr"},
    {0x0048u, L"May"},
    {0x0049u, L"Jun"},
    {0x004au, L"Jul"},
    {0x004bu, L"Aug"},
    {0x004cu, L"Sep"},
    {0x004du, L"Oct"},
    {0x004eu, L"Nov"},
    {0x004fu, L"Dec"},
    {0x0050u, L""},
    {0x0051u, L"-"},
    {0x0052u, L"3"},
    {0x0053u, L"0"},
    {0x0054u, L"1"},
    {0x0055u, L"0"},
    {0x0056u, L"1"},
    {0x0057u, L"0"},
    {0x0059u, L"en"},
    {0x005au, L"US"},
    {0x005bu, L"244"},
    {0x1001u, L"English"},
    {0x1002u, L"United States"},
    {0x1003u, L"h:mm:ss tt"},
    {0x1004u, L"1252"},
    {0x1005u, L"0"},
    {0x1006u, L"MMMM, yyyy"},
    {0x1007u, L"US Dollar"},
    {0x1008u, L"US Dollar"},
    {0x1009u, L"1"},
    {0x100au, L"1"},
    {0x100bu, L"0"},
    {0x100cu, L"6"},
    {0x100du, L"0"},
    {0x100eu, L""},
    {0x100fu, L""},
    {0x1010u, L"1"},
    {0x1011u, L"10000"},
    {0x1012u, L"037"},
    {0x1013u, L"Default"},
    {0x1014u, L"1"},
};

static const WCHAR *locale_value(u32 type) {
    for (u32 i = 0; i < sizeof k_values / sizeof k_values[0]; ++i) {
        if (k_values[i].type == type) {
            return k_values[i].value;
        }
    }
    return 0;
}

int WINAPI GetLocaleInfoW(LCID Locale, DWORD LCType, LPWSTR lpLCData, int cchData) {
    if (!known_locale(Locale)) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return 0;
    }
    const u32 type = LCType & ~(LOCALE_NOUSEROVERRIDE | LOCALE_USE_CP_ACP | LOCALE_RETURN_NUMBER);
    const WCHAR *value = locale_value(type);
    if (value == 0 || cchData < 0) {
        SetLastError(ERROR_INVALID_FLAGS);
        return 0;
    }
    if ((LCType & LOCALE_RETURN_NUMBER) != 0u) {
        /* A number as a DWORD in two WCHARs: hex for the languages. */
        const u32 base = type == 0x0001u || type == 0x0009u ? 16u : 10u;
        u32 number = 0;
        if (value[0] == 0) {
            SetLastError(ERROR_INVALID_FLAGS);
            return 0;
        }
        for (const WCHAR *c = value; *c != 0; ++c) {
            const u32 digit = *c >= '0' && *c <= '9' ? (u32)(*c - '0') : *c >= 'a' && *c <= 'f' ? (u32)(*c - 'a' + 10) : 99u;
            if (digit >= base) {
                SetLastError(ERROR_INVALID_FLAGS);
                return 0;
            }
            number = number * base + digit;
        }
        if (cchData == 0) {
            return 2;
        }
        if (cchData < 2 || lpLCData == NULL) {
            SetLastError(ERROR_INSUFFICIENT_BUFFER);
            return 0;
        }
        lpLCData[0] = (WCHAR)number;
        lpLCData[1] = (WCHAR)(number >> 16);
        return 2;
    }
    const int length = (int)wide_length(value);
    if (cchData == 0) {
        return length + 1;
    }
    if (cchData <= length || lpLCData == NULL) {
        SetLastError(ERROR_INSUFFICIENT_BUFFER);
        return 0;
    }
    for (int i = 0; i <= length; ++i) {
        lpLCData[i] = value[i];
    }
    return length + 1;
}

/* ---- Dates and times ------------------------------------------------------------- */

typedef struct text_out {
    WCHAR *buffer; /* NULL to only count */
    int capacity;
    int length;    /* without the terminator */
} text_out;

static void put_char(text_out *out, WCHAR c) {
    if (out->buffer != NULL && out->length < out->capacity) {
        out->buffer[out->length] = c;
    }
    ++out->length;
}

static void put_text(text_out *out, const WCHAR *text) {
    while (*text != 0) {
        put_char(out, *text++);
    }
}

static void put_number(text_out *out, u32 value, u32 digits) {
    WCHAR buffer[12];
    u32 n = 0;
    do {
        buffer[n++] = (WCHAR)('0' + value % 10u);
        value /= 10u;
    } while (value != 0u);
    while (n < digits) {
        buffer[n++] = '0';
    }
    while (n > 0u) {
        put_char(out, buffer[--n]);
    }
}

/* The run of one letter at *at, its length, *at past it. */
static u32 run_of(const WCHAR **at) {
    const WCHAR c = **at;
    u32 n = 0;
    while (**at == c) {
        ++*at;
        ++n;
    }
    return n;
}

/* A quoted literal at *at (the opening quote), to out or dropped; '' in it
 * is one quote, and an unclosed one runs to the end. */
static void quoted(const WCHAR **at, text_out *out) {
    ++*at;
    for (;;) {
        if (**at == 0) {
            return;
        }
        if (**at == '\'') {
            if ((*at)[1] == '\'') {
                if (out != 0) {
                    put_char(out, '\'');
                }
                *at += 2;
                continue;
            }
            ++*at;
            return;
        }
        if (out != 0) {
            put_char(out, **at);
        }
        ++*at;
    }
}

static int leap(u32 year) {
    return (year % 4u == 0u && year % 100u != 0u) || year % 400u == 0u;
}

static u32 days_in_month(u32 year, u32 month) {
    static const u8 days[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    return month == 2u && leap(year) ? 29u : days[month - 1u];
}

/* 0 Sunday to 6 Saturday, from the date itself (Zeller's congruence). */
static u32 day_of_week(u32 year, u32 month, u32 day) {
    if (month < 3u) {
        month += 12u;
        --year;
    }
    const u32 k = year % 100u;
    const u32 j = year / 100u;
    const u32 h = (day + 13u * (month + 1u) / 5u + k + k / 4u + j / 4u + 5u * j) % 7u; /* 0 Saturday */
    return (h + 6u) % 7u;
}

static void format_date(const SYSTEMTIME *t, const WCHAR *picture, text_out *out) {
    const u32 dow = day_of_week(t->wYear, t->wMonth, t->wDay);
    const WCHAR *at = picture;
    while (*at != 0) {
        const WCHAR c = *at;
        if (c == '\'') {
            quoted(&at, out);
            continue;
        }
        if (c == 'd') {
            const u32 n = run_of(&at);
            if (n <= 2u) {
                put_number(out, t->wDay, n);
            } else {
                /* LOCALE_SDAYNAME1 is Monday. */
                const u32 index = (dow + 6u) % 7u;
                put_text(out, locale_value((n == 3u ? LOCALE_SABBREVDAYNAME1 : LOCALE_SDAYNAME1) + index));
            }
        } else if (c == 'M') {
            const u32 n = run_of(&at);
            if (n <= 2u) {
                put_number(out, t->wMonth, n);
            } else {
                put_text(out, locale_value((n == 3u ? LOCALE_SABBREVMONTHNAME1 : LOCALE_SMONTHNAME1) + t->wMonth - 1u));
            }
        } else if (c == 'y') {
            const u32 n = run_of(&at);
            if (n <= 2u) {
                put_number(out, t->wYear % 100u, n);
            } else {
                put_number(out, t->wYear, 1);
            }
        } else if (c == 'g') {
            (void)run_of(&at); /* the Gregorian calendar's era has no name here */
        } else {
            put_char(out, c);
            ++at;
        }
    }
}

/* Writes what out counted: the length with its terminator, or 0 with the
 * error when it does not fit. */
static int finish(text_out *out, WCHAR *buffer, int capacity) {
    if (capacity == 0) {
        return out->length + 1;
    }
    if (out->length + 1 > capacity) {
        SetLastError(ERROR_INSUFFICIENT_BUFFER);
        return 0;
    }
    buffer[out->length] = 0;
    return out->length + 1;
}

int WINAPI GetDateFormatW(LCID Locale, DWORD dwFlags, const SYSTEMTIME *lpDate, LPCWSTR lpFormat, LPWSTR lpDateStr,
                          int cchDate) {
    if (!known_locale(Locale) || cchDate < 0 || (cchDate != 0 && lpDateStr == NULL)) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return 0;
    }
    const u32 kind = dwFlags & ~LOCALE_NOUSEROVERRIDE;
    if ((lpFormat != NULL && kind != 0u) || (kind & ~(DATE_SHORTDATE | DATE_LONGDATE | DATE_YEARMONTH)) != 0u ||
        (kind & (kind - 1u)) != 0u) {
        SetLastError(ERROR_INVALID_FLAGS);
        return 0;
    }
    SYSTEMTIME now;
    if (lpDate == NULL) {
        GetLocalTime(&now);
        lpDate = &now;
    }
    if (lpDate->wYear < 1601u || lpDate->wYear > 30827u || lpDate->wMonth < 1u || lpDate->wMonth > 12u ||
        lpDate->wDay < 1u || lpDate->wDay > days_in_month(lpDate->wYear, lpDate->wMonth)) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return 0;
    }
    const WCHAR *picture = lpFormat != NULL                ? lpFormat
                           : (kind & DATE_LONGDATE) != 0u  ? locale_value(LOCALE_SLONGDATE)
                           : (kind & DATE_YEARMONTH) != 0u ? locale_value(LOCALE_SYEARMONTH)
                                                           : locale_value(LOCALE_SSHORTDATE);
    text_out out = {lpDateStr, cchDate, 0};
    format_date(lpDate, picture, &out);
    return finish(&out, lpDateStr, cchDate);
}

/* A time picture's fields: h, H, m, s and t runs. A field the flags drop
 * takes with it the text between it and the field before. */
static void format_time(const SYSTEMTIME *t, const WCHAR *picture, u32 flags, text_out *out) {
    WCHAR pending[64];
    u32 pending_length = 0;
    int written_field = 0;
    const WCHAR *at = picture;
    while (*at != 0) {
        const WCHAR c = *at;
        if (c == '\'') {
            text_out literal = {pending + pending_length, (int)(sizeof pending / sizeof pending[0]) - (int)pending_length, 0};
            quoted(&at, &literal);
            pending_length += (u32)literal.length < sizeof pending / sizeof pending[0] - pending_length
                                  ? (u32)literal.length
                                  : sizeof pending / sizeof pending[0] - pending_length;
            continue;
        }
        if (c != 'h' && c != 'H' && c != 'm' && c != 's' && c != 't') {
            if (pending_length < sizeof pending / sizeof pending[0]) {
                pending[pending_length++] = c;
            }
            ++at;
            continue;
        }
        const u32 n = run_of(&at);
        const int dropped = ((c == 'm' || c == 's') && (flags & TIME_NOMINUTESORSECONDS) != 0u) ||
                            (c == 's' && (flags & TIME_NOSECONDS) != 0u) ||
                            (c == 't' && (flags & TIME_NOTIMEMARKER) != 0u);
        if (dropped) {
            pending_length = 0;
            continue;
        }
        for (u32 i = 0; i < pending_length; ++i) {
            put_char(out, pending[i]);
        }
        pending_length = 0;
        written_field = 1;
        WCHAR kind = c;
        if (kind == 'h' && (flags & TIME_FORCE24HOURFORMAT) != 0u) {
            kind = 'H';
        }
        switch (kind) {
        case 'h': {
            const u32 hour = t->wHour % 12u;
            put_number(out, hour == 0u ? 12u : hour, n >= 2u ? 2u : 1u);
            break;
        }
        case 'H':
            put_number(out, t->wHour, n >= 2u ? 2u : 1u);
            break;
        case 'm':
            put_number(out, t->wMinute, n >= 2u ? 2u : 1u);
            break;
        case 's':
            put_number(out, t->wSecond, n >= 2u ? 2u : 1u);
            break;
        default: {
            const WCHAR *marker = locale_value(t->wHour < 12u ? LOCALE_S1159 : LOCALE_S2359);
            if (n == 1u) {
                if (marker[0] != 0) {
                    put_char(out, marker[0]);
                }
            } else {
                put_text(out, marker);
            }
            break;
        }
        }
    }
    (void)written_field;
    for (u32 i = 0; i < pending_length; ++i) {
        put_char(out, pending[i]);
    }
}

int WINAPI GetTimeFormatW(LCID Locale, DWORD dwFlags, const SYSTEMTIME *lpTime, LPCWSTR lpFormat, LPWSTR lpTimeStr,
                          int cchTime) {
    if (!known_locale(Locale) || cchTime < 0 || (cchTime != 0 && lpTimeStr == NULL)) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return 0;
    }
    const u32 flags = dwFlags & ~LOCALE_NOUSEROVERRIDE;
    if ((flags & ~(TIME_NOMINUTESORSECONDS | TIME_NOSECONDS | TIME_NOTIMEMARKER | TIME_FORCE24HOURFORMAT)) != 0u) {
        SetLastError(ERROR_INVALID_FLAGS);
        return 0;
    }
    SYSTEMTIME now;
    if (lpTime == NULL) {
        GetLocalTime(&now);
        lpTime = &now;
    }
    if (lpTime->wHour > 23u || lpTime->wMinute > 59u || lpTime->wSecond > 59u || lpTime->wMilliseconds > 999u) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return 0;
    }
    text_out out = {lpTimeStr, cchTime, 0};
    format_time(lpTime, lpFormat != NULL ? lpFormat : locale_value(LOCALE_STIMEFORMAT), flags, &out);
    return finish(&out, lpTimeStr, cchTime);
}

/* The pictures Windows XP lists for this locale, the default first. */
static const WCHAR *const k_short_dates[] = {L"M/d/yyyy", L"M/d/yy",     L"MM/dd/yy",  L"MM/dd/yyyy",
                                              L"yy/MM/dd", L"yyyy-MM-dd", L"dd-MMM-yy", 0};
static const WCHAR *const k_long_dates[] = {L"dddd, MMMM dd, yyyy", L"MMMM dd, yyyy", L"dddd, dd MMMM, yyyy",
                                             L"dd MMMM, yyyy", 0};
static const WCHAR *const k_year_months[] = {L"MMMM, yyyy", 0};
static const WCHAR *const k_times[] = {L"h:mm:ss tt", L"hh:mm:ss tt", L"H:mm:ss", L"HH:mm:ss", 0};

typedef BOOL(CALLBACK *enum_picture)(LPWSTR picture);

static BOOL enumerate(enum_picture callback, const WCHAR *const *list) {
    static WCHAR copy[64];
    for (u32 i = 0; list[i] != 0; ++i) {
        u32 n = 0;
        for (; list[i][n] != 0 && n + 1u < sizeof copy / sizeof copy[0]; ++n) {
            copy[n] = list[i][n];
        }
        copy[n] = 0;
        if (!callback(copy)) {
            break;
        }
    }
    return TRUE;
}

BOOL WINAPI EnumDateFormatsW(enum_picture lpDateFmtEnumProc, LCID Locale, DWORD dwFlags) {
    if (lpDateFmtEnumProc == NULL || !known_locale(Locale)) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    switch (dwFlags & ~LOCALE_USE_CP_ACP) {
    case DATE_SHORTDATE: return enumerate(lpDateFmtEnumProc, k_short_dates);
    case DATE_LONGDATE: return enumerate(lpDateFmtEnumProc, k_long_dates);
    case DATE_YEARMONTH: return enumerate(lpDateFmtEnumProc, k_year_months);
    default:
        SetLastError(ERROR_INVALID_FLAGS);
        return FALSE;
    }
}

BOOL WINAPI EnumTimeFormatsW(enum_picture lpTimeFmtEnumProc, LCID Locale, DWORD dwFlags) {
    if (lpTimeFmtEnumProc == NULL || !known_locale(Locale)) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    if ((dwFlags & ~LOCALE_USE_CP_ACP) != 0u) {
        SetLastError(ERROR_INVALID_FLAGS);
        return FALSE;
    }
    return enumerate(lpTimeFmtEnumProc, k_times);
}

/* ---- CompareStringW ------------------------------------------------------------ */

/* One character's weights at each level. primary 0 means the character is
 * left out (a symbol under NORM_IGNORESYMBOLS); special marks the hyphen
 * and the apostrophe, which a word sort weighs only at the last level. */
typedef struct weight {
    u32 primary;
    u8 accent;
    u8 upper;
    u8 special;
} weight;

/* Latin-1's letters: the base letter and the accent, 0 for none. */
static const char k_base[] = "AAAAAAACEEEEIIIIDNOOOOO*OUUUUYTsaaaaaaaceeeeiiiidnooooo/ouuuuyty";
static const u8 k_accent[64] = {2, 1, 3, 4, 5, 6, 9, 7, 2, 1, 3, 5, 2, 1, 3, 5, 8, 4, 2, 1, 3, 4, 5, 0,
                                8, 2, 1, 3, 5, 1, 0, 0, 2, 1, 3, 4, 5, 6, 9, 7, 2, 1, 3, 5, 2, 1, 3, 5,
                                8, 4, 2, 1, 3, 4, 5, 0, 8, 2, 1, 3, 5, 1, 0, 5};

/* The alphabet's order, a to z: ñ is n with a tilde, as English sorts it. */
static u32 letter_rank(u32 lower) {
    return lower - 'a';
}

static weight weigh(WCHAR c, u32 flags) {
    weight w = {0, 0, 0, 0};
    if (c == '-' || c == '\'' || c == 0xadu) {
        if ((flags & NORM_IGNORESYMBOLS) != 0u) {
            return w;
        }
        if ((flags & SORT_STRINGSORT) == 0u) {
            w.special = (u8)c;
            return w;
        }
        w.primary = 0x100u + c;
        return w;
    }
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) {
        const u32 lower = c >= 'a' ? c : c + 0x20u;
        w.primary = 0x1000u + 4u * letter_rank(lower);
        w.upper = c < 'a';
        return w;
    }
    if (c >= 0xc0u && c <= 0xffu && c != 0xd7u && c != 0xf7u) {
        const char base = k_base[c - 0xc0u];
        if (c == 0xdfu) {
            w.primary = 0x1000u + 4u * letter_rank('s') + 1u;
        } else if (c == 0xdeu || c == 0xfeu) {
            w.primary = 0x1000u + 4u * 27u; /* thorn, after z */
        } else {
            const u32 lower = (u32)(base >= 'a' ? base : base + 0x20);
            w.primary = 0x1000u + 4u * letter_rank(lower);
        }
        w.accent = k_accent[c - 0xc0u];
        w.upper = c < 0xe0u;
        return w;
    }
    if (c >= '0' && c <= '9') {
        w.primary = 0x0800u + c;
        return w;
    }
    if (c < 0x80u || (c >= 0xa0u && c < 0xc0u) || c == 0xd7u || c == 0xf7u) {
        if ((flags & NORM_IGNORESYMBOLS) != 0u) {
            return w;
        }
        w.primary = 0x100u + c;
        return w;
    }
    w.primary = 0x2000u + c;
    return w;
}

#define COMPARE_MAX 4096u

typedef struct weights {
    weight items[COMPARE_MAX];
    u32 count;
    u32 special_at[COMPARE_MAX];
    WCHAR special[COMPARE_MAX];
    u32 special_count;
} weights;

static void weigh_all(const WCHAR *s, int length, u32 flags, weights *out) {
    out->count = 0;
    out->special_count = 0;
    for (int i = 0; (length < 0 ? s[i] != 0 : i < length) && out->count < COMPARE_MAX; ++i) {
        const weight w = weigh(s[i], flags);
        if (w.special != 0u) {
            out->special_at[out->special_count] = out->count;
            out->special[out->special_count++] = s[i];
        } else if (w.primary != 0u) {
            out->items[out->count++] = w;
        }
    }
}

static int order(u32 a, u32 b) {
    return a < b ? CSTR_LESS_THAN : a > b ? CSTR_GREATER_THAN : CSTR_EQUAL;
}

int WINAPI CompareStringW(LCID Locale, DWORD dwCmpFlags, LPCWSTR lpString1, int cchCount1, LPCWSTR lpString2,
                          int cchCount2) {
    static weights a;
    static weights b;
    if (lpString1 == NULL || lpString2 == NULL || !known_locale(Locale)) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return 0;
    }
    if ((dwCmpFlags & ~(NORM_IGNORECASE | NORM_IGNORENONSPACE | NORM_IGNORESYMBOLS | NORM_IGNOREKANATYPE |
                        NORM_IGNOREWIDTH | SORT_STRINGSORT)) != 0u) {
        SetLastError(ERROR_INVALID_FLAGS);
        return 0;
    }
    weigh_all(lpString1, cchCount1, dwCmpFlags, &a);
    weigh_all(lpString2, cchCount2, dwCmpFlags, &b);
    const u32 n = a.count < b.count ? a.count : b.count;
    for (u32 i = 0; i < n; ++i) {
        if (a.items[i].primary != b.items[i].primary) {
            return order(a.items[i].primary, b.items[i].primary);
        }
    }
    if (a.count != b.count) {
        return order(a.count, b.count);
    }
    if ((dwCmpFlags & NORM_IGNORENONSPACE) == 0u) {
        for (u32 i = 0; i < n; ++i) {
            if (a.items[i].accent != b.items[i].accent) {
                return order(a.items[i].accent, b.items[i].accent);
            }
        }
    }
    if ((dwCmpFlags & NORM_IGNORECASE) == 0u) {
        for (u32 i = 0; i < n; ++i) {
            if (a.items[i].upper != b.items[i].upper) {
                return order(a.items[i].upper, b.items[i].upper);
            }
        }
    }
    const u32 m = a.special_count < b.special_count ? a.special_count : b.special_count;
    for (u32 i = 0; i < m; ++i) {
        if (a.special_at[i] != b.special_at[i]) {
            return order(b.special_at[i], a.special_at[i]);
        }
        if (a.special[i] != b.special[i]) {
            return order(a.special[i], b.special[i]);
        }
    }
    return order(a.special_count, b.special_count);
}

/* ---- win.ini ---------------------------------------------------------------------- */
/*
 * There is no win.ini: every section is empty, so each name answers with the
 * default it is given, the way Windows XP answers a name its win.ini lacks.
 */

UINT WINAPI GetProfileIntW(LPCWSTR lpAppName, LPCWSTR lpKeyName, int nDefault) {
    (void)lpAppName;
    (void)lpKeyName;
    return (UINT)nDefault;
}

UINT WINAPI GetProfileIntA(LPCSTR lpAppName, LPCSTR lpKeyName, int nDefault) {
    (void)lpAppName;
    (void)lpKeyName;
    return (UINT)nDefault;
}

/* The default, cut to the buffer; the count is the shorter of its length
 * and the buffer's, as Windows XP answered for a missing name. */
DWORD WINAPI GetProfileStringW(LPCWSTR lpAppName, LPCWSTR lpKeyName, LPCWSTR lpDefault, LPWSTR lpReturnedString,
                               DWORD nSize) {
    (void)lpAppName;
    if (lpReturnedString == NULL || nSize == 0u) {
        return 0;
    }
    if (lpKeyName == NULL || lpAppName == NULL) {
        /* The list of names: empty, with its two terminators. */
        lpReturnedString[0] = 0;
        if (nSize > 1u) {
            lpReturnedString[1] = 0;
        }
        return 0;
    }
    const WCHAR *value = lpDefault != NULL ? lpDefault : (const WCHAR *)L"";
    const DWORD length = wide_length(value);
    DWORD n = 0;
    while (n < length && n + 1u < nSize) {
        lpReturnedString[n] = value[n];
        ++n;
    }
    lpReturnedString[n] = 0;
    return length < nSize ? length : nSize;
}

/* An empty section: one terminator and 0, as Windows XP answers for a
 * section its win.ini lacks. */
DWORD WINAPI GetProfileSectionW(LPCWSTR lpAppName, LPWSTR lpReturnedString, DWORD nSize) {
    (void)lpAppName;
    if (lpReturnedString != NULL && nSize != 0u) {
        lpReturnedString[0] = 0;
    }
    return 0;
}
