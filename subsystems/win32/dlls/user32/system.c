/*
 * system.c -- user32's system queries and small helpers: the system metrics,
 * the system colour brushes, clipboard format names, finding a window, the
 * active window, the pointer's position and the rectangle helpers WordPad's
 * start-up uses.
 */
#include "user32_private.h"

#include "sxw_abi.h"

__declspec(dllimport) u32 WINAPI SxwService(u32 service, const u32 *arguments);

/* ---- System metrics: Windows XP's classic style at 96 dpi -------------------- */

static void screen_size(int *width, int *height) {
    u32 size[2] = {0, 0};
    u32 arguments[1] = {(u32)size};
    SxwService(SXW_SVC_DISPLAY, arguments);
    *width = (int)size[0];
    *height = (int)size[1];
}

int WINAPI GetSystemMetrics(int nIndex) {
    int width, height;
    switch (nIndex) {
    case 0:  /* SM_CXSCREEN */
    case 16: /* SM_CXFULLSCREEN */
    case 61: /* SM_CXMAXIMIZED: no taskbar subtracted, the host decides */
    case 78: /* SM_CXVIRTUALSCREEN */
        screen_size(&width, &height);
        return width;
    case 1:  /* SM_CYSCREEN */
    case 17: /* SM_CYFULLSCREEN */
    case 62: /* SM_CYMAXIMIZED */
    case 79: /* SM_CYVIRTUALSCREEN */
        screen_size(&width, &height);
        return height;
    case 2:  /* SM_CXVSCROLL */
    case 3:  /* SM_CYHSCROLL */
    case 9:  /* SM_CYVTHUMB */
    case 10: /* SM_CXHTHUMB */
    case 20: /* SM_CYVSCROLL */
    case 21: /* SM_CXHSCROLL */
    case 49: /* SM_CXSMICON */
    case 50: /* SM_CYSMICON */
        return 16;
    case 4:  /* SM_CYCAPTION */
    case 30: /* SM_CXSIZE */
    case 31: /* SM_CYSIZE */
    case 54: /* SM_CXMENUSIZE */
    case 55: /* SM_CYMENUSIZE */
        return 18;
    case 5:  /* SM_CXBORDER */
    case 6:  /* SM_CYBORDER */
    case 19: /* SM_MOUSEPRESENT */
    case 80: /* SM_CMONITORS */
    case 81: /* SM_SAMEDISPLAYFORMAT */
        return 1;
    case 7:  /* SM_CXDLGFRAME */
    case 8:  /* SM_CYDLGFRAME */
        return 3;
    case 11: /* SM_CXICON */
    case 12: /* SM_CYICON */
    case 13: /* SM_CXCURSOR */
    case 14: /* SM_CYCURSOR */
        return 32;
    case 15: /* SM_CYMENU */
        return MENU_BAR_HEIGHT - 1;
    case 28: /* SM_CXMIN */
    case 34: /* SM_CXMINTRACK */
        return 112;
    case 29: /* SM_CYMIN */
    case 35: /* SM_CYMINTRACK */
        return 27;
    case 32: /* SM_CXFRAME */
    case 33: /* SM_CYFRAME */
        return 4;
    case 36: /* SM_CXDOUBLECLK */
    case 37: /* SM_CYDOUBLECLK */
        return 4;
    case 38: /* SM_CXICONSPACING */
    case 39: /* SM_CYICONSPACING */
        return 75;
    case 43: /* SM_CMOUSEBUTTONS */
        return 3;
    case 45: /* SM_CXEDGE */
    case 46: /* SM_CYEDGE */
        return 2;
    case 51: /* SM_CYSMCAPTION */
        return 16;
    case 71: /* SM_CXMENUCHECK */
    case 72: /* SM_CYMENUCHECK */
        return 13;
    default:
        /* SM_SWAPBUTTON, SM_PENWINDOWS, SM_DBCSENABLED, SM_DEBUG,
         * SM_REMOTESESSION, the virtual screen's origin and the rest: 0. */
        return 0;
    }
}

HBRUSH WINAPI GetSysColorBrush(int nIndex) {
    return system_brush(nIndex);
}

/* ---- Clipboard format names ------------------------------------------------------- */

/* Windows takes both from one atom table: the same name gives the same
 * number from either call. */
UINT WINAPI RegisterClipboardFormatW(LPCWSTR lpszFormat) {
    return RegisterWindowMessageW(lpszFormat);
}

/* ---- Windows ------------------------------------------------------------------------ */

/* A top-level window by class name and title, either one NULL for any. A
 * class given as an atom is compared with the class's atom. */
