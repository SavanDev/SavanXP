/*
 * thread.c -- kernel32's threads and the objects they wait on, over the
 * host's services (sxw_abi.h, SXW_SVC_THREAD_CREATE to SXW_SVC_SLEEP).
 * Guest threads take turns (docs/userland/WIN32.md, "Threads"): one runs
 * guest code at a time, and a wait or a sleep lets another run.
 */
#define SXW_BUILDING_KERNEL32
#include <windows.h>

#include "sxw_abi.h"

typedef unsigned int u32;
typedef DWORD(WINAPI *thread_start)(LPVOID parameter);

__declspec(dllimport) u32 WINAPI SxwService(u32 service, const u32 *arguments);
__declspec(dllimport) __declspec(noreturn) void WINAPI SxwExitThread(u32 code);

#define CREATE_SUSPENDED 0x4u
#define THREAD_PRIORITY_ERROR_RETURN 0x7fffffff
#define DUPLICATE_CLOSE_SOURCE 0x1u
#define ERROR_ALREADY_EXISTS_CODE 183u

static void set_last_error(DWORD error) {
    __asm__ volatile("movl %0, %%fs:%c1" : : "r"(error), "i"(SXW_TEB_LAST_ERROR) : "memory");
}

static BOOL done(u32 error) {
    if (error != 0u) {
        set_last_error(error);
        return FALSE;
    }
    return TRUE;
}

HANDLE WINAPI CreateThread(LPSECURITY_ATTRIBUTES lpThreadAttributes, SIZE_T dwStackSize,
                           thread_start lpStartAddress, LPVOID lpParameter, DWORD dwCreationFlags,
                           LPDWORD lpThreadId) {
    (void)lpThreadAttributes;
    u32 out[2] = {0, 0};
    u32 arguments[5] = {(u32)lpStartAddress, (u32)lpParameter, dwStackSize, dwCreationFlags & CREATE_SUSPENDED,
                        (u32)out};
    if (!done(SxwService(SXW_SVC_THREAD_CREATE, arguments))) {
        return NULL;
    }
    if (lpThreadId != NULL) {
        *lpThreadId = out[1];
    }
    return (HANDLE)out[0];
}

void WINAPI ExitThread(DWORD dwExitCode) {
    SxwExitThread(dwExitCode);
}

static DWORD suspend(HANDLE thread, u32 resume) {
    u32 previous = 0;
    u32 arguments[3] = {(u32)thread, resume, (u32)&previous};
    return done(SxwService(SXW_SVC_THREAD_SUSPEND, arguments)) ? previous : (DWORD)-1;
}

DWORD WINAPI SuspendThread(HANDLE hThread) {
    return suspend(hThread, 0);
}

DWORD WINAPI ResumeThread(HANDLE hThread) {
    return suspend(hThread, 1);
}

static BOOL query(HANDLE thread, u32 out[3]) {
    u32 arguments[2] = {(u32)thread, (u32)out};
    return done(SxwService(SXW_SVC_THREAD_QUERY, arguments));
}

BOOL WINAPI GetExitCodeThread(HANDLE hThread, LPDWORD lpExitCode) {
    u32 out[3];
    if (!query(hThread, out)) {
        return FALSE;
    }
    *lpExitCode = out[0];
    return TRUE;
}

int WINAPI GetThreadPriority(HANDLE hThread) {
    u32 out[3];
    return query(hThread, out) ? (int)out[1] : THREAD_PRIORITY_ERROR_RETURN;
}

BOOL WINAPI SetThreadPriority(HANDLE hThread, int nPriority) {
    u32 arguments[2] = {(u32)hThread, (u32)nPriority};
    return done(SxwService(SXW_SVC_THREAD_PRIORITY, arguments));
}

/* A new or named object: its handle, with ERROR_ALREADY_EXISTS for a name
 * that was there. */
static HANDLE create(u32 kind, u32 a, u32 b, LPCWSTR name) {
    u32 out[2] = {0, 0};
    u32 arguments[5] = {kind, a, b, (u32)name, (u32)out};
    if (!done(SxwService(SXW_SVC_OBJECT_CREATE, arguments))) {
        return NULL;
    }
    set_last_error(out[1] != 0u ? ERROR_ALREADY_EXISTS_CODE : 0u);
    return (HANDLE)out[0];
}

