/*
 * kernel32.dll -- What the fixtures, Solitaire and WordPad's start-up use,
 * over a service of the gate (sxw_abi.h) or the state ntdll keeps. The rest
 * of what programs import is declared (declared.txt). memory.c, system.c and
 * strings.c hold the heap's handles, the process-wide state and the strings.
 */
#define SXW_BUILDING_KERNEL32
#include <windows.h>

#include "sxw_abi.h"
#include "codepage.h"
#include "resource.h"

typedef unsigned int u32;
typedef unsigned short u16;

__declspec(dllimport) u32 WINAPI SxwService(u32 service, const u32 *arguments);
__declspec(dllimport) __declspec(noreturn) void WINAPI RtlExitUserProcess(u32 status);
__declspec(dllimport) void WINAPI RtlRaiseException(EXCEPTION_RECORD *record);
__declspec(dllimport) u32 WINAPI SxwLoadLibrary(const char *name, u32 *error);
__declspec(dllimport) u32 WINAPI SxwGetProcAddress(u32 base, const char *name, u32 *error);
__declspec(dllimport) u32 WINAPI SxwFindModule(const char *name);

static void set_last_error(DWORD error) {
    __asm__ volatile("movl %0, %%fs:%c1" : : "r"(error), "i"(SXW_TEB_LAST_ERROR) : "memory");
}

static u32 peb(void) {
    u32 value;
    __asm__ volatile("movl %%fs:%c1, %0" : "=r"(value) : "i"(SXW_TEB_PEB));
    return value;
}

/* ---- Standard handles and output ------------------------------------------- */

HANDLE WINAPI GetStdHandle(DWORD nStdHandle) {
    u32 which;
    switch (nStdHandle) {
    case STD_INPUT_HANDLE: which = 0; break;
    case STD_OUTPUT_HANDLE: which = 1; break;
    case STD_ERROR_HANDLE: which = 2; break;
    default:
        set_last_error(ERROR_INVALID_HANDLE);
        return INVALID_HANDLE_VALUE;
    }
    u32 arguments[1] = {which};
    u32 handle = SxwService(SXW_SVC_STD_HANDLE, arguments);
    return handle == 0 ? NULL : (HANDLE)handle;
}