HWND WINAPI FindWindowW(LPCWSTR lpClassName, LPCWSTR lpWindowName) {
    for (int i = 0; i < MAX_WINDOWS; ++i) {
        window *w = &g_windows[i];
        if (!w->used || w->parent != 0 || w->destroying) {
            continue;
        }
        if (lpClassName != NULL) {
            if ((u32)lpClassName < 0x10000u ? w->cls->atom != (ATOM)(u32)lpClassName
                                             : !same_name(w->cls->name, lpClassName)) {
                continue;
            }
        }
        if (lpWindowName != NULL) {
            const WCHAR *a = w->text;
            const WCHAR *b = lpWindowName;
            while (*a != 0 && *a == *b) {
                ++a;
                ++b;
            }
            if (*a != *b) {
                continue;
            }
        }
        return handle_of(w);
    }
    return NULL;
}

HWND WINAPI GetActiveWindow(void) {
    return g_active != 0 ? handle_of(g_active) : NULL;
}

/* Where the pointer was last seen, in screen coordinates. */
BOOL WINAPI GetCursorPos(LPPOINT lpPoint) {
    if (lpPoint == NULL) {
        set_last_error(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    *lpPoint = g_last_point;
    return TRUE;
}

/* ---- System parameters ------------------------------------------------------------- */

/* What gdi32 draws text with: edges smoothed in grey, XP's standard
 * smoothing (docs/userland/WIN32.md, "Text"). Other actions fail, as they
 * did while the call was only declared. */
static BOOL system_parameters(UINT uiAction, LPVOID pvParam) {
    switch (uiAction) {
    case 0x004a: /* SPI_GETFONTSMOOTHING */
    case 0x200a: /* SPI_GETFONTSMOOTHINGTYPE: FE_FONTSMOOTHINGSTANDARD */
        if (pvParam == NULL) {
            set_last_error(ERROR_INVALID_PARAMETER);
            return FALSE;
        }
        *(UINT *)pvParam = 1u;
        return TRUE;
    default:
        return FALSE;
    }
}

BOOL WINAPI SystemParametersInfoW(UINT uiAction, UINT uiParam, LPVOID pvParam, UINT fWinIni) {
    (void)uiParam;
    (void)fWinIni;
    return system_parameters(uiAction, pvParam);
}

BOOL WINAPI SystemParametersInfoA(UINT uiAction, UINT uiParam, LPVOID pvParam, UINT fWinIni) {
    (void)uiParam;
    (void)fWinIni;
    return system_parameters(uiAction, pvParam);
}

/* ---- Rectangles ---------------------------------------------------------------------- */

BOOL WINAPI SetRect(LPRECT lprc, int xLeft, int yTop, int xRight, int yBottom) {
    if (lprc == NULL) {
        return FALSE;
    }
    lprc->left = xLeft;
    lprc->top = yTop;
    lprc->right = xRight;
    lprc->bottom = yBottom;
    return TRUE;
}

BOOL WINAPI SetRectEmpty(LPRECT lprc) {
    return SetRect(lprc, 0, 0, 0, 0);
}

BOOL WINAPI IsRectEmpty(const RECT *lprc) {
    return lprc == NULL || lprc->right <= lprc->left || lprc->bottom <= lprc->top;
}

BOOL WINAPI EqualRect(const RECT *lprc1, const RECT *lprc2) {
    if (lprc1 == NULL || lprc2 == NULL) {
        return FALSE;
    }
    return lprc1->left == lprc2->left && lprc1->top == lprc2->top && lprc1->right == lprc2->right &&
           lprc1->bottom == lprc2->bottom;
}

BOOL WINAPI OffsetRect(LPRECT lprc, int dx, int dy) {
    if (lprc == NULL) {
        return FALSE;
    }
    lprc->left += dx;
    lprc->right += dx;
    lprc->top += dy;
    lprc->bottom += dy;
    return TRUE;
}

/* The smallest rectangle holding both; an empty one does not count. */
BOOL WINAPI UnionRect(LPRECT lprcDst, const RECT *lprcSrc1, const RECT *lprcSrc2) {
    if (lprcDst == NULL || lprcSrc1 == NULL || lprcSrc2 == NULL) {
        return FALSE;
    }
    RECT a = *lprcSrc1;
    RECT b = *lprcSrc2;
    if (IsRectEmpty(&a)) {
        if (IsRectEmpty(&b)) {
            SetRectEmpty(lprcDst);
            return FALSE;
        }
        *lprcDst = b;
        return TRUE;
    }
    if (IsRectEmpty(&b)) {
        *lprcDst = a;
        return TRUE;
    }
    lprcDst->left = a.left < b.left ? a.left : b.left;
    lprcDst->top = a.top < b.top ? a.top : b.top;
    lprcDst->right = a.right > b.right ? a.right : b.right;
    lprcDst->bottom = a.bottom > b.bottom ? a.bottom : b.bottom;
    return TRUE;
}
