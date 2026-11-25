/*
 * system.c -- kernel32's process-wide state: the last error, the version,
 * the error mode, the clocks, thread-local storage, critical sections and
 * the interlocked operations.
 *
 * One thread runs in a process today (WIN32_WORDPAD_ROADMAP.md, WP3 adds
 * more), so a critical section only counts its owner's recursion and the
 * interlocked operations are plain read-modify-writes made with the x86
 * locked instructions, which stay right when threads arrive.
 */
#define SXW_BUILDING_KERNEL32
#include <windows.h>

#include "sxw_abi.h"

typedef unsigned int u32;

__declspec(dllimport) u32 WINAPI SxwService(u32 service, const u32 *arguments);

static u32 teb_read(u32 offset) {
    u32 value;
    __asm__ volatile("movl %%fs:(%1), %0" : "=r"(value) : "r"(offset));
    return value;
}

static void teb_write(u32 offset, u32 value) {
    __asm__ volatile("movl %0, %%fs:(%1)" : : "r"(value), "r"(offset) : "memory");
}

/* ---- The last error ----------------------------------------------------------- */

DWORD WINAPI GetLastError(void) {
    return teb_read(SXW_TEB_LAST_ERROR);
}

void WINAPI SetLastError(DWORD dwErrCode) {
    teb_write(SXW_TEB_LAST_ERROR, dwErrCode);
}

/* ---- The version: Windows XP Professional SP3 ----------------------------------- */

#define VERSION_MAJOR 5u
#define VERSION_MINOR 1u
#define VERSION_BUILD 2600u
#define VER_PLATFORM_WIN32_NT 2u

/* Build in the high word, minor then major in the low one; bit 31 clear: NT. */
DWORD WINAPI GetVersion(void) {
    return (VERSION_BUILD << 16) | (VERSION_MINOR << 8) | VERSION_MAJOR;
}

BOOL WINAPI GetVersionExA(LPOSVERSIONINFOA lpVersionInformation) {
    DWORD size = lpVersionInformation->dwOSVersionInfoSize;
    if (size != sizeof(OSVERSIONINFOA) && size != sizeof(OSVERSIONINFOEXA)) {
        SetLastError(ERROR_INSUFFICIENT_BUFFER);
        return FALSE;
    }
    lpVersionInformation->dwMajorVersion = VERSION_MAJOR;
    lpVersionInformation->dwMinorVersion = VERSION_MINOR;
    lpVersionInformation->dwBuildNumber = VERSION_BUILD;
    lpVersionInformation->dwPlatformId = VER_PLATFORM_WIN32_NT;
    static const char service_pack[] = "Service Pack 3";
    for (u32 i = 0; i < sizeof lpVersionInformation->szCSDVersion; ++i) {
        lpVersionInformation->szCSDVersion[i] = i < sizeof service_pack ? service_pack[i] : 0;
    }
    if (size == sizeof(OSVERSIONINFOEXA)) {
        OSVERSIONINFOEXA *ex = (OSVERSIONINFOEXA *)lpVersionInformation;
        ex->wServicePackMajor = 3;
        ex->wServicePackMinor = 0;
        ex->wSuiteMask = 0x0100; /* VER_SUITE_SINGLEUSERTS, as XP reports */
        ex->wProductType = 1;    /* VER_NT_WORKSTATION */
        ex->wReserved = 0;
    }
    return TRUE;
}

/* The subsystem version from the executable's header (process 0: this one),
 * major in the high word. Only the calling process is known. */
DWORD WINAPI GetProcessVersion(DWORD ProcessId) {
    if (ProcessId != 0 && ProcessId != teb_read(SXW_TEB_PROCESS_ID)) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return 0;
    }
    u32 peb = teb_read(SXW_TEB_PEB);
    const unsigned char *image = (const unsigned char *)*(u32 *)(peb + SXW_PEB_IMAGE_BASE);
    const unsigned char *optional = image + *(const u32 *)(image + 0x3c) + 24;
    return ((u32) * (const unsigned short *)(optional + 48) << 16) | *(const unsigned short *)(optional + 50);
}

/* Kept and handed back; nothing here shows a critical-error box anyway. */
static UINT g_error_mode;

UINT WINAPI SetErrorMode(UINT uMode) {
    UINT previous = g_error_mode;
    g_error_mode = uMode;
    return previous;
}

/* ---- The process and its thread ---------------------------------------------- */

HANDLE WINAPI GetCurrentProcess(void) {
    return (HANDLE)(long)-1;
}

HANDLE WINAPI GetCurrentThread(void) {
    return (HANDLE)(long)-2;
}

DWORD WINAPI GetCurrentProcessId(void) {
    return teb_read(SXW_TEB_PROCESS_ID);
}

DWORD WINAPI GetCurrentThreadId(void) {
    return teb_read(SXW_TEB_THREAD_ID);
}

/* Nothing asked for a window position, size, title or handles, so only cb is
 * set, as GetStartupInfoA does. */
void WINAPI GetStartupInfoW(LPSTARTUPINFOW lpStartupInfo) {
    unsigned char *p = (unsigned char *)lpStartupInfo;
    for (u32 i = 0; i < sizeof(*lpStartupInfo); ++i) {
        p[i] = 0;
    }
    lpStartupInfo->cb = sizeof(*lpStartupInfo);
}

/* ---- Clocks --------------------------------------------------------------------- */

DWORD WINAPI GetTickCount(void) {
    return SxwService(SXW_SVC_TICKS, 0);
}

/* 100-ns intervals since 1601: the host's seconds since 1970, so the
 * fraction of the second is always 0. */
