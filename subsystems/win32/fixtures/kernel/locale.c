/*
 * locale.exe -- WP4's fixture for the locale and win.ini
 * (WIN32_WORDPAD_ROADMAP.md): every value GetLocaleInfoW has for the user's
 * locale, English (United States), the date and time formats WordPad's Date
 * and Time lists and how they format a fixed instant, CompareStringW on pairs
 * that show the collation, and win.ini's answers to names it does not hold.
 *
 * It needs Windows XP set to English (United States), as the test machine
 * is; nothing it prints depends on the date, the clock or the time zone.
 */
#include "print.h"

typedef BOOL(CALLBACK *DATEFMT_ENUMPROCW)(LPWSTR);
typedef BOOL(CALLBACK *TIMEFMT_ENUMPROCW)(LPWSTR);

DECLSPEC_IMPORT int WINAPI GetLocaleInfoW(LCID locale, DWORD type, LPWSTR data, int size);
DECLSPEC_IMPORT LCID WINAPI GetUserDefaultLCID(void);
DECLSPEC_IMPORT int WINAPI GetDateFormatW(LCID locale, DWORD flags, const SYSTEMTIME *date, LPCWSTR format,
                                          LPWSTR out, int size);
DECLSPEC_IMPORT int WINAPI GetTimeFormatW(LCID locale, DWORD flags, const SYSTEMTIME *time, LPCWSTR format,
                                          LPWSTR out, int size);
DECLSPEC_IMPORT BOOL WINAPI EnumDateFormatsW(DATEFMT_ENUMPROCW callback, LCID locale, DWORD flags);
DECLSPEC_IMPORT BOOL WINAPI EnumTimeFormatsW(TIMEFMT_ENUMPROCW callback, LCID locale, DWORD flags);
DECLSPEC_IMPORT int WINAPI CompareStringW(LCID locale, DWORD flags, LPCWSTR a, int a_length, LPCWSTR b,
                                          int b_length);
DECLSPEC_IMPORT UINT WINAPI GetProfileIntW(LPCWSTR section, LPCWSTR key, int fallback);
DECLSPEC_IMPORT UINT WINAPI GetProfileIntA(LPCSTR section, LPCSTR key, int fallback);
DECLSPEC_IMPORT DWORD WINAPI GetProfileStringW(LPCWSTR section, LPCWSTR key, LPCWSTR fallback, LPWSTR out,
                                               DWORD size);
DECLSPEC_IMPORT DWORD WINAPI GetProfileSectionW(LPCWSTR section, LPWSTR out, DWORD size);

#define LOCALE_USER_DEFAULT 0x0400u
#define LOCALE_NOUSEROVERRIDE 0x80000000u
#define LOCALE_RETURN_NUMBER 0x20000000u
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
#define SORT_STRINGSORT 0x1000u

static void locale_values(void) {
    line("GetUserDefaultLCID");
    say_hex(GetUserDefaultLCID());
    WCHAR value[128];
    for (DWORD type = 1; type <= 0x7fu; ++type) {
        if (type == 0x58u) {
            continue; /* LOCALE_FONTSIGNATURE: binary, not text */
        }
        const int n = GetLocaleInfoW(LOCALE_USER_DEFAULT, type, value, 128);
        if (n == 0) {
            continue;
        }
        line("LOCALE");
        say_hex(type);
        say(" ");
        say_int(n);
        say(" ");
        say_w(value);
    }
    for (DWORD type = 0x1000u; type <= 0x1080u; ++type) {
        const int n = GetLocaleInfoW(LOCALE_USER_DEFAULT, type, value, 128);
        if (n == 0) {
            continue;
        }
        line("LOCALE");
        say_hex(type);
        say(" ");
        say_int(n);
        say(" ");
        say_w(value);
    }
    line("LOCALE_SDECIMAL by number");
    say_int(GetLocaleInfoW(GetUserDefaultLCID(), 0x000eu, value, 128));
    say(" ");
    say_w(value);
    line("LOCALE_IMEASURE as a number");
    DWORD number = 0;
    say_int(GetLocaleInfoW(LOCALE_USER_DEFAULT, 0x000du | LOCALE_RETURN_NUMBER, (LPWSTR)&number, 2));
    say(" ");
    say_int((long)number);
    line("LOCALE_ILANGUAGE as a number");
    say_int(GetLocaleInfoW(LOCALE_USER_DEFAULT, 0x0001u | LOCALE_RETURN_NUMBER, (LPWSTR)&number, 2));
    say(" ");
    say_hex(number);
    line("LOCALE_SLONGDATE needs");
    say_int(GetLocaleInfoW(LOCALE_USER_DEFAULT, 0x0020u, value, 0));
    line("LOCALE_SLONGDATE in 4");
    say_int(GetLocaleInfoW(LOCALE_USER_DEFAULT, 0x0020u, value, 4));
    say_error();
    line("type 0");
    say_int(GetLocaleInfoW(LOCALE_USER_DEFAULT, 0, value, 128));
    say_error();
    line("an unknown locale");
    SetLastError(0);
    say_int(GetLocaleInfoW(0x7777u, 0x0002u, value, 128));
    say_error();
}

