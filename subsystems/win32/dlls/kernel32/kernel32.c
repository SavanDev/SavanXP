/*
 * kernel32.dll -- Only what the fixtures and the W0 matrix import, each
 * function over a service of the gate (sxw_abi.h).
 */
#define SXW_BUILDING_KERNEL32
#include <windows.h>

#include "sxw_abi.h"

typedef unsigned int u32;

__declspec(dllimport) u32 WINAPI SxwService(u32 service, const u32 *arguments);
__declspec(dllimport) __declspec(noreturn) void WINAPI RtlExitUserProcess(u32 status);

static void set_last_error(DWORD error) {
    __asm__ volatile("movl %0, %%fs:%c1" : : "r"(error), "i"(SXW_TEB_LAST_ERROR) : "memory");
}

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