void WINAPI GetSystemTimeAsFileTime(LPFILETIME lpSystemTimeAsFileTime) {
    unsigned long long seconds = (unsigned long long)SxwService(SXW_SVC_TIME, 0) + 11644473600ull;
    unsigned long long intervals = seconds * 10000000ull;
    lpSystemTimeAsFileTime->dwLowDateTime = (DWORD)intervals;
    lpSystemTimeAsFileTime->dwHighDateTime = (DWORD)(intervals >> 32);
}

/* The millisecond clock, at 1000 counts a second. */
BOOL WINAPI QueryPerformanceCounter(LARGE_INTEGER *lpPerformanceCount) {
    lpPerformanceCount->QuadPart = (long long)GetTickCount();
    return TRUE;
}

BOOL WINAPI QueryPerformanceFrequency(LARGE_INTEGER *lpFrequency) {
    lpFrequency->QuadPart = 1000;
    return TRUE;
}

/* ---- Thread-local storage: 64 slots in the TEB, as Windows XP has ------------- */

#define TLS_SLOTS 0xe10u /* TEB.TlsSlots */
#define TLS_SLOT_COUNT 64u

static unsigned long long g_tls_used;

DWORD WINAPI TlsAlloc(void) {
    for (u32 i = 0; i < TLS_SLOT_COUNT; ++i) {
        if ((g_tls_used & (1ull << i)) == 0u) {
            g_tls_used |= 1ull << i;
            teb_write(TLS_SLOTS + 4u * i, 0);
            return i;
        }
    }
    SetLastError(ERROR_NO_MORE_ITEMS);
    return TLS_OUT_OF_INDEXES;
}

BOOL WINAPI TlsFree(DWORD dwTlsIndex) {
    if (dwTlsIndex >= TLS_SLOT_COUNT || (g_tls_used & (1ull << dwTlsIndex)) == 0u) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    g_tls_used &= ~(1ull << dwTlsIndex);
    teb_write(TLS_SLOTS + 4u * dwTlsIndex, 0);
    return TRUE;
}

/* Windows clears the last error on success: a value of 0 is told from a
 * failure by it. */
LPVOID WINAPI TlsGetValue(DWORD dwTlsIndex) {
    if (dwTlsIndex >= TLS_SLOT_COUNT) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return NULL;
    }
    SetLastError(ERROR_SUCCESS);
    return (LPVOID)teb_read(TLS_SLOTS + 4u * dwTlsIndex);
}

BOOL WINAPI TlsSetValue(DWORD dwTlsIndex, LPVOID lpTlsValue) {
    if (dwTlsIndex >= TLS_SLOT_COUNT) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    teb_write(TLS_SLOTS + 4u * dwTlsIndex, (u32)lpTlsValue);
    return TRUE;
}

/* ---- Critical sections ----------------------------------------------------------- */

/* ntdll's, as on Windows XP (RtlEnterCriticalSection and the rest). */
__declspec(dllimport) u32 WINAPI RtlInitializeCriticalSection(LPCRITICAL_SECTION section);
__declspec(dllimport) u32 WINAPI RtlDeleteCriticalSection(LPCRITICAL_SECTION section);
__declspec(dllimport) u32 WINAPI RtlEnterCriticalSection(LPCRITICAL_SECTION section);
__declspec(dllimport) u32 WINAPI RtlLeaveCriticalSection(LPCRITICAL_SECTION section);
__declspec(dllimport) int WINAPI RtlTryEnterCriticalSection(LPCRITICAL_SECTION section);

void WINAPI InitializeCriticalSection(LPCRITICAL_SECTION lpCriticalSection) {
    RtlInitializeCriticalSection(lpCriticalSection);
}

void WINAPI DeleteCriticalSection(LPCRITICAL_SECTION lpCriticalSection) {
    RtlDeleteCriticalSection(lpCriticalSection);
}

void WINAPI EnterCriticalSection(LPCRITICAL_SECTION lpCriticalSection) {
    RtlEnterCriticalSection(lpCriticalSection);
}

void WINAPI LeaveCriticalSection(LPCRITICAL_SECTION lpCriticalSection) {
    RtlLeaveCriticalSection(lpCriticalSection);
}

BOOL WINAPI TryEnterCriticalSection(LPCRITICAL_SECTION lpCriticalSection) {
    return RtlTryEnterCriticalSection(lpCriticalSection);
}

/* ---- Interlocked operations ---------------------------------------------------- */

LONG WINAPI InterlockedIncrement(LPLONG lpAddend) {
    LONG value = 1;
    __asm__ volatile("lock xaddl %0, %1" : "+r"(value), "+m"(*lpAddend) : : "memory");
    return value + 1;
}

LONG WINAPI InterlockedDecrement(LPLONG lpAddend) {
    LONG value = -1;
    __asm__ volatile("lock xaddl %0, %1" : "+r"(value), "+m"(*lpAddend) : : "memory");
    return value - 1;
}

LONG WINAPI InterlockedExchange(LPLONG Target, LONG Value) {
    __asm__ volatile("xchgl %0, %1" : "+r"(Value), "+m"(*Target) : : "memory");
    return Value;
}

LONG WINAPI InterlockedCompareExchange(LPLONG Destination, LONG Exchange, LONG Comparand) {
    __asm__ volatile("lock cmpxchgl %2, %1" : "+a"(Comparand), "+m"(*Destination) : "r"(Exchange) : "memory");
    return Comparand;
}

LONG WINAPI InterlockedExchangeAdd(LPLONG Addend, LONG Value) {
    __asm__ volatile("lock xaddl %0, %1" : "+r"(Value), "+m"(*Addend) : : "memory");
    return Value;
}
