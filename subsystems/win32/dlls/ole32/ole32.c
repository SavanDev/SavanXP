/*
 * ole32.dll -- The minimum WordPad and MFC need from OLE
 * (WIN32_WORDPAD_ROADMAP.md, "OLE is the minimum WordPad and MFC need"):
 * initialization succeeds, a message filter is accepted and none is kept,
 * and task memory is the process heap. Everything else WordPad imports is
 * declared (declared.txt) and fails with E_NOTIMPL.
 */
#include <windows.h>

typedef unsigned int u32;
typedef long HRESULT;

__declspec(dllimport) void *WINAPI SxwHeapAllocate(u32 size, u32 zero);
__declspec(dllimport) int WINAPI SxwHeapFree(void *pointer);

#define S_OK 0L
#define S_FALSE 1L

static u32 g_initialized;

/* S_OK the first time, S_FALSE after, as Windows answers a repeated call. */
HRESULT WINAPI OleInitialize(LPVOID pvReserved) {
    (void)pvReserved;
    return g_initialized++ == 0u ? S_OK : S_FALSE;
}

void WINAPI OleUninitialize(void) {
    if (g_initialized != 0u) {
        --g_initialized;
    }
}

/* No message filter is ever called: there are no calls between apartments. */
HRESULT WINAPI CoRegisterMessageFilter(LPVOID lpMessageFilter, LPVOID *lplpMessageFilter) {
    (void)lpMessageFilter;
    if (lplpMessageFilter != NULL) {
        *lplpMessageFilter = NULL;
    }
    return S_OK;
}

LPVOID WINAPI CoTaskMemAlloc(SIZE_T cb) {
    return SxwHeapAllocate(cb, 0);
}

void WINAPI CoTaskMemFree(LPVOID pv) {
    if (pv != NULL) {
        SxwHeapFree(pv);
    }
}
