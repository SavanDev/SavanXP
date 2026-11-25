/*
 * memory.c -- kernel32's global and local memory: fixed blocks straight from
 * ntdll's heap, and moveable blocks behind a handle.
 *
 * A moveable handle is the address of an entry in a table here whose first
 * word is the block's address, as on Windows, where *(void **)handle is the
 * data of a moveable block. The block carries an 8-byte header (a tag and its
 * handle) so GlobalHandle can find the handle from the data. A block may move
 * on GlobalReAlloc; its handle never does. A moveable block of 0 bytes is a
 * discarded one: a handle with no data.
 *
 * Local and global memory are the same heap on Win32, and their flags agree
 * in every bit this file reads (MOVEABLE, ZEROINIT, MODIFY), so the Local
 * functions are the Global ones. One thread: no lock until there are threads.
 */
#define SXW_BUILDING_KERNEL32
#include <windows.h>

typedef unsigned int u32;
typedef unsigned short u16;

__declspec(dllimport) void *WINAPI SxwHeapAllocate(u32 size, u32 zero);
__declspec(dllimport) int WINAPI SxwHeapFree(void *pointer);
__declspec(dllimport) u32 WINAPI SxwHeapSize(const void *pointer);
__declspec(dllimport) int WINAPI SxwHeapResize(void *pointer, u32 size);
__declspec(dllimport) int WINAPI SxwHeapIsBlock(const void *pointer);

#define GMEM_DISCARDED 0x4000u
#define MOVEABLE_TAG 0x4d575853u /* "SXWM" */
#define HANDLES 8192u
#define ENTRY_USED 0x0001u
#define ENTRY_DISCARDABLE 0x0002u
#define ENTRY_DDESHARE 0x0004u

typedef struct handle_entry {
    void *data;  /* first, as Windows' handle entry: *(void **)handle */
    u16 locks;
    u16 flags;
} handle_entry;

static handle_entry g_handles[HANDLES];
static u32 g_next_handle;

static handle_entry *entry_of(HGLOBAL handle) {
    u32 address = (u32)handle;
    u32 first = (u32)&g_handles[0];
    if (address < first || address >= first + sizeof(g_handles) || (address - first) % sizeof(handle_entry) != 0u) {
        return 0;
    }
    handle_entry *entry = (handle_entry *)address;
    return (entry->flags & ENTRY_USED) != 0u ? entry : 0;
}

/* The data of a moveable block, after its header; 0 on failure. */
static void *allocate_moveable_data(handle_entry *entry, u32 size, int zero) {
    u32 *header = SxwHeapAllocate(size + 8u, (u32)zero);
    if (header == 0) {
        return 0;
    }
    header[0] = MOVEABLE_TAG;
    header[1] = (u32)entry;
    return header + 2;
}

static void free_moveable_data(void *data) {
    if (data != 0) {
        SxwHeapFree((u32 *)data - 2);
    }
}

static void set_error(DWORD error) {
    SetLastError(error);
}

HGLOBAL WINAPI GlobalAlloc(UINT uFlags, SIZE_T dwBytes) {
    const int zero = (uFlags & GMEM_ZEROINIT) != 0u;
    if ((uFlags & GMEM_MOVEABLE) == 0u) {
        void *block = SxwHeapAllocate(dwBytes, (u32)zero);
        if (block == 0) {
            set_error(ERROR_NOT_ENOUGH_MEMORY);
        }
        return block;
    }
    handle_entry *entry = 0;
    for (u32 i = 0; i < HANDLES; ++i) {
        u32 at = (g_next_handle + i) % HANDLES;
        if ((g_handles[at].flags & ENTRY_USED) == 0u) {
            entry = &g_handles[at];
            g_next_handle = at + 1u;
            break;
        }
    }
    if (entry == 0) {
        set_error(ERROR_NOT_ENOUGH_MEMORY);
        return 0;
    }
    void *data = 0;
    if (dwBytes != 0u) {
        data = allocate_moveable_data(entry, dwBytes, zero);
        if (data == 0) {
            set_error(ERROR_NOT_ENOUGH_MEMORY);
            return 0;
        }
    }
    entry->data = data;
    entry->locks = 0;
    entry->flags = (u16)(ENTRY_USED | ((uFlags & GMEM_DISCARDABLE) != 0u ? ENTRY_DISCARDABLE : 0u) |
                         ((uFlags & GMEM_DDESHARE) != 0u ? ENTRY_DDESHARE : 0u));
    return (HGLOBAL)entry;
}

/* A fixed block's handle is its address. */
static int is_fixed(HGLOBAL handle) {
    return handle != 0 && SxwHeapIsBlock(handle);
}

