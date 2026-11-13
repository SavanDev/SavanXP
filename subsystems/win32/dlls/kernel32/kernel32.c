/*
 * kernel32.dll -- Only what the fixtures and the W0 matrix import, over a
 * service of the gate (sxw_abi.h) or the state ntdll keeps.
 */
#define SXW_BUILDING_KERNEL32
#include <windows.h>

#include "sxw_abi.h"

typedef unsigned int u32;
typedef unsigned short u16;

__declspec(dllimport) u32 WINAPI SxwService(u32 service, const u32 *arguments);
__declspec(dllimport) __declspec(noreturn) void WINAPI RtlExitUserProcess(u32 status);
__declspec(dllimport) u32 WINAPI SxwLoadLibrary(const char *name, u32 *error);
__declspec(dllimport) u32 WINAPI SxwGetProcAddress(u32 base, const char *name, u32 *error);
__declspec(dllimport) u32 WINAPI SxwFindModule(const char *name);
__declspec(dllimport) void *WINAPI SxwHeapAllocate(u32 size, u32 zero);
__declspec(dllimport) int WINAPI SxwHeapFree(void *pointer);

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

/* ---- Memory ------------------------------------------------------------------ */

/* Fixed blocks only: what the profile allocates is LPTR. A moveable block is
 * refused rather than handed out as if it were fixed. */
HLOCAL WINAPI LocalAlloc(UINT uFlags, SIZE_T uBytes) {
    if ((uFlags & ~(UINT)(LMEM_ZEROINIT | LMEM_NOCOMPACT | LMEM_NODISCARD)) != 0u) {
        set_last_error(ERROR_INVALID_PARAMETER);
        return NULL;
    }
    void *block = SxwHeapAllocate(uBytes, (uFlags & LMEM_ZEROINIT) != 0u);
    if (block == NULL) {
        set_last_error(ERROR_NOT_ENOUGH_MEMORY);
    }
    return block;
}

HLOCAL WINAPI LocalFree(HLOCAL hMem) {
    if (hMem == NULL || SxwHeapFree(hMem)) {
        return NULL;
    }
    set_last_error(ERROR_INVALID_HANDLE);
    return hMem;
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
