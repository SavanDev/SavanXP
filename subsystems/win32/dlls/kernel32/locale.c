/*
 * locale.c -- kernel32's locale: one, Spanish (Argentina), LCID 0x2C0A.
 *
 * SavanXP has no regional setting yet, so every locale query answers with
 * the locale of the Windows XP the profiles were taken from. Only the values
 * a program has asked for are written; any other is reported once on the
 * debug output and refused with ERROR_INVALID_FLAGS, so the next one to add
 * shows up by name.
 */
#define SXW_BUILDING_KERNEL32
#include <windows.h>

#include "sxw_abi.h"

typedef unsigned int u32;

__declspec(dllimport) u32 WINAPI SxwService(u32 service, const u32 *arguments);

#define LOCALE_SPANISH_ARGENTINA 0x2c0au
#define LOCALE_SYSTEM_DEFAULT 0x0800u
#define LOCALE_USER_DEFAULT 0x0400u
#define LOCALE_NOUSEROVERRIDE 0x80000000u
#define LOCALE_RETURN_NUMBER 0x20000000u
#define ERROR_INVALID_FLAGS 1004u

LCID WINAPI GetUserDefaultLCID(void) {
    return LOCALE_SPANISH_ARGENTINA;
}

LCID WINAPI GetSystemDefaultLCID(void) {
    return LOCALE_SPANISH_ARGENTINA;
}

LCID WINAPI GetThreadLocale(void) {
    return LOCALE_SPANISH_ARGENTINA;
}

static const struct {
    u32 type;
    const char *value;
} k_values[] = {
    {0x0001u, "2c0a"},  /* LOCALE_ILANGUAGE */
    {0x000bu, "850"},   /* LOCALE_IDEFAULTCODEPAGE */
    {0x000du, "0"},     /* LOCALE_IMEASURE: metric */
    {0x000eu, ","},     /* LOCALE_SDECIMAL */
    {0x000fu, "."},     /* LOCALE_STHOUSAND */
    {0x1004u, "1252"},  /* LOCALE_IDEFAULTANSICODEPAGE */
};

static void report_unknown(u32 type) {
    static u32 reported[16];
    static u32 count;
    for (u32 i = 0; i < count; ++i) {
        if (reported[i] == type) {
            return;
        }
    }
    if (count < 16u) {
        reported[count++] = type;
    }
    char line[64] = "kernel32.dll!GetLocaleInfoW: no value for 0x";
    u32 n = 0;
    while (line[n] != 0) {
        ++n;
    }
    for (int shift = 28; shift >= 0; shift -= 4) {
        line[n++] = "0123456789abcdef"[(type >> shift) & 15u];
    }
    u32 arguments[2] = {(u32)line, n};
    SxwService(SXW_SVC_DEBUG_OUTPUT, arguments);
}

int WINAPI GetLocaleInfoW(LCID Locale, DWORD LCType, LPWSTR lpLCData, int cchData) {
    if (Locale != LOCALE_SPANISH_ARGENTINA && Locale != LOCALE_USER_DEFAULT && Locale != LOCALE_SYSTEM_DEFAULT &&
        Locale != 0) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return 0;
    }
    u32 type = LCType & ~(LOCALE_NOUSEROVERRIDE | LOCALE_RETURN_NUMBER);
    const char *value = 0;
    for (u32 i = 0; i < sizeof k_values / sizeof k_values[0]; ++i) {
        if (k_values[i].type == type) {
            value = k_values[i].value;
        }
    }
    if (value == 0) {
        report_unknown(type);
        SetLastError(ERROR_INVALID_FLAGS);
        return 0;
    }
    if ((LCType & LOCALE_RETURN_NUMBER) != 0u) {
        /* The number as a DWORD: two WCHARs' room. Hex for the language. */
        u32 number = 0;
        u32 base = type == 0x0001u ? 16u : 10u;
        for (const char *c = value; *c != 0; ++c) {
            number = number * base + (u32)(*c <= '9' ? *c - '0' : *c - 'a' + 10);
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
    int length = 0;
    while (value[length] != 0) {
        ++length;
    }
    if (cchData == 0) {
        return length + 1;
    }
    if (cchData <= length || lpLCData == NULL) {
        SetLastError(ERROR_INSUFFICIENT_BUFFER);
        return 0;
    }
    for (int i = 0; i <= length; ++i) {
        lpLCData[i] = (WCHAR)(unsigned char)value[i];
    }
    return length + 1;
}