LPVOID WINAPI GlobalLock(HGLOBAL hMem) {
    handle_entry *entry = entry_of(hMem);
    if (entry != 0) {
        if (entry->data != 0 && entry->locks < 0xffu) {
            ++entry->locks;
        }
        return entry->data;
    }
    if (is_fixed(hMem)) {
        return hMem;
    }
    set_error(ERROR_INVALID_HANDLE);
    return 0;
}

BOOL WINAPI GlobalUnlock(HGLOBAL hMem) {
    handle_entry *entry = entry_of(hMem);
    if (entry != 0) {
        if (entry->locks == 0u) {
            set_error(ERROR_NOT_LOCKED);
            return FALSE;
        }
        if (entry->locks < 0xffu) {
            --entry->locks;
        }
        if (entry->locks == 0u) {
            set_error(ERROR_SUCCESS);
            return FALSE;
        }
        return TRUE;
    }
    return TRUE; /* a fixed block is never locked */
}

SIZE_T WINAPI GlobalSize(HGLOBAL hMem) {
    handle_entry *entry = entry_of(hMem);
    if (entry != 0) {
        return entry->data != 0 ? SxwHeapSize((u32 *)entry->data - 2) - 8u : 0u;
    }
    if (is_fixed(hMem)) {
        return SxwHeapSize(hMem);
    }
    set_error(ERROR_INVALID_HANDLE);
    return 0;
}

UINT WINAPI GlobalFlags(HGLOBAL hMem) {
    handle_entry *entry = entry_of(hMem);
    if (entry != 0) {
        return entry->locks | (entry->data == 0 ? GMEM_DISCARDED : 0u) |
               ((entry->flags & ENTRY_DISCARDABLE) != 0u ? GMEM_DISCARDABLE : 0u) |
               ((entry->flags & ENTRY_DDESHARE) != 0u ? GMEM_DDESHARE : 0u);
    }
    if (is_fixed(hMem)) {
        return 0;
    }
    set_error(ERROR_INVALID_HANDLE);
    return GMEM_INVALID_HANDLE;
}

HGLOBAL WINAPI GlobalHandle(LPCVOID pMem) {
    if (entry_of((HGLOBAL)pMem) != 0) {
        return (HGLOBAL)pMem;
    }
    if (pMem != 0 && ((u32)pMem & 7u) == 0u) {
        const u32 *header = (const u32 *)pMem - 2;
        if (SxwHeapIsBlock(header) && header[0] == MOVEABLE_TAG) {
            handle_entry *entry = entry_of((HGLOBAL)header[1]);
            if (entry != 0 && entry->data == pMem) {
                return (HGLOBAL)entry;
            }
        }
        if (SxwHeapIsBlock(pMem)) {
            return (HGLOBAL)pMem;
        }
    }
    set_error(ERROR_INVALID_HANDLE);
    return 0;
}

HGLOBAL WINAPI GlobalFree(HGLOBAL hMem) {
    handle_entry *entry = entry_of(hMem);
    if (entry != 0) {
        free_moveable_data(entry->data);
        entry->data = 0;
        entry->flags = 0;
        entry->locks = 0;
        return 0;
    }
    if (hMem == 0 || SxwHeapFree(hMem)) {
        return 0;
    }
    set_error(ERROR_INVALID_HANDLE);
    return hMem;
}

static void copy_bytes(void *to, const void *from, u32 count) {
    unsigned char *t = to;
    const unsigned char *f = from;
    for (u32 i = 0; i < count; ++i) {
        t[i] = f[i];
    }
}

static void zero_bytes(void *to, u32 count) {
    unsigned char *t = to;
    for (u32 i = 0; i < count; ++i) {
        t[i] = 0;
    }
}

HGLOBAL WINAPI GlobalReAlloc(HGLOBAL hMem, SIZE_T dwBytes, UINT uFlags) {
    const int zero = (uFlags & GMEM_ZEROINIT) != 0u;
    handle_entry *entry = entry_of(hMem);
    if (entry != 0) {
        if ((uFlags & GMEM_MODIFY) != 0u) {
            entry->flags = (u16)((entry->flags & ~ENTRY_DISCARDABLE) |
                                 ((uFlags & GMEM_DISCARDABLE) != 0u ? ENTRY_DISCARDABLE : 0u));
            return hMem;
        }
        u32 old_size = entry->data != 0 ? SxwHeapSize((u32 *)entry->data - 2) - 8u : 0u;
        void *data = 0;
        if (dwBytes != 0u) {
            data = allocate_moveable_data(entry, dwBytes, 0);
            if (data == 0) {
                set_error(ERROR_NOT_ENOUGH_MEMORY);
                return 0;
            }
            u32 keep = old_size < dwBytes ? old_size : dwBytes;
            if (keep != 0u) {
                copy_bytes(data, entry->data, keep);
            }
            if (dwBytes > keep) {
                if (zero) {
                    zero_bytes((unsigned char *)data + keep, dwBytes - keep);
                }
            }
        }
        free_moveable_data(entry->data);
        entry->data = data;
        if (data == 0) {
            entry->locks = 0;
        }
        return hMem;
    }
    if (!is_fixed(hMem)) {
        set_error(ERROR_INVALID_HANDLE);
        return 0;
    }
    if ((uFlags & GMEM_MODIFY) != 0u) {
        return hMem;
    }
    u32 old_size = SxwHeapSize(hMem);
    if (SxwHeapResize(hMem, dwBytes)) {
        if (zero && dwBytes > old_size) {
            zero_bytes((unsigned char *)hMem + old_size, dwBytes - old_size);
        }
        return hMem; /* within the space the block has */
    }
    if ((uFlags & GMEM_MOVEABLE) == 0u) {
        set_error(ERROR_NOT_ENOUGH_MEMORY); /* a fixed block grows only by moving */
        return 0;
    }
    void *block = SxwHeapAllocate(dwBytes, 0);
    if (block == 0) {
        set_error(ERROR_NOT_ENOUGH_MEMORY);
        return 0;
    }
    copy_bytes(block, hMem, old_size);
    if (zero) {
        zero_bytes((unsigned char *)block + old_size, dwBytes - old_size);
    }
    SxwHeapFree(hMem);
    return block;
}

