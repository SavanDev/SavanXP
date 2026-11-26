/*
 * caret.c -- The caret (docs/userland/WIN32.md, "Carets"): one for the
 * process, owned by a window, with a hide count, blinking every
 * GetCaretBlinkTime milliseconds. It is drawn by inverting its rectangle on
 * the window, so drawing it twice takes it away; painting the window
 * takes it off first. The calls fail and succeed as Windows XP's did for
 * fixtures/hooks: no caret is ERROR_ACCESS_DENIED, a window that does not
 * own it fails without an error, and a new caret keeps the old position.
 */
#include "user32_private.h"

#define BLINK_MS 530u

typedef struct caret {
    window *owner; /* 0: no caret */
    HWND handle;
    int width;
    int height;
    int x;
    int y;
    int hidden;   /* hide count: shown only at 0 */
    int drawn;    /* inverted on the window now */
    u32 next;     /* when it blinks next */
} caret;

static caret g_caret;
static u32 g_blink = BLINK_MS;

static void draw(void) {
    window *w = g_caret.owner;
    if (w == 0 || !visible_chain(w)) {
        return;
    }
    HDC dc = window_dc(w, 0);
    PatBlt(dc, g_caret.x, g_caret.y, g_caret.width, g_caret.height, DSTINVERT);
    DeleteObject(dc);
    g_caret.drawn = !g_caret.drawn;
}

static void take_off(void) {
    if (g_caret.drawn) {
        draw();
    }
}

static int owns(HWND hWnd) {
    return g_caret.owner != 0 && (hWnd == NULL || hWnd == g_caret.handle);
}

BOOL WINAPI CreateCaret(HWND hWnd, HBITMAP hBitmap, int nWidth, int nHeight) {
    window *w = checked(hWnd);
    if (w == 0) {
        return FALSE;
    }
    if (g_caret.owner != 0) {
        DestroyCaret();
    }
    (void)hBitmap;
    g_caret.owner = w;
    g_caret.handle = hWnd;
    g_caret.width = nWidth > 0 ? nWidth : GetSystemMetrics(SM_CXBORDER);
    g_caret.height = nHeight > 0 ? nHeight : GetSystemMetrics(SM_CYBORDER);
    g_caret.hidden = 1;
    g_caret.drawn = 0;
    return TRUE;
}

BOOL WINAPI DestroyCaret(void) {
    if (g_caret.owner == 0) {
        set_last_error(ERROR_ACCESS_DENIED);
        return FALSE;
    }
    take_off();
    g_caret.owner = 0;
    g_caret.handle = NULL;
    return TRUE;
}

BOOL WINAPI ShowCaret(HWND hWnd) {
    if (!owns(hWnd)) {
        return FALSE;
    }
    if (g_caret.hidden > 0 && --g_caret.hidden == 0) {
        draw();
        g_caret.next = ticks() + g_blink;
    }
    return TRUE;
}

BOOL WINAPI HideCaret(HWND hWnd) {
    if (!owns(hWnd)) {
        return FALSE;
    }
    if (g_caret.hidden++ == 0) {
        take_off();
    }
    return TRUE;
}

BOOL WINAPI SetCaretPos(int X, int Y) {
    if (g_caret.owner == 0) {
        set_last_error(ERROR_ACCESS_DENIED);
        return FALSE;
    }
    if (X == g_caret.x && Y == g_caret.y) {
        return TRUE;
    }
    const int was = g_caret.drawn;
    take_off();
    g_caret.x = X;
    g_caret.y = Y;
    if (was || (g_caret.hidden == 0)) {
        draw();
        g_caret.next = ticks() + g_blink;
    }
    return TRUE;
}

BOOL WINAPI GetCaretPos(LPPOINT lpPoint) {
    if (lpPoint == NULL) {
        return FALSE;
    }
    lpPoint->x = g_caret.x;
    lpPoint->y = g_caret.y;
    return TRUE;
}

UINT WINAPI GetCaretBlinkTime(void) {
    return g_blink;
}

BOOL WINAPI SetCaretBlinkTime(UINT uMSeconds) {
    g_blink = uMSeconds != 0 ? uMSeconds : BLINK_MS;
    return TRUE;
}

/* BeginPaint takes the caret off a window before it is painted over, and
 * EndPaint puts it back at once and starts its blink again, as Windows XP
 * does: the caret is on the window after every paint, whatever the blink
 * had come to (fixtures/controls). */
void caret_hide_for_paint(window *w) {
    if (g_caret.owner == w) {
        take_off();
    }
}

void caret_show_after_paint(window *w) {
    if (g_caret.owner == w && g_caret.hidden == 0 && !g_caret.drawn) {
        draw();
        g_caret.next = ticks() + g_blink;
    }
}

void caret_window_gone(window *w) {
    if (g_caret.owner == w) {
        g_caret.owner = 0;
        g_caret.handle = NULL;
        g_caret.drawn = 0;
    }
}

/* When the caret of this thread blinks next, or 0 for never. */
u32 caret_next_blink(u32 now) {
    (void)now;
    if (g_caret.owner == 0 || g_caret.hidden > 0 || g_caret.owner->thread != GetCurrentThreadId()) {
        return 0;
    }
    return g_caret.next;
}

void caret_blink(u32 now) {
    if (g_caret.owner == 0 || g_caret.hidden > 0 || (int)(now - g_caret.next) < 0) {
        return;
    }
    draw();
    g_caret.next = now + g_blink;
}