BOOL WINAPI WriteFile(HANDLE hFile, LPCVOID lpBuffer, DWORD nNumberOfBytesToWrite,
                      LPDWORD lpNumberOfBytesWritten, LPOVERLAPPED lpOverlapped) {
    if (lpNumberOfBytesWritten != NULL) {
        *lpNumberOfBytesWritten = 0;
    }
    if (lpOverlapped != NULL) {
        set_last_error(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    u32 arguments[3] = {(u32)hFile, (u32)lpBuffer, nNumberOfBytesToWrite};
    u32 written = SxwService(SXW_SVC_WRITE, arguments);
    if (written == SXW_FAILED) {
        set_last_error(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    if (lpNumberOfBytesWritten != NULL) {
        *lpNumberOfBytesWritten = written;
    }
    return TRUE;
}

void WINAPI ExitProcess(unsigned int uExitCode) {
    RtlExitUserProcess(uExitCode);
}

void WINAPI RaiseException(DWORD code, DWORD flags, DWORD count, const ULONG_PTR *arguments) {
    EXCEPTION_RECORD record;
    record.ExceptionCode = code & ~0x10000000u;
    record.ExceptionFlags = flags & EXCEPTION_NONCONTINUABLE;
    record.ExceptionRecord = NULL;
    record.ExceptionAddress = NULL;
    if (arguments == NULL) {
        count = 0;
    } else if (count > EXCEPTION_MAXIMUM_PARAMETERS) {
        count = EXCEPTION_MAXIMUM_PARAMETERS;
    }
    record.NumberParameters = count;
    for (DWORD i = 0; i < EXCEPTION_MAXIMUM_PARAMETERS; ++i) {
        record.ExceptionInformation[i] = i < count ? arguments[i] : 0;
    }
    RtlRaiseException(&record);
}

/* ---- The process ------------------------------------------------------------- */

LPWSTR WINAPI GetCommandLineW(void) {
    u32 params = *(u32 *)(peb() + SXW_PEB_PROCESS_PARAMETERS);
    return (LPWSTR) * (u32 *)(params + SXW_PARAMS_COMMAND_LINE + 4u);
}

/* What the program was started with. Nothing asked for a window position,
 * size, title or handles, so only cb is set; dwFlags is 0. */
void WINAPI GetStartupInfoA(LPSTARTUPINFOA lpStartupInfo) {
    unsigned char *p = (unsigned char *)lpStartupInfo;
    for (u32 i = 0; i < sizeof(*lpStartupInfo); ++i) {
        p[i] = 0;
    }
    lpStartupInfo->cb = sizeof(*lpStartupInfo);
}

/* ---- Modules ----------------------------------------------------------------- */

/* A module name as the loader looks it up: ".dll" added when the last
 * component has no extension, a trailing '.' meaning "no extension". 0 when
 * it does not fit. */
static int module_name(const char *name, char *out, u32 capacity) {
    u32 n = 0;
    u32 component = 0;
    while (name[n] != 0) {
        if (n + 5u >= capacity) {
            return 0;
        }
        out[n] = name[n];
        if (name[n] == '\\' || name[n] == '/') {
            component = n + 1u;
        }
        ++n;
    }
    out[n] = 0;
    int has_dot = 0;
    for (u32 i = component; i < n; ++i) {
        has_dot |= out[i] == '.';
    }
    if (n > component && out[n - 1u] == '.') {
        out[n - 1u] = 0;
    } else if (!has_dot) {
        const char *suffix = ".dll";
        for (u32 i = 0; i <= 4u; ++i) {
            out[n + i] = suffix[i];
        }
    }
    return 1;
}

HMODULE WINAPI GetModuleHandleA(LPCSTR lpModuleName) {
    if (lpModuleName == NULL) {
        return (HMODULE) * (u32 *)(peb() + SXW_PEB_IMAGE_BASE);
    }
    char name[260];
    u32 base = module_name(lpModuleName, name, sizeof(name)) ? SxwFindModule(name) : 0u;
    if (base == 0) {
        set_last_error(ERROR_MOD_NOT_FOUND);
    }
    return (HMODULE)base;
}

HMODULE WINAPI LoadLibraryA(LPCSTR lpLibFileName) {
    char name[260];
    if (lpLibFileName == NULL || !module_name(lpLibFileName, name, sizeof(name))) {
        set_last_error(ERROR_INVALID_PARAMETER);
        return NULL;
    }
    u32 error = 0;
    u32 base = SxwLoadLibrary(name, &error);
    if (base == 0) {
        set_last_error(error);
    }
    return (HMODULE)base;
}

FARPROC WINAPI GetProcAddress(HMODULE hModule, LPCSTR lpProcName) {
    u32 base = hModule != NULL ? (u32)hModule : *(u32 *)(peb() + SXW_PEB_IMAGE_BASE);
    u32 error = 0;
    u32 address = SxwGetProcAddress(base, lpProcName, &error);
    if (address == 0) {
        set_last_error(error);
    }
    return (FARPROC)address;
}

/* A wide name in the ANSI code page, for the loader's ANSI services. 0 when
 * it does not fit or a character has no ANSI form. */
static int narrow_name(LPCWSTR wide, char *out, u32 capacity) {
    u32 n = 0;
    for (; wide[n] != 0; ++n) {
        if (n + 1u >= capacity) {
            return 0;
        }
        unsigned char c = sxw_wide_to_ansi(wide[n]);
        if (c == '?' && wide[n] != '?') {
            return 0;
        }
        out[n] = (char)c;
    }
    out[n] = 0;
    return 1;
}

HMODULE WINAPI GetModuleHandleW(LPCWSTR lpModuleName) {
    if (lpModuleName == NULL) {
        return GetModuleHandleA(NULL);
    }
    char name[260];
    if (!narrow_name(lpModuleName, name, sizeof(name))) {
        set_last_error(ERROR_MOD_NOT_FOUND);
        return NULL;
    }
    return GetModuleHandleA(name);
}

HMODULE WINAPI LoadLibraryW(LPCWSTR lpLibFileName) {
    char name[260];
    if (lpLibFileName == NULL || !narrow_name(lpLibFileName, name, sizeof(name))) {
        set_last_error(ERROR_MOD_NOT_FOUND);
        return NULL;
    }
    return LoadLibraryA(name);
}

/* The flags are not honoured: LOAD_LIBRARY_AS_DATAFILE loads the DLL as a
 * module (MFC's resource DLL has no entry point, so nothing of it runs), and
 * there is one search order. */
HMODULE WINAPI LoadLibraryExA(LPCSTR lpLibFileName, HANDLE hFile, DWORD dwFlags) {
    (void)hFile;
    (void)dwFlags;
    return LoadLibraryA(lpLibFileName);
}

HMODULE WINAPI LoadLibraryExW(LPCWSTR lpLibFileName, HANDLE hFile, DWORD dwFlags) {
    (void)hFile;
    (void)dwFlags;
    return LoadLibraryW(lpLibFileName);
}

/* Modules are never unloaded yet: the call succeeds and the DLL stays. */
BOOL WINAPI FreeLibrary(HMODULE hLibModule) {
    if (hLibModule == NULL) {
        set_last_error(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    return TRUE;
}

/* DLL_THREAD_ATTACH and DETACH are not sent yet: there is one thread. */
BOOL WINAPI DisableThreadLibraryCalls(HMODULE hLibModule) {
    (void)hLibModule;
    return TRUE;
}

/* The module's Windows path. A path longer than the buffer is cut to nSize
 * units without a terminator and the last error is left alone, as Windows XP
 * does. */
DWORD WINAPI GetModuleFileNameW(HMODULE hModule, LPWSTR lpFilename, DWORD nSize) {
    WCHAR path[520];
    u32 base = hModule != NULL ? (u32)hModule : *(u32 *)(peb() + SXW_PEB_IMAGE_BASE);
    u32 arguments[3] = {base, (u32)path, sizeof(path) / sizeof(path[0])};
    u32 length = SxwService(SXW_SVC_MODULE_PATH, arguments);
    if (length == 0) {
        set_last_error(ERROR_MOD_NOT_FOUND);
        return 0;
    }
    if (nSize == 0) {
        set_last_error(ERROR_INSUFFICIENT_BUFFER);
        return 0;
    }
    u32 copy = length < nSize ? length + 1u : nSize;
    for (u32 i = 0; i < copy; ++i) {
        lpFilename[i] = path[i];
    }
    if (length >= nSize) {
        return nSize; /* Windows XP leaves the last error as it was */
    }
    return length;
}

DWORD WINAPI GetModuleFileNameA(HMODULE hModule, LPSTR lpFilename, DWORD nSize) {
    WCHAR path[520];
    DWORD length = GetModuleFileNameW(hModule, path, sizeof(path) / sizeof(path[0]));
    if (length == 0) {
        return 0;
    }
    DWORD copy = length < nSize ? length + 1u : nSize;
    for (DWORD i = 0; i < copy; ++i) {
        lpFilename[i] = (char)sxw_wide_to_ansi(path[i]);
    }
    if (length >= nSize) {
        return nSize;
    }
    return length;
}

/* C:\WINDOWS\system32, where the DLLs are (/system/win/drive_c/windows/system32
 * on SavanXP). Returns the length without the terminator, or the size needed
 * with it when the buffer is too small. */
static const char k_system_directory[] = "C:\\WINDOWS\\system32";

UINT WINAPI GetSystemDirectoryA(LPSTR lpBuffer, UINT uSize) {
    const UINT length = sizeof(k_system_directory) - 1u;
    if (lpBuffer == NULL || uSize <= length) {
        return length + 1u;
    }
    for (UINT i = 0; i <= length; ++i) {
        lpBuffer[i] = k_system_directory[i];
    }
    return length;
}

UINT WINAPI GetSystemDirectoryW(LPWSTR lpBuffer, UINT uSize) {
    const UINT length = sizeof(k_system_directory) - 1u;
    if (lpBuffer == NULL || uSize <= length) {
        return length + 1u;
    }
    for (UINT i = 0; i <= length; ++i) {
        lpBuffer[i] = (WCHAR)(unsigned char)k_system_directory[i];
    }
    return length;
}

/* ---- Resources ----------------------------------------------------------------- */

/* An HRSRC is the address of the resource's IMAGE_RESOURCE_DATA_ENTRY. */
HRSRC WINAPI FindResourceExW(HMODULE hModule, LPCWSTR lpType, LPCWSTR lpName, WORD wLanguage) {
    u32 base = hModule != NULL ? (u32)hModule : *(u32 *)(peb() + SXW_PEB_IMAGE_BASE);
    const unsigned char *entry = sxw_resource_entry(base, lpType, lpName, wLanguage == 0 ? SXW_ANY_LANGUAGE : wLanguage);
    if (entry == 0) {
        set_last_error(ERROR_RESOURCE_NAME_NOT_FOUND);
    }
    return (HRSRC)entry;
}

HRSRC WINAPI FindResourceW(HMODULE hModule, LPCWSTR lpName, LPCWSTR lpType) {
    return FindResourceExW(hModule, lpType, lpName, 0);
}

/* The data never moves: the handle is its address in the mapped image. */
HGLOBAL WINAPI LoadResource(HMODULE hModule, HRSRC hResInfo) {
    if (hResInfo == NULL) {
        set_last_error(ERROR_INVALID_HANDLE);
        return NULL;
    }
    u32 base = hModule != NULL ? (u32)hModule : *(u32 *)(peb() + SXW_PEB_IMAGE_BASE);
    return (HGLOBAL)(base + *(const u32 *)hResInfo);
}

LPVOID WINAPI LockResource(HGLOBAL hResData) {
    return hResData;
}

DWORD WINAPI SizeofResource(HMODULE hModule, HRSRC hResInfo) {
    (void)hModule;
    if (hResInfo == NULL) {
        set_last_error(ERROR_INVALID_HANDLE);
        return 0;
    }
    return *((const u32 *)hResInfo + 1);
}

/* Obsolete on Win32: nothing to release, FALSE as Windows XP answers. */
BOOL WINAPI FreeResource(HGLOBAL hResData) {
    (void)hResData;
    return FALSE;
}

/* ---- Strings and arithmetic -------------------------------------------------- */

int WINAPI lstrlenW(LPCWSTR lpString) {
    int n = 0;
    if (lpString == NULL) {
        return 0;
    }
    while (lpString[n] != 0) {
        ++n;
    }
    return n;
}

/* Unsigned 64 by 32 division without the C runtime's __aulldiv. */
static unsigned long long divide(unsigned long long dividend, u32 divisor) {
    unsigned long long quotient = 0;
    unsigned long long remainder = 0;
    for (int bit = 63; bit >= 0; --bit) {
        remainder = (remainder << 1) | ((dividend >> bit) & 1u);
        if (remainder >= divisor) {
            remainder -= divisor;
            quotient |= 1ull << bit;
        }
    }
    return quotient;
}

/* (a * b) / c with a 64-bit product, rounded half away from zero; -1 when c
 * is 0 or the result does not fit, as Windows answers. */
int WINAPI MulDiv(int nNumber, int nNumerator, int nDenominator) {
    if (nDenominator == 0) {
        return -1;
    }
    long long product = (long long)nNumber * nNumerator;
    int negative = (product < 0) != (nDenominator < 0);
    unsigned long long magnitude = product < 0 ? 0ull - (unsigned long long)product : (unsigned long long)product;
    u32 divisor = nDenominator < 0 ? 0u - (u32)nDenominator : (u32)nDenominator;
    unsigned long long result = divide(magnitude + divisor / 2u, divisor);
    if (result > 2147483647ull) {
        return -1;
    }
    return negative ? -(int)result : (int)result;
}