/* ---- Local memory: the same heap and the same handles ----------------------- */

HLOCAL WINAPI LocalAlloc(UINT uFlags, SIZE_T uBytes) {
    return GlobalAlloc(uFlags & (LMEM_MOVEABLE | LMEM_ZEROINIT), uBytes);
}

HLOCAL WINAPI LocalReAlloc(HLOCAL hMem, SIZE_T uBytes, UINT uFlags) {
    return GlobalReAlloc(hMem, uBytes, uFlags & (LMEM_MOVEABLE | LMEM_ZEROINIT | LMEM_MODIFY));
}

LPVOID WINAPI LocalLock(HLOCAL hMem) {
    return GlobalLock(hMem);
}

BOOL WINAPI LocalUnlock(HLOCAL hMem) {
    return GlobalUnlock(hMem);
}

HLOCAL WINAPI LocalFree(HLOCAL hMem) {
    return GlobalFree(hMem);
}

/* ---- Heaps: ntdll's (RtlCreateHeap and the rest), as on Windows ---------------- */

__declspec(dllimport) void *WINAPI RtlCreateHeap(u32 flags, void *base, u32 reserve, u32 commit, void *lock,
                                                 void *parameters);
__declspec(dllimport) void *WINAPI RtlDestroyHeap(void *heap);
__declspec(dllimport) void *WINAPI RtlAllocateHeap(void *heap, u32 flags, u32 size);
__declspec(dllimport) int WINAPI RtlFreeHeap(void *heap, u32 flags, void *pointer);
__declspec(dllimport) u32 WINAPI RtlSizeHeap(void *heap, u32 flags, const void *pointer);
__declspec(dllimport) void *WINAPI RtlReAllocateHeap(void *heap, u32 flags, void *pointer, u32 size);
__declspec(dllimport) void *WINAPI SxwProcessHeap(void);

HANDLE WINAPI GetProcessHeap(void) {
    return SxwProcessHeap();
}

/* A maximum size makes a heap that holds no more than that; 0 one that
 * grows. */
HANDLE WINAPI HeapCreate(DWORD flOptions, SIZE_T dwInitialSize, SIZE_T dwMaximumSize) {
    HANDLE heap = RtlCreateHeap(flOptions | (dwMaximumSize == 0u ? 0x2u : 0u), NULL, dwMaximumSize, dwInitialSize,
                                NULL, NULL);
    if (heap == NULL) {
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
    }
    return heap;
}

BOOL WINAPI HeapDestroy(HANDLE hHeap) {
    if (RtlDestroyHeap(hHeap) != NULL) {
        SetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    return TRUE;
}

/* As Windows XP's, only a failed HeapAlloc sets the last error. */
LPVOID WINAPI HeapAlloc(HANDLE hHeap, DWORD dwFlags, SIZE_T dwBytes) {
    return RtlAllocateHeap(hHeap, dwFlags, dwBytes);
}

BOOL WINAPI HeapFree(HANDLE hHeap, DWORD dwFlags, LPVOID lpMem) {
    return RtlFreeHeap(hHeap, dwFlags, lpMem);
}

SIZE_T WINAPI HeapSize(HANDLE hHeap, DWORD dwFlags, LPCVOID lpMem) {
    return RtlSizeHeap(hHeap, dwFlags, lpMem);
}

LPVOID WINAPI HeapReAlloc(HANDLE hHeap, DWORD dwFlags, LPVOID lpMem, SIZE_T dwBytes) {
    return RtlReAllocateHeap(hHeap, dwFlags, lpMem, dwBytes);
}