static const SYSTEMTIME k_instant = {2001, 8, 5, 24, 14, 5, 9, 250};
static const SYSTEMTIME k_morning = {2001, 1, 1, 1, 0, 7, 3, 0};

static void say_date(DWORD flags, const SYSTEMTIME *t, const WCHAR *format) {
    WCHAR out[128];
    out[0] = 0;
    const int n = GetDateFormatW(LOCALE_USER_DEFAULT, flags, t, format, out, 128);
    say_int(n);
    if (n == 0) {
        say_error();
    } else {
        say(" ");
        say_w(out);
    }
}

static void say_time(DWORD flags, const SYSTEMTIME *t, const WCHAR *format) {
    WCHAR out[128];
    out[0] = 0;
    const int n = GetTimeFormatW(LOCALE_USER_DEFAULT, flags, t, format, out, 128);
    say_int(n);
    if (n == 0) {
        say_error();
    } else {
        say(" ");
        say_w(out);
    }
}

static BOOL CALLBACK list_format(LPWSTR format) {
    say(" ");
    say_w(format);
    return TRUE;
}

static int g_seen;
static BOOL CALLBACK first_two(LPWSTR format) {
    say(" ");
    say_w(format);
    return ++g_seen < 2;
}

static void dates(void) {
    static const DWORD flag_cases[] = {0, DATE_SHORTDATE, DATE_LONGDATE, DATE_YEARMONTH};
    for (unsigned i = 0; i < sizeof flag_cases / sizeof flag_cases[0]; ++i) {
        line("GetDateFormatW flags");
        say_hex(flag_cases[i]);
        say(" ");
        say_date(flag_cases[i], &k_instant, NULL);
        say(" / ");
        say_date(flag_cases[i], &k_morning, NULL);
    }
    static const WCHAR *const pictures[] = {
        L"dddd, d' de 'MMMM' de 'yyyy", L"dd/MM/yy", L"d/M/y", L"ddd MMM", L"yyyyy", L"'it''s' d", L"gg yyyy",
        L"MMMMM ddddd", L"no letters here", L"'unclosed", L"d dd ddd dddd M MM MMM MMMM y yy yyyy",
    };
    for (unsigned i = 0; i < sizeof pictures / sizeof pictures[0]; ++i) {
        line("GetDateFormatW");
        say_w(pictures[i]);
        say(" ");
        say_date(0, &k_instant, pictures[i]);
    }
    WCHAR out[8];
    line("GetDateFormatW needs");
    say_int(GetDateFormatW(LOCALE_USER_DEFAULT, DATE_LONGDATE, &k_instant, NULL, out, 0));
    line("GetDateFormatW in 8");
    say_int(GetDateFormatW(LOCALE_USER_DEFAULT, DATE_LONGDATE, &k_instant, NULL, out, 8));
    say_error();
    line("GetDateFormatW today's year");
    say_int(GetDateFormatW(LOCALE_USER_DEFAULT, 0, NULL, L"yyyy", out, 8));
    const SYSTEMTIME bad = {2001, 2, 0, 30, 0, 0, 0, 0};
    line("GetDateFormatW(February 30)");
    say_date(0, &bad, NULL);
    line("GetDateFormatW(format and flags)");
    say_date(DATE_LONGDATE, &k_instant, L"yyyy");
    line("EnumDateFormatsW(short):");
    say_result(EnumDateFormatsW(list_format, LOCALE_USER_DEFAULT, DATE_SHORTDATE));
    line("EnumDateFormatsW(long):");
    say_result(EnumDateFormatsW(list_format, LOCALE_USER_DEFAULT, DATE_LONGDATE));
    line("EnumDateFormatsW(short), two:");
    g_seen = 0;
    say_result(EnumDateFormatsW(first_two, LOCALE_USER_DEFAULT, DATE_SHORTDATE));
    line("EnumDateFormatsW(NULL)");
    say_result(EnumDateFormatsW(NULL, LOCALE_USER_DEFAULT, DATE_SHORTDATE));
}