HANDLE WINAPI CreateEventW(LPSECURITY_ATTRIBUTES lpEventAttributes, BOOL bManualReset, BOOL bInitialState,
                           LPCWSTR lpName) {
    (void)lpEventAttributes;
    return create(SXW_OBJECT_EVENT, bManualReset != 0, bInitialState != 0, lpName);
}

HANDLE WINAPI CreateMutexW(LPSECURITY_ATTRIBUTES lpMutexAttributes, BOOL bInitialOwner, LPCWSTR lpName) {
    (void)lpMutexAttributes;
    return create(SXW_OBJECT_MUTEX, bInitialOwner != 0, 0, lpName);
}

HANDLE WINAPI CreateSemaphoreW(LPSECURITY_ATTRIBUTES lpSemaphoreAttributes, LONG lInitialCount, LONG lMaximumCount,
                               LPCWSTR lpName) {
    (void)lpSemaphoreAttributes;
    return create(SXW_OBJECT_SEMAPHORE, (u32)lInitialCount, (u32)lMaximumCount, lpName);
}

static BOOL signal(HANDLE object, u32 operation, u32 count, LPLONG previous) {
    u32 arguments[4] = {(u32)object, operation, count, (u32)previous};
    return done(SxwService(SXW_SVC_OBJECT_SIGNAL, arguments));
}

BOOL WINAPI SetEvent(HANDLE hEvent) {
    return signal(hEvent, SXW_SIGNAL_SET, 0, NULL);
}

BOOL WINAPI ResetEvent(HANDLE hEvent) {
    return signal(hEvent, SXW_SIGNAL_RESET, 0, NULL);
}

BOOL WINAPI ReleaseMutex(HANDLE hMutex) {
    return signal(hMutex, SXW_SIGNAL_RELEASE, 1, NULL);
}

BOOL WINAPI ReleaseSemaphore(HANDLE hSemaphore, LONG lReleaseCount, LPLONG lpPreviousCount) {
    return signal(hSemaphore, SXW_SIGNAL_RELEASE, (u32)lReleaseCount, lpPreviousCount);
}

DWORD WINAPI WaitForMultipleObjects(DWORD nCount, const HANDLE *lpHandles, BOOL bWaitAll, DWORD dwMilliseconds) {
    u32 result = 0;
    u32 arguments[5] = {(u32)lpHandles, nCount, bWaitAll != 0, dwMilliseconds, (u32)&result};
    return done(SxwService(SXW_SVC_WAIT, arguments)) ? result : WAIT_FAILED;
}

DWORD WINAPI WaitForSingleObject(HANDLE hHandle, DWORD dwMilliseconds) {
    return WaitForMultipleObjects(1, &hHandle, FALSE, dwMilliseconds);
}

void WINAPI Sleep(DWORD dwMilliseconds) {
    u32 arguments[1] = {dwMilliseconds};
    SxwService(SXW_SVC_SLEEP, arguments);
}

/* Within this process only: both process handles must be its own. */
BOOL WINAPI DuplicateHandle(HANDLE hSourceProcessHandle, HANDLE hSourceHandle, HANDLE hTargetProcessHandle,
                            LPHANDLE lpTargetHandle, DWORD dwDesiredAccess, BOOL bInheritHandle, DWORD dwOptions) {
    (void)dwDesiredAccess;
    (void)bInheritHandle;
    if ((u32)hSourceProcessHandle != SXW_CURRENT_PROCESS || (u32)hTargetProcessHandle != SXW_CURRENT_PROCESS ||
        lpTargetHandle == NULL) {
        return done(ERROR_INVALID_PARAMETER);
    }
    u32 copy = 0;
    u32 arguments[2] = {(u32)hSourceHandle, (u32)&copy};
    if (!done(SxwService(SXW_SVC_DUPLICATE, arguments))) {
        return FALSE;
    }
    *lpTargetHandle = (HANDLE)copy;
    if ((dwOptions & DUPLICATE_CLOSE_SOURCE) != 0u) {
        u32 close[1] = {(u32)hSourceHandle};
        SxwService(SXW_SVC_CLOSE, close);
    }
    return TRUE;
}
