/*
 * time.c -- kernel32's calendar: SYSTEMTIME and FILETIME, and local time.
 *
 * Local time is UTC: SavanXP has no time zone yet (TIMEZONE_ROADMAP.md, T6
 * is Win32's), so GetLocalTime answers what GetSystemTime does and the
 * local file-time conversions change nothing.
 */
#define SXW_BUILDING_KERNEL32
#include <windows.h>

#include "sxw_abi.h"

typedef unsigned int u32;
typedef unsigned long long u64;

static void set_last_error(DWORD error) {
    __asm__ volatile("movl %0, %%fs:%c1" : : "r"(error), "i"(SXW_TEB_LAST_ERROR) : "memory");
}

/* Unsigned 64 by 32 division without the C runtime's __aulldiv. */
static u64 divide(u64 dividend, u32 divisor, u32 *remainder) {
    u64 quotient = 0;
    u64 rest = 0;
    for (int bit = 63; bit >= 0; --bit) {
        rest = (rest << 1) | ((dividend >> bit) & 1u);
        if (rest >= divisor) {
            rest -= divisor;
            quotient |= 1ull << bit;
        }
    }
    *remainder = (u32)rest;
    return quotient;
}

static int leap(u32 year) {
    return (year % 4u == 0u && year % 100u != 0u) || year % 400u == 0u;
}

static u32 days_in_month(u32 year, u32 month) {
    static const unsigned char days[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    return month == 2u && leap(year) ? 29u : days[month - 1u];
}

BOOL WINAPI FileTimeToSystemTime(const FILETIME *lpFileTime, LPSYSTEMTIME lpSystemTime) {
    if ((lpFileTime->dwHighDateTime & 0x80000000u) != 0u) {
        set_last_error(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    const u64 t = ((u64)lpFileTime->dwHighDateTime << 32) | lpFileTime->dwLowDateTime;
    u32 ticks;
    const u64 seconds_total = divide(t, 10000000u, &ticks);
    u32 seconds_of_day;
    u32 days = (u32)divide(seconds_total, 86400u, &seconds_of_day);
    lpSystemTime->wMilliseconds = (WORD)(ticks / 10000u);
    lpSystemTime->wSecond = (WORD)(seconds_of_day % 60u);
    lpSystemTime->wMinute = (WORD)(seconds_of_day / 60u % 60u);
    lpSystemTime->wHour = (WORD)(seconds_of_day / 3600u);
    /* 1601-01-01 was a Monday. */
    lpSystemTime->wDayOfWeek = (WORD)((days + 1u) % 7u);
    u32 year = 1601;
    for (;;) {
        const u32 in_year = leap(year) ? 366u : 365u;
        if (days < in_year) {
            break;
        }
        days -= in_year;
        ++year;
    }
    u32 month = 1;
    while (days >= days_in_month(year, month)) {
        days -= days_in_month(year, month);
        ++month;
    }
    lpSystemTime->wYear = (WORD)year;
    lpSystemTime->wMonth = (WORD)month;
    lpSystemTime->wDay = (WORD)(days + 1u);
    return TRUE;
}

BOOL WINAPI SystemTimeToFileTime(const SYSTEMTIME *lpSystemTime, LPFILETIME lpFileTime) {
    const SYSTEMTIME *s = lpSystemTime;
    if (s->wYear < 1601u || s->wYear > 30827u || s->wMonth < 1u || s->wMonth > 12u || s->wDay < 1u ||
        s->wDay > days_in_month(s->wYear, s->wMonth) || s->wHour > 23u || s->wMinute > 59u || s->wSecond > 59u ||
        s->wMilliseconds > 999u) {
        set_last_error(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    u64 days = 0;
    /* Whole 400-year cycles first: 146097 days each. */
    u32 year = 1601;
    while (year + 400u <= s->wYear) {
        days += 146097u;
        year += 400u;
    }
    for (; year < s->wYear; ++year) {
        days += leap(year) ? 366u : 365u;
    }
    for (u32 month = 1; month < s->wMonth; ++month) {
        days += days_in_month(s->wYear, month);
    }
    days += s->wDay - 1u;
    const u64 seconds = days * 86400ull + s->wHour * 3600u + s->wMinute * 60u + s->wSecond;
    const u64 t = seconds * 10000000ull + s->wMilliseconds * 10000u;
    lpFileTime->dwLowDateTime = (DWORD)t;
    lpFileTime->dwHighDateTime = (DWORD)(t >> 32);
    return TRUE;
}

void WINAPI GetSystemTime(LPSYSTEMTIME lpSystemTime) {
    FILETIME now;
    GetSystemTimeAsFileTime(&now);
    FileTimeToSystemTime(&now, lpSystemTime);
}

void WINAPI GetLocalTime(LPSYSTEMTIME lpSystemTime) {
    GetSystemTime(lpSystemTime);
}

BOOL WINAPI FileTimeToLocalFileTime(const FILETIME *lpFileTime, LPFILETIME lpLocalFileTime) {
    *lpLocalFileTime = *lpFileTime;
    return TRUE;
}

BOOL WINAPI LocalFileTimeToFileTime(const FILETIME *lpLocalFileTime, LPFILETIME lpFileTime) {
    *lpFileTime = *lpLocalFileTime;
    return TRUE;
}
