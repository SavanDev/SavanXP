/*
 * drop.c -- File lists (docs/userland/WIN32.md, "Drag and drop"): the
 * DROPFILES block a WM_DROPFILES or the clipboard's CF_HDROP carries, read
 * with DragQueryFileW and DragQueryPoint and freed with DragFinish, and
 * DragAcceptFiles, which says a window takes dropped files. The block's
 * names may be wide or ANSI (Windows-1252), as Windows XP reads both
 * (fixtures/clip).
 */
#define SXW_BUILDING_SHELL32
#include <windows.h>

/* The name at index into out (capacity units), NUL-terminated and cut to
 * fit; its whole length, cut or not, as Windows XP answers (fixtures/clip).
 * A NULL out asks only for the length. -1 when there is no such name. */
static int name_at(const unsigned char *block, SIZE_T size, UINT index, WCHAR *out, UINT capacity, UINT *count) {
    const DROPFILES *d = (const DROPFILES *)block;
    SIZE_T at = d->pFiles;
    UINT i = 0;
    while (at < size) {
        UINT length = 0;
        if (d->fWide) {
            const WCHAR *w = (const WCHAR *)(block + at);
            while (at + 2u * (length + 1u) <= size && w[length] != 0) {
                ++length;
            }
            if (length == 0) {
                break;
            }
            if (i == index && out != NULL && capacity > 0) {
                const UINT n = length < capacity - 1u ? length : capacity - 1u;
                for (UINT k = 0; k < n; ++k) {
                    out[k] = w[k];
                }
                out[n] = 0;
                return (int)length;
            }
            at += 2u * (length + 1u);
        } else {
            const char *c = (const char *)(block + at);
            while (at + length + 1u <= size && c[length] != 0) {
                ++length;
            }
            if (length == 0) {
                break;
            }
            if (i == index && out != NULL && capacity > 0) {
                WCHAR wide[MAX_PATH];
                const int units = MultiByteToWideChar(CP_ACP, 0, c, (int)length, wide, MAX_PATH);
                const UINT n = (UINT)units < capacity - 1u ? (UINT)units : capacity - 1u;
                for (UINT k = 0; k < n; ++k) {
                    out[k] = wide[k];
                }
                out[n] = 0;
                return units;
            }
            at += length + 1u;
        }
        if (i == index) {
            return (int)length;
        }
        ++i;
    }
    *count = i;
    return -1;
}

UINT WINAPI DragQueryFileW(HDROP hDrop, UINT iFile, LPWSTR lpszFile, UINT cch) {
    const unsigned char *block = GlobalLock(hDrop);
    if (block == NULL) {
        return 0;
    }
    const SIZE_T size = GlobalSize(hDrop);
    UINT count = 0;
    int result;
    if (iFile == 0xffffffffu) {
        name_at(block, size, 0xffffffffu, NULL, 0, &count);
        result = (int)count;
    } else {
        result = name_at(block, size, iFile, lpszFile, cch, &count);
        if (result < 0) {
            result = 0;
        }
    }
    GlobalUnlock(hDrop);
    return (UINT)result;
}

/* Where the files were dropped, in the window's client coordinates; TRUE
 * when that is inside its client area. */
BOOL WINAPI DragQueryPoint(HDROP hDrop, LPPOINT ppt) {
    const DROPFILES *d = GlobalLock(hDrop);
    if (d == NULL) {
        return FALSE;
    }
    if (ppt != NULL) {
        *ppt = d->pt;
    }
    const BOOL client = !d->fNC;
    GlobalUnlock(hDrop);
    return client;
}

void WINAPI DragFinish(HDROP hDrop) {
    GlobalFree(hDrop);
}

void WINAPI DragAcceptFiles(HWND hWnd, BOOL fAccept) {
    const LONG ex = GetWindowLongW(hWnd, GWL_EXSTYLE);
    const LONG wanted = fAccept ? ex | WS_EX_ACCEPTFILES : ex & ~WS_EX_ACCEPTFILES;
    if (wanted != ex) {
        SetWindowLongW(hWnd, GWL_EXSTYLE, wanted);
    }
}