static void times(void) {
    static const DWORD flag_cases[] = {0, TIME_NOSECONDS, TIME_NOMINUTESORSECONDS, TIME_NOTIMEMARKER,
                                       TIME_FORCE24HOURFORMAT, TIME_NOSECONDS | TIME_FORCE24HOURFORMAT};
    for (unsigned i = 0; i < sizeof flag_cases / sizeof flag_cases[0]; ++i) {
        line("GetTimeFormatW flags");
        say_hex(flag_cases[i]);
        say(" ");
        say_time(flag_cases[i], &k_instant, NULL);
        say(" / ");
        say_time(flag_cases[i], &k_morning, NULL);
    }
    static const WCHAR *const pictures[] = {L"HH:mm", L"h tt", L"H 'h' m 'min'", L"hh:mm:ss t", L"tt", L"mm:ss"};
    for (unsigned i = 0; i < sizeof pictures / sizeof pictures[0]; ++i) {
        line("GetTimeFormatW");
        say_w(pictures[i]);
        say(" ");
        say_time(0, &k_instant, pictures[i]);
        say(" / ");
        say_time(0, &k_morning, pictures[i]);
    }
    const SYSTEMTIME bad = {2001, 8, 5, 24, 25, 0, 0, 0};
    line("GetTimeFormatW(25 hours)");
    say_time(0, &bad, NULL);
    line("EnumTimeFormatsW:");
    say_result(EnumTimeFormatsW(list_format, LOCALE_USER_DEFAULT, 0));
}

static void compare(void) {
    static const struct {
        const WCHAR *a;
        const WCHAR *b;
    } pairs[] = {
        {L"a", L"B"},        {L"abc", L"ABC"},     {L"Abc", L"abc"},   {L"caf\xe9", L"cafe"}, {L"caf\xe9", L"cafes"},
        {L"\xf1", L"n"},     {L"\xf1", L"o"},      {L"\xd1", L"\xf1"}, {L"co-op", L"coop"},   {L"co-op", L"cop"},
        {L"a", L"a "},       {L"", L"a"},          {L"a1", L"a10"},    {L"a2", L"a10"},       {L"r\xe9sum\xe9", L"resume"},
        {L"x.y", L"xy"},     {L"zebra", L"\xc1rbol"}, {L"chico", L"cosa"}, {L"llama", L"luz"}, {L"a'b", L"ab"},
    };
    static const DWORD flag_cases[] = {0, NORM_IGNORECASE, NORM_IGNORENONSPACE, NORM_IGNORESYMBOLS, SORT_STRINGSORT,
                                       NORM_IGNORECASE | NORM_IGNORENONSPACE};
    for (unsigned i = 0; i < sizeof pairs / sizeof pairs[0]; ++i) {
        line("CompareStringW");
        say_w(pairs[i].a);
        say(" ");
        say_w(pairs[i].b);
        say(":");
        for (unsigned f = 0; f < sizeof flag_cases / sizeof flag_cases[0]; ++f) {
            say(" ");
            say_int(CompareStringW(LOCALE_USER_DEFAULT, flag_cases[f], pairs[i].a, -1, pairs[i].b, -1));
        }
    }
    line("CompareStringW with lengths");
    say_int(CompareStringW(LOCALE_USER_DEFAULT, 0, L"abcdef", 3, L"abcxyz", 3));
    say(" ");
    say_int(CompareStringW(LOCALE_USER_DEFAULT, 0, L"abc", 2, L"abc", -1));
    line("CompareStringW(NULL)");
    say_int(CompareStringW(LOCALE_USER_DEFAULT, 0, NULL, -1, L"a", -1));
    say_error();
    line("CompareStringW(bad flags)");
    say_int(CompareStringW(LOCALE_USER_DEFAULT, 0x80000000u, L"a", -1, L"a", -1));
    say_error();
}

static void profile(void) {
    WCHAR out[64];
    line("GetProfileIntW(windows, a missing key, 7)");
    say_int((long)GetProfileIntW(L"windows", L"SxwFixtureMissing", 7));
    line("GetProfileIntA(windows, a missing key, -3)");
    say_int((long)GetProfileIntA("windows", "SxwFixtureMissing", -3));
    line("GetProfileIntW(a missing section, key, 11)");
    say_int((long)GetProfileIntW(L"SxwFixtureSection", L"Key", 11));
    line("GetProfileStringW(missing, default)");
    const DWORD n = GetProfileStringW(L"SxwFixtureSection", L"Key", L"by default", out, 64);
    say_int((long)n);
    say(" ");
    say_w(out);
    line("GetProfileStringW(missing, default) in 4");
    say_int((long)GetProfileStringW(L"SxwFixtureSection", L"Key", L"by default", out, 4));
    say(" ");
    say_w(out);
    line("GetProfileSectionW(missing)");
    out[0] = 'x';
    out[1] = 'x';
    say_int((long)GetProfileSectionW(L"SxwFixtureSection", out, 64));
    say(" units ");
    say_int(out[0]);
    say(" ");
    say_int(out[1]);
}

void __stdcall start(void) {
    say("locale.exe");
    locale_values();
    dates();
    times();
    compare();
    profile();
    say("\r\n");
    ExitProcess(0);
}
