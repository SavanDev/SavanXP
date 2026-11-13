/*
 * user32.dll -- Windows and messages (docs/userland/WIN32.md, "user32 and
 * gdi32"): window classes, the window tree, the message queue, painting,
 * timers, input, capture, focus and activation, and DefWindowProc, for the
 * functions of the W0 matrix, and the resources a program loads: bitmaps,
 * strings, icons, cursors and accelerator tables. Menus and dialogs are W5's.
 *
 * A top-level window is a host window (SXW_SVC_WINDOW_CREATE) whose frame
 * and caption are the host's; its client area is a surface in guest memory
 * that every window of the tree draws on through gdi32. Child windows exist
 * only here. One thread: a message is "sent" by calling the window
 * procedure.
 *
 * The order of the messages each call sends is Windows XP's, as
 * fixtures/window recorded it there.
 */
#define SXW_BUILDING_USER32
#include <windows.h>

#include "codepage.h"
#include "region.h"
#include "sxw_abi.h"
#include "sxw_gdi.h"

typedef unsigned int u32;

__declspec(dllimport) u32 WINAPI SxwService(u32 service, const u32 *arguments);
__declspec(dllimport) void *WINAPI SxwHeapAllocate(u32 size, u32 zero);
__declspec(dllimport) int WINAPI SxwHeapFree(void *pointer);

#define MAX_CLASSES 32
#define MAX_WINDOWS 128
#define MAX_TIMERS 32
#define MAX_POSTED 256
#define MAX_INPUT 128
#define MAX_REGISTERED 32
#define MAX_TEXT 256
#define DOUBLE_CLICK_MS 500u
#define DOUBLE_CLICK_DISTANCE 2 /* half of SM_CXDOUBLECLK */
#define DESKTOP_HANDLE ((HWND)0x00010010)
#define WAIT_OBJECT_0 0u
#define WAIT_FAILED 0xffffffffu
#define ERROR_INVALID_WINDOW_HANDLE 1400u
#define ERROR_CANNOT_FIND_WND_CLASS 1407u
#define ERROR_CLASS_ALREADY_EXISTS 1410u
#define ERROR_RESOURCE_TYPE_NOT_FOUND 1813u
#define ERROR_CALL_NOT_IMPLEMENTED 120u

/* The frame Windows XP draws in its classic style, which AdjustWindowRect
 * and WM_NCCALCSIZE report. On SavanXP the frame is the window manager's;
 * these numbers only decide how a window size given to CreateWindowEx
 * becomes a client size, and they do it the same way both directions. */
#define METRIC_CAPTION 19
#define METRIC_FRAME 4
#define METRIC_DIALOG_FRAME 3
#define METRIC_BORDER 1

static void set_last_error(DWORD error) {
    __asm__ volatile("movl %0, %%fs:%c1" : : "r"(error), "i"(SXW_TEB_LAST_ERROR) : "memory");
}

static u32 ticks(void) {
    return SxwService(SXW_SVC_TICKS, 0);
}

static void zero(void *p, u32 size) {
    unsigned char *b = p;
    for (u32 i = 0; i < size; ++i) {
        b[i] = 0;
    }
}

static int same_name(const WCHAR *a, const WCHAR *b) {
    for (;; ++a, ++b) {
        WCHAR ca = *a >= 'A' && *a <= 'Z' ? (WCHAR)(*a + 32) : *a;
        WCHAR cb = *b >= 'A' && *b <= 'Z' ? (WCHAR)(*b + 32) : *b;
        if (ca != cb) {
            return 0;
        }
        if (ca == 0) {
            return 1;
        }
    }
}

static int copy_text(WCHAR *out, const WCHAR *text, int capacity) {
    int n = 0;
    if (text != NULL) {
        while (text[n] != 0 && n + 1 < capacity) {
            out[n] = text[n];
            ++n;
        }
    }
    out[n] = 0;
    return n;
}

/* ---- Classes ---------------------------------------------------------------- */

typedef struct wclass {
    int used;
    ATOM atom;
    WCHAR name[64];
    UINT style;
    WNDPROC proc;
    int window_extra;
    HINSTANCE instance;
    HICON icon;
    HCURSOR cursor;
    HBRUSH background;
} wclass;

static wclass g_classes[MAX_CLASSES];

static wclass *find_class(LPCWSTR name) {
    for (int i = 0; i < MAX_CLASSES; ++i) {
        wclass *c = &g_classes[i];
        if (c->used && ((u32)name < 0x10000u ? c->atom == (ATOM)(u32)name : same_name(c->name, name))) {
            return c;
        }
    }
    return 0;
}

static ATOM register_class(UINT style, WNDPROC proc, int window_extra, HINSTANCE instance, HICON icon,
                           HCURSOR cursor, HBRUSH background, LPCWSTR name) {
    if (proc == NULL || name == NULL || (u32)name < 0x10000u || window_extra < 0 || window_extra > 1024) {
        set_last_error(ERROR_INVALID_PARAMETER);
        return 0;
    }
    if (find_class(name) != 0) {
        set_last_error(ERROR_CLASS_ALREADY_EXISTS);
        return 0;
    }
    for (int i = 0; i < MAX_CLASSES; ++i) {
        wclass *c = &g_classes[i];
        if (!c->used) {
            zero(c, sizeof(*c));
            c->used = 1;
            c->atom = (ATOM)(0xc000 + i);
            copy_text(c->name, name, 64);
            c->style = style;
            c->proc = proc;
            c->window_extra = window_extra;
            c->instance = instance;
            c->icon = icon;
            c->cursor = cursor;
            c->background = background;
            return c->atom;
        }
    }
    set_last_error(ERROR_NOT_ENOUGH_MEMORY);
    return 0;
}

ATOM WINAPI RegisterClassW(const WNDCLASSW *lpWndClass) {
    return register_class(lpWndClass->style, lpWndClass->lpfnWndProc, lpWndClass->cbWndExtra,
                          lpWndClass->hInstance, lpWndClass->hIcon, lpWndClass->hCursor,
                          lpWndClass->hbrBackground, lpWndClass->lpszClassName);
}

ATOM WINAPI RegisterClassExW(const WNDCLASSEXW *lpWndClass) {
    if (lpWndClass->cbSize != sizeof(WNDCLASSEXW)) {
        set_last_error(ERROR_INVALID_PARAMETER);
        return 0;
    }
    return register_class(lpWndClass->style, lpWndClass->lpfnWndProc, lpWndClass->cbWndExtra,
                          lpWndClass->hInstance, lpWndClass->hIcon, lpWndClass->hCursor,
                          lpWndClass->hbrBackground, lpWndClass->lpszClassName);
}

/* ---- Windows ----------------------------------------------------------------- */

typedef struct window window;
struct window {
    int used;
    u32 generation;
    wclass *cls;
    WNDPROC proc;
    DWORD style;
    DWORD ex_style;
    window *parent;
    window *child; /* the topmost child */
    window *next;  /* the next sibling below this one */
    /* In the parent's client coordinates; a top-level window's in the
     * screen's. */
    sxw_rect rect;
    sxw_rect client;
    UINT_PTR id;
    WCHAR text[MAX_TEXT];
    unsigned char *extra;
    /* Invalid area, client coordinates, and whether it needs erasing. */
    sxw_region invalid;
    int erase;
    /* A top-level window has not had its first WM_SIZE and WM_MOVE. */
    int need_size;
    int destroying;
    /* Top-level only: the host window and the surface every window of the
     * tree draws on. */
    u32 host;
    sxw_surface surface;
    int surface_capacity; /* pixels */
};

static window g_windows[MAX_WINDOWS];
static u32 g_generation = 1;
static window *g_active;
static window *g_focus;
static window *g_capture;

static HWND handle_of(const window *w) {
    return w == 0 ? NULL : (HWND)((w->generation << 16) | (u32)((w - g_windows) + 1) * 4u);
}

static window *window_of(HWND h) {
    const u32 v = (u32)h;
    const u32 index = (v & 0xffffu) / 4u;
    if ((v & 3u) != 0 || index == 0 || index > MAX_WINDOWS) {
        return 0;
    }
    window *w = &g_windows[index - 1u];
    return w->used && w->generation == (v >> 16) ? w : 0;
}

static window *checked(HWND h) {
    window *w = window_of(h);
    if (w == 0) {
        set_last_error(ERROR_INVALID_WINDOW_HANDLE);
    }
    return w;
}

static window *top_of(window *w) {
    while (w->parent != 0) {
        w = w->parent;
    }
    return w;
}

static LRESULT send(window *w, UINT message, WPARAM wParam, LPARAM lParam) {
    return w != 0 && w->proc != NULL ? w->proc(handle_of(w), message, wParam, lParam) : 0;
}

static int visible_chain(const window *w) {
    for (; w != 0; w = w->parent) {
        if ((w->style & WS_VISIBLE) == 0) {
            return 0;
        }
    }
    return 1;
}

/* Where a window's client area starts on its top-level window's surface. */
static void client_origin(const window *w, int *x, int *y) {
    *x = 0;
    *y = 0;
    for (; w->parent != 0; w = w->parent) {
        *x += w->client.left;
        *y += w->client.top;
    }
}

static int client_width(const window *w) {
    return w->client.right - w->client.left;
}

static int client_height(const window *w) {
    return w->client.bottom - w->client.top;
}

/* The part of a window's client area that is drawn, in surface
 * coordinates: inside every ancestor's client area, without its children
 * when it clips them, and without the siblings above it when it clips those. */
static void visible_region(const window *w, sxw_region *out) {
    sxw_region_clear(out);
    if (!visible_chain(w)) {
        return;
    }
    int ox;
    int oy;
    client_origin(w, &ox, &oy);
    sxw_rect r = {ox, oy, ox + client_width(w), oy + client_height(w)};
    sxw_region_set(out, &r);
    for (const window *a = w->parent; a != 0; a = a->parent) {
        int ax;
        int ay;
        client_origin(a, &ax, &ay);
        sxw_rect ar = {ax, ay, ax + client_width(a), ay + client_height(a)};
        sxw_region_intersect_rect(out, &ar);
    }
    if ((w->style & WS_CLIPCHILDREN) != 0) {
        for (const window *c = w->child; c != 0; c = c->next) {
            if ((c->style & WS_VISIBLE) != 0) {
                sxw_rect cr = c->rect;
                sxw_rect_offset(&cr, ox, oy);
                sxw_region_subtract_rect(out, &cr);
            }
        }
    }
    if ((w->style & WS_CLIPSIBLINGS) != 0 && w->parent != 0) {
        int px;
        int py;
        client_origin(w->parent, &px, &py);
        for (const window *s = w->parent->child; s != 0 && s != w; s = s->next) {
            if ((s->style & WS_VISIBLE) != 0) {
                sxw_rect sr = s->rect;
                sxw_rect_offset(&sr, px, py);
                sxw_region_subtract_rect(out, &sr);
            }
        }
    }
}

/* A device context on a window's client area, clipped to clip (client
 * coordinates) when it is given. */
static HDC window_dc(window *w, const sxw_region *clip) {
    window *top = top_of(w);
    sxw_region visible;
    visible_region(w, &visible);
    int ox;
    int oy;
    client_origin(w, &ox, &oy);
    if (clip != 0) {
        sxw_region c = *clip;
        sxw_region_offset(&c, ox, oy);
        sxw_region_intersect(&visible, &c);
    }
    return (HDC)SxwCreateSurfaceDC(&top->surface, ox, oy, &visible);
}

/* ---- Frame metrics ------------------------------------------------------------ */

/* Left, top, right and bottom thickness of the frame a style asks for. */
static void frame_of(DWORD style, int menu, int out[4]) {
    int edge = 0;
    if ((style & WS_THICKFRAME) != 0) {
        edge = METRIC_FRAME;
    } else if ((style & WS_DLGFRAME) != 0 || (style & WS_CAPTION) == WS_CAPTION) {
        edge = METRIC_DIALOG_FRAME;
    } else if ((style & WS_BORDER) != 0) {
        edge = METRIC_BORDER;
    }
    out[0] = out[2] = out[3] = edge;
    out[1] = edge + ((style & WS_CAPTION) == WS_CAPTION ? METRIC_CAPTION : 0) + (menu ? 20 : 0);
}

BOOL WINAPI AdjustWindowRect(LPRECT lpRect, DWORD dwStyle, BOOL bMenu) {
    if (lpRect == NULL) {
        return FALSE;
    }
    int f[4];
    frame_of((dwStyle & WS_CHILD) != 0 ? dwStyle : dwStyle | WS_CAPTION, bMenu, f);
    lpRect->left -= f[0];
    lpRect->top -= f[1];
    lpRect->right += f[2];
    lpRect->bottom += f[3];
    return TRUE;
}

/* ---- The surface of a top-level window ---------------------------------------- */

static int size_surface(window *top, int width, int height) {
    if (width < 1) {
        width = 1;
    }
    if (height < 1) {
        height = 1;
    }
    if (width * height > top->surface_capacity) {
        unsigned int *pixels = SxwHeapAllocate((u32)(width * height) * 4u, 1);
        if (pixels == 0) {
            return 0;
        }
        if (top->surface.pixels != 0) {
            SxwHeapFree(top->surface.pixels);
        }
        top->surface.pixels = pixels;
        top->surface_capacity = width * height;
    }
    top->surface.stride = width;
    top->surface.width = width;
    top->surface.height = height;
    sxw_region_clear(&top->surface.dirty);
    return 1;
}

/* Shows what was drawn on every top-level window since the last time. */
static void present_all(void) {
    for (int i = 0; i < MAX_WINDOWS; ++i) {
        window *w = &g_windows[i];
        if (!w->used || w->parent != 0 || w->host == 0 || sxw_region_empty(&w->surface.dirty)) {
            continue;
        }
        static RECT rects[SXW_REGION_MAX];
        const sxw_region *d = &w->surface.dirty;
        for (int j = 0; j < d->count; ++j) {
            rects[j].left = d->rects[j].left;
            rects[j].top = d->rects[j].top;
            rects[j].right = d->rects[j].right;
            rects[j].bottom = d->rects[j].bottom;
        }
        u32 size[2] = {(u32)w->surface.width, (u32)w->surface.height};
        u32 arguments[6] = {w->host, (u32)w->surface.pixels, (u32)w->surface.stride * 4u, (u32)size,
                            (u32)rects, (u32)d->count};
        SxwService(SXW_SVC_WINDOW_PRESENT, arguments);
        sxw_region_clear(&w->surface.dirty);
    }
}

/* ---- Invalidation and painting --------------------------------------------------- */

/* Adds rect (client coordinates; NULL for the whole client area) to a
 * window's invalid area, and to its children's when children is set. */
static void invalidate(window *w, const sxw_rect *rect, int erase, int children) {
    sxw_rect whole = {0, 0, client_width(w), client_height(w)};
    sxw_rect r;
    if (!sxw_rect_intersect(&r, rect != 0 ? rect : &whole, &whole)) {
        return;
    }
    sxw_region_union_rect(&w->invalid, &r);
    w->erase |= erase;
    if (!children) {
        return;
    }
    for (window *c = w->child; c != 0; c = c->next) {
        sxw_rect cr = r;
        sxw_rect_offset(&cr, -c->client.left, -c->client.top);
        invalidate(c, &cr, erase, 1);
    }
}

/* WM_ERASEBKGND for the invalid part; 1 when the window erased it. */
static int erase_background(window *w) {
    HDC dc = window_dc(w, &w->invalid);
    const int erased = send(w, WM_ERASEBKGND, (WPARAM)dc, 0) != 0;
    DeleteObject(dc);
    return erased;
}

/* Erases what is waiting to be erased, the window first and its children
 * after, as showing a window does (RDW_ERASENOW). */
static void erase_now(window *w) {
    if (!visible_chain(w)) {
        return;
    }
    if (w->erase && !sxw_region_empty(&w->invalid)) {
        w->erase = !erase_background(w);
    }
    for (window *c = w->child; c != 0; c = c->next) {
        erase_now(c);
    }
}

/* The first visible window, parents before children, that needs painting. */
static window *needs_paint(window *w, HWND filter) {
    if (!w->used || !visible_chain(w)) {
        return 0;
    }
    if (!sxw_region_empty(&w->invalid) && (filter == NULL || filter == handle_of(w))) {
        return w;
    }
    for (window *c = w->child; c != 0; c = c->next) {
        window *found = needs_paint(c, filter);
        if (found != 0) {
            return found;
        }
    }
    return 0;
}

static window *any_paint(HWND filter) {
    for (int i = 0; i < MAX_WINDOWS; ++i) {
        window *w = &g_windows[i];
        if (w->used && w->parent == 0 && !w->destroying) {
            window *found = needs_paint(w, filter);
            if (found != 0) {
                return found;
            }
        }
    }
    return 0;
}

HDC WINAPI BeginPaint(HWND hWnd, LPPAINTSTRUCT lpPaint) {
    window *w = checked(hWnd);
    if (w == 0 || lpPaint == NULL) {
        return NULL;
    }
    zero(lpPaint, sizeof(*lpPaint));
    sxw_region update = w->invalid;
    if (w->erase) {
        w->erase = 0;
        lpPaint->fErase = !erase_background(w);
    }
    sxw_rect bounds = sxw_region_bounds(&update);
    lpPaint->rcPaint.left = bounds.left;
    lpPaint->rcPaint.top = bounds.top;
    lpPaint->rcPaint.right = bounds.right;
    lpPaint->rcPaint.bottom = bounds.bottom;
    sxw_region_clear(&w->invalid);
    lpPaint->hdc = window_dc(w, &update);
    return lpPaint->hdc;
}

BOOL WINAPI EndPaint(HWND hWnd, const PAINTSTRUCT *lpPaint) {
    (void)hWnd;
    if (lpPaint != NULL && lpPaint->hdc != NULL) {
        DeleteObject(lpPaint->hdc);
    }
    return TRUE;
}

/* GetDC(NULL), the screen's device context: what a program asks it about
 * (GetDeviceCaps) works, and drawing on it does nothing. */
static sxw_surface g_screen_surface;
static unsigned int g_screen_pixel;

HDC WINAPI GetDC(HWND hWnd) {
    if (hWnd == NULL) {
        sxw_region nothing;
        sxw_region_clear(&nothing);
        g_screen_surface.pixels = &g_screen_pixel;
        g_screen_surface.stride = g_screen_surface.width = g_screen_surface.height = 1;
        return (HDC)SxwCreateSurfaceDC(&g_screen_surface, 0, 0, &nothing);
    }
    window *w = checked(hWnd);
    return w != 0 ? window_dc(w, 0) : NULL;
}

int WINAPI ReleaseDC(HWND hWnd, HDC hDC) {
    (void)hWnd;
    return DeleteObject(hDC) ? 1 : 0;
}

/* A NULL rectangle is the whole client area. Children are included unless
 * the window clips them, as RedrawWindow decides without RDW_ALLCHILDREN. */
BOOL WINAPI InvalidateRect(HWND hWnd, const RECT *lpRect, BOOL bErase) {
    window *w = checked(hWnd);
    if (w == 0) {
        return FALSE;
    }
    sxw_rect r;
    if (lpRect != NULL) {
        r.left = lpRect->left;
        r.top = lpRect->top;
        r.right = lpRect->right;
        r.bottom = lpRect->bottom;
    }
    invalidate(w, lpRect != NULL ? &r : 0, bErase != 0, (w->style & WS_CLIPCHILDREN) == 0);
    return TRUE;
}

static void update_now(window *w) {
    if (!w->used || !visible_chain(w)) {
        return;
    }
    if (!sxw_region_empty(&w->invalid)) {
        send(w, WM_PAINT, 0, 0);
    }
    for (window *c = w->child; c != 0; c = c->next) {
        update_now(c);
    }
}

/* The window's WM_PAINT if it needs one, then its children's. */
BOOL WINAPI UpdateWindow(HWND hWnd) {
    window *w = checked(hWnd);
    if (w == 0) {
        return FALSE;
    }
    update_now(w);
    return TRUE;
}

/* ---- Activation, focus and capture --------------------------------------------- */

HWND WINAPI SetFocus(HWND hWnd) {
    window *w = hWnd != NULL ? checked(hWnd) : 0;
    if (hWnd != NULL && w == 0) {
        return NULL;
    }
    window *old = g_focus;
    if (w == old) {
        return handle_of(old);
    }
    if (old != 0) {
        send(old, WM_KILLFOCUS, (WPARAM)handle_of(w), 0);
    }
    g_focus = w;
    if (w != 0) {
        send(w, WM_SETFOCUS, (WPARAM)handle_of(old), 0);
    }
    return handle_of(old);
}

static void activate(window *w) {
    if (g_active == w) {
        return;
    }
    const int app_was_active = g_active != 0;
    g_active = w;
    if (!app_was_active) {
        send(w, WM_ACTIVATEAPP, TRUE, 0);
    }
    send(w, WM_NCACTIVATE, TRUE, 0);
    send(w, WM_ACTIVATE, WA_ACTIVE, 0);
}

static int within(const window *w, const window *ancestor) {
    for (; w != 0; w = w->parent) {
        if (w == ancestor) {
            return 1;
        }
    }
    return 0;
}

/* The last window of the program stops being active, and loses the focus. */
static void deactivate(window *w) {
    if (g_active == w) {
        send(w, WM_NCACTIVATE, FALSE, 0);
        send(w, WM_ACTIVATE, WA_INACTIVE, 0);
        g_active = 0;
        send(w, WM_ACTIVATEAPP, FALSE, 0);
    }
    if (g_focus != 0 && within(g_focus, w)) {
        window *old = g_focus;
        g_focus = 0;
        send(old, WM_KILLFOCUS, 0, 0);
    }
}

HWND WINAPI SetCapture(HWND hWnd) {
    window *w = checked(hWnd);
    window *old = g_capture;
    if (w == 0) {
        return NULL;
    }
    g_capture = w;
    if (old != 0 && old != w) {
        send(old, WM_CAPTURECHANGED, 0, (LPARAM)hWnd);
    }
    return handle_of(old);
}

BOOL WINAPI ReleaseCapture(void) {
    window *old = g_capture;
    g_capture = 0;
    if (old != 0) {
        send(old, WM_CAPTURECHANGED, 0, 0);
    }
    return TRUE;
}

HWND WINAPI GetCapture(void) {
    return handle_of(g_capture);
}

/* ---- Creation and destruction --------------------------------------------------- */

static void link_child(window *parent, window *w) {
    w->next = parent->child;
    parent->child = w;
}

static void unlink_child(window *w) {
    if (w->parent == 0) {
        return;
    }
    for (window **link = &w->parent->child; *link != 0; link = &(*link)->next) {
        if (*link == w) {
            *link = w->next;
            return;
        }
    }
}

static void notify_parents(window *w, UINT event, LPARAM lParam) {
    if ((w->ex_style & WS_EX_NOPARENTNOTIFY) != 0 || (w->style & WS_CHILD) == 0) {
        return;
    }
    for (window *p = w->parent; p != 0; p = p->parent) {
        send(p, WM_PARENTNOTIFY, event == WM_CREATE || event == WM_DESTROY ? (event | (w->id << 16)) : event,
             lParam);
        if ((p->style & WS_CHILD) == 0 || (p->ex_style & WS_EX_NOPARENTNOTIFY) != 0) {
            break;
        }
    }
}

static void free_window(window *w) {
    if (w->extra != 0) {
        SxwHeapFree(w->extra);
    }
    if (w->surface.pixels != 0) {
        SxwHeapFree(w->surface.pixels);
    }
    w->used = 0;
}

static void send_size_move(window *w) {
    const DWORD kind = (w->style & WS_MINIMIZE) != 0 ? SIZE_MINIMIZED : SIZE_RESTORED;
    send(w, WM_SIZE, kind, MAKELPARAM(client_width(w), client_height(w)));
    send(w, WM_MOVE, 0, MAKELPARAM(w->client.left, w->client.top));
}

static void window_pos(window *w, UINT flags) {
    WINDOWPOS pos;
    zero(&pos, sizeof(pos));
    pos.hwnd = handle_of(w);
    pos.x = w->rect.left;
    pos.y = w->rect.top;
    pos.cx = w->rect.right - w->rect.left;
    pos.cy = w->rect.bottom - w->rect.top;
    pos.flags = flags;
    send(w, WM_WINDOWPOSCHANGING, 0, (LPARAM)&pos);
    send(w, WM_WINDOWPOSCHANGED, 0, (LPARAM)&pos);
}

static void destroy_tree(window *w);

static void timers_of_window_end(window *w);
static void drop_messages(window *w);

HWND WINAPI CreateWindowExW(DWORD dwExStyle, LPCWSTR lpClassName, LPCWSTR lpWindowName, DWORD dwStyle, int X,
                            int Y, int nWidth, int nHeight, HWND hWndParent, HMENU hMenu, HINSTANCE hInstance,
                            LPVOID lpParam) {
    wclass *cls = lpClassName != NULL ? find_class(lpClassName) : 0;
    if (cls == 0) {
        set_last_error(ERROR_CANNOT_FIND_WND_CLASS);
        return NULL;
    }
    window *parent = 0;
    if ((dwStyle & WS_CHILD) != 0) {
        parent = checked(hWndParent);
        if (parent == 0) {
            return NULL;
        }
    } else if (hMenu != NULL) {
        /* A menu bar is W5's. */
        set_last_error(ERROR_CALL_NOT_IMPLEMENTED);
        return NULL;
    }
    window *w = 0;
    for (int i = 0; i < MAX_WINDOWS && w == 0; ++i) {
        if (!g_windows[i].used) {
            w = &g_windows[i];
        }
    }
    if (w == 0) {
        set_last_error(ERROR_NOT_ENOUGH_MEMORY);
        return NULL;
    }
    zero(w, sizeof(*w));
    w->used = 1;
    w->generation = g_generation;
    g_generation = g_generation == 0x7fffu ? 1u : g_generation + 1u;
    w->cls = cls;
    w->proc = cls->proc;
    w->style = dwStyle & ~(DWORD)WS_VISIBLE;
    w->ex_style = dwExStyle;
    w->parent = parent;
    w->id = parent != 0 ? (UINT_PTR)hMenu : 0;
    if (cls->window_extra > 0) {
        w->extra = SxwHeapAllocate((u32)cls->window_extra, 1);
        if (w->extra == 0) {
            w->used = 0;
            set_last_error(ERROR_NOT_ENOUGH_MEMORY);
            return NULL;
        }
    }
    if (parent == 0) {
        if (X == CW_USEDEFAULT) {
            X = Y = 22;
        }
        if (nWidth == CW_USEDEFAULT) {
            nWidth = 640;
            nHeight = 480;
        }
        w->style |= WS_CLIPSIBLINGS;
        if ((w->style & WS_POPUP) == 0) {
            w->style |= WS_CAPTION;
        }
        w->need_size = 1;
    } else {
        link_child(parent, w);
    }
    w->rect.left = X;
    w->rect.top = Y;
    w->rect.right = X + (nWidth > 0 ? nWidth : 0);
    w->rect.bottom = Y + (nHeight > 0 ? nHeight : 0);
    const HWND hwnd = handle_of(w);

    CREATESTRUCTW cs;
    cs.lpCreateParams = lpParam;
    cs.hInstance = hInstance;
    cs.hMenu = hMenu;
    cs.hwndParent = hWndParent;
    cs.cy = nHeight;
    cs.cx = nWidth;
    cs.y = Y;
    cs.x = X;
    cs.style = (LONG)dwStyle;
    cs.lpszName = lpWindowName;
    cs.lpszClass = lpClassName;
    cs.dwExStyle = dwExStyle;

    if (parent == 0) {
        MINMAXINFO info;
        zero(&info, sizeof(info));
        send(w, WM_GETMINMAXINFO, 0, (LPARAM)&info);
    }
    if (!send(w, WM_NCCREATE, 0, (LPARAM)&cs)) {
        destroy_tree(w);
        return NULL;
    }
    RECT r = {w->rect.left, w->rect.top, w->rect.right, w->rect.bottom};
    send(w, WM_NCCALCSIZE, FALSE, (LPARAM)&r);
    w->client.left = r.left;
    w->client.top = r.top;
    w->client.right = r.right > r.left ? r.right : r.left;
    w->client.bottom = r.bottom > r.top ? r.bottom : r.top;
    if (parent == 0 && !size_surface(w, client_width(w), client_height(w))) {
        destroy_tree(w);
        set_last_error(ERROR_NOT_ENOUGH_MEMORY);
        return NULL;
    }
    if (send(w, WM_CREATE, 0, (LPARAM)&cs) == -1) {
        DestroyWindow(hwnd);
        return NULL;
    }
    if (window_of(hwnd) == 0) {
        return NULL;
    }
    if (parent != 0) {
        send_size_move(w);
        notify_parents(w, WM_CREATE, (LPARAM)hwnd);
    }
    if ((dwStyle & WS_VISIBLE) != 0 && window_of(hwnd) != 0) {
        ShowWindow(hwnd, SW_SHOW);
    }
    return window_of(hwnd) != 0 ? hwnd : NULL;
}

/* WM_DESTROY parents first, WM_NCDESTROY children first. */
static void send_destroy(window *w) {
    w->destroying = 1;
    if (g_capture == w) {
        ReleaseCapture();
    }
    send(w, WM_DESTROY, 0, 0);
    for (window *c = w->child; c != 0; c = c->next) {
        send_destroy(c);
    }
}

static void send_nc_destroy(window *w) {
    for (window *c = w->child; c != 0; c = c->next) {
        send_nc_destroy(c);
    }
    send(w, WM_NCDESTROY, 0, 0);
}

static void destroy_tree(window *w) {
    while (w->child != 0) {
        window *c = w->child;
        w->child = c->next;
        c->parent = 0;
        destroy_tree(c);
    }
    if (g_focus == w) {
        g_focus = 0;
    }
    if (g_active == w) {
        g_active = 0;
    }
    if (g_capture == w) {
        g_capture = 0;
    }
    timers_of_window_end(w);
    drop_messages(w);
    unlink_child(w);
    if (w->host != 0) {
        u32 arguments[1] = {w->host};
        SxwService(SXW_SVC_WINDOW_DESTROY, arguments);
    }
    free_window(w);
}

BOOL WINAPI DestroyWindow(HWND hWnd) {
    window *w = checked(hWnd);
    if (w == 0) {
        return FALSE;
    }
    if (w->destroying) {
        return TRUE;
    }
    if ((w->style & WS_CHILD) != 0) {
        notify_parents(w, WM_DESTROY, (LPARAM)hWnd);
    }
    if ((w->style & WS_VISIBLE) != 0) {
        window_pos(w, 0x0080u /* SWP_HIDEWINDOW */);
        w->style &= ~(DWORD)WS_VISIBLE;
        if (w->parent != 0) {
            sxw_rect r = w->rect;
            invalidate(w->parent, &r, 1, 0);
        }
    }
    if (w->parent == 0) {
        deactivate(w);
    } else if (g_focus != 0 && within(g_focus, w)) {
        SetFocus(NULL);
    }
    send_destroy(w);
    send_nc_destroy(w);
    if (w->parent != 0) {
        unlink_child(w);
        w->parent = 0;
    }
    destroy_tree(w);
    return TRUE;
}

/* ---- Showing, moving and enabling ----------------------------------------------- */

static int show_host(window *w) {
    if (w->host != 0) {
        return 1;
    }
    u32 size[2] = {0, 0};
    u32 arguments[4] = {(u32)w->text, (u32)client_width(w), (u32)client_height(w), (u32)size};
    if (client_width(w) <= 0 || client_height(w) <= 0) {
        arguments[1] = arguments[2] = 1;
    }
    w->host = SxwService(SXW_SVC_WINDOW_CREATE, arguments);
    if (w->host == 0) {
        return 0;
    }
    if ((int)size[0] != client_width(w) || (int)size[1] != client_height(w)) {
        /* The window manager gave another size. */
        w->rect.right += (int)size[0] - client_width(w);
        w->rect.bottom += (int)size[1] - client_height(w);
        w->client.right = w->client.left + (int)size[0];
        w->client.bottom = w->client.top + (int)size[1];
        size_surface(w, (int)size[0], (int)size[1]);
    }
    return 1;
}

BOOL WINAPI ShowWindow(HWND hWnd, int nCmdShow) {
    window *w = checked(hWnd);
    if (w == 0) {
        return FALSE;
    }
    const int was_visible = (w->style & WS_VISIBLE) != 0;
    const int show = nCmdShow != SW_HIDE;
    if (show == was_visible) {
        return was_visible;
    }
    send(w, WM_SHOWWINDOW, show, 0);
    if (!show) {
        window_pos(w, 0x0080u /* SWP_HIDEWINDOW */);
        w->style &= ~(DWORD)WS_VISIBLE;
        if (w->parent != 0) {
            sxw_rect r = w->rect;
            invalidate(w->parent, &r, 1, 0);
        } else {
            deactivate(w);
        }
        return was_visible;
    }
    WINDOWPOS pos;
    zero(&pos, sizeof(pos));
    pos.hwnd = hWnd;
    pos.flags = 0x0040u; /* SWP_SHOWWINDOW */
    send(w, WM_WINDOWPOSCHANGING, 0, (LPARAM)&pos);
    w->style |= WS_VISIBLE;
    if (w->parent == 0) {
        if (!show_host(w)) {
            w->style &= ~(DWORD)WS_VISIBLE;
            return FALSE;
        }
        if (nCmdShow != SW_SHOWNA && nCmdShow != SW_SHOWNOACTIVATE && nCmdShow != SW_SHOWMINNOACTIVE) {
            activate(w);
        }
    }
    invalidate(w, 0, 1, 1);
    if (visible_chain(w)) {
        erase_now(w);
    }
    pos.x = w->rect.left;
    pos.y = w->rect.top;
    pos.cx = w->rect.right - w->rect.left;
    pos.cy = w->rect.bottom - w->rect.top;
    send(w, WM_WINDOWPOSCHANGED, 0, (LPARAM)&pos);
    return was_visible;
}

/* A child is moved and sized; a top-level window is moved only, since the
 * window manager owns its size on SavanXP. */
BOOL WINAPI MoveWindow(HWND hWnd, int X, int Y, int nWidth, int nHeight, BOOL bRepaint) {
    window *w = checked(hWnd);
    if (w == 0) {
        return FALSE;
    }
    const sxw_rect old = w->rect;
    const int old_width = client_width(w);
    const int old_height = client_height(w);
    if (w->parent == 0) {
        nWidth = old.right - old.left;
        nHeight = old.bottom - old.top;
    }
    const int dx = X - old.left;
    const int dy = Y - old.top;
    w->rect.left = X;
    w->rect.top = Y;
    w->rect.right = X + nWidth;
    w->rect.bottom = Y + nHeight;
    const int border = (w->parent != 0 && (w->style & WS_BORDER) != 0) ? 1 : 0;
    w->client.left += dx;
    w->client.top += dy;
    if (w->parent != 0) {
        w->client.left = X + border;
        w->client.top = Y + border;
        w->client.right = X + nWidth - border;
        w->client.bottom = Y + nHeight - border;
        if (w->client.right < w->client.left) {
            w->client.right = w->client.left;
        }
        if (w->client.bottom < w->client.top) {
            w->client.bottom = w->client.top;
        }
    } else {
        w->client.right += dx;
        w->client.bottom += dy;
    }
    WINDOWPOS pos;
    zero(&pos, sizeof(pos));
    pos.hwnd = hWnd;
    pos.x = X;
    pos.y = Y;
    pos.cx = nWidth;
    pos.cy = nHeight;
    send(w, WM_WINDOWPOSCHANGING, 0, (LPARAM)&pos);
    send(w, WM_WINDOWPOSCHANGED, 0, (LPARAM)&pos);
    if (old_width != client_width(w) || old_height != client_height(w)) {
        send(w, WM_SIZE, SIZE_RESTORED, MAKELPARAM(client_width(w), client_height(w)));
    }
    if (dx != 0 || dy != 0) {
        send(w, WM_MOVE, 0, MAKELPARAM(w->client.left, w->client.top));
    }
    if (bRepaint && w->parent != 0) {
        sxw_rect before = old;
        invalidate(w->parent, &before, 1, 0);
        invalidate(w, 0, 1, 1);
    }
    return TRUE;
}

BOOL WINAPI EnableWindow(HWND hWnd, BOOL bEnable) {
    window *w = checked(hWnd);
    if (w == 0) {
        return FALSE;
    }
    const BOOL was_disabled = (w->style & WS_DISABLED) != 0;
    if (bEnable && was_disabled) {
        w->style &= ~(DWORD)WS_DISABLED;
        send(w, WM_ENABLE, TRUE, 0);
    } else if (!bEnable && !was_disabled) {
        w->style |= WS_DISABLED;
        if (g_focus != 0 && within(g_focus, w)) {
            SetFocus(NULL);
        }
        send(w, WM_ENABLE, FALSE, 0);
    }
    return was_disabled;
}

BOOL WINAPI IsIconic(HWND hWnd) {
    window *w = window_of(hWnd);
    return w != 0 && (w->style & WS_MINIMIZE) != 0;
}

BOOL WINAPI GetClientRect(HWND hWnd, LPRECT lpRect) {
    window *w = checked(hWnd);
    if (w == 0 || lpRect == NULL) {
        return FALSE;
    }
    lpRect->left = 0;
    lpRect->top = 0;
    lpRect->right = client_width(w);
    lpRect->bottom = client_height(w);
    return TRUE;
}

HWND WINAPI GetDesktopWindow(void) {
    return DESKTOP_HANDLE;
}

/* Screen coordinates from the window's own idea of its position: on SavanXP
 * the window manager places top-level windows and does not say where. */
BOOL WINAPI ClientToScreen(HWND hWnd, LPPOINT lpPoint) {
    window *w = checked(hWnd);
    if (w == 0 || lpPoint == NULL) {
        return FALSE;
    }
    for (; w != 0; w = w->parent) {
        lpPoint->x += w->client.left;
        lpPoint->y += w->client.top;
    }
    return TRUE;
}

/* ---- Timers ------------------------------------------------------------------ */

typedef struct timer {
    int used;
    window *w;
    HWND hwnd;
    UINT_PTR id;
    u32 elapse;
    u32 due;
    TIMERPROC proc;
} timer;

static timer g_timers[MAX_TIMERS];
static UINT_PTR g_next_timer_id = 1;

static int expired(u32 due, u32 now) {
    return (int)(now - due) >= 0;
}

UINT_PTR WINAPI SetTimer(HWND hWnd, UINT_PTR nIDEvent, UINT uElapse, TIMERPROC lpTimerFunc) {
    window *w = 0;
    if (hWnd != NULL && (w = checked(hWnd)) == 0) {
        return 0;
    }
    if (uElapse < 10u) {
        uElapse = 10u; /* USER_TIMER_MINIMUM */
    }
    timer *slot = 0;
    for (int i = 0; i < MAX_TIMERS; ++i) {
        timer *t = &g_timers[i];
        if (t->used && hWnd != NULL && t->hwnd == hWnd && t->id == nIDEvent) {
            slot = t;
            break;
        }
        if (!t->used && slot == 0) {
            slot = t;
        }
    }
    if (slot == 0) {
        set_last_error(ERROR_NOT_ENOUGH_MEMORY);
        return 0;
    }
    if (hWnd == NULL) {
        nIDEvent = g_next_timer_id++;
    }
    slot->used = 1;
    slot->w = w;
    slot->hwnd = hWnd;
    slot->id = nIDEvent;
    slot->elapse = uElapse;
    slot->due = ticks() + uElapse;
    slot->proc = lpTimerFunc;
    return hWnd != NULL ? nIDEvent : slot->id;
}

BOOL WINAPI KillTimer(HWND hWnd, UINT_PTR uIDEvent) {
    for (int i = 0; i < MAX_TIMERS; ++i) {
        timer *t = &g_timers[i];
        if (t->used && t->hwnd == hWnd && t->id == uIDEvent) {
            t->used = 0;
            return TRUE;
        }
    }
    return FALSE;
}

static void timers_of_window_end(window *w) {
    for (int i = 0; i < MAX_TIMERS; ++i) {
        if (g_timers[i].used && g_timers[i].w == w) {
            g_timers[i].used = 0;
        }
    }
}

/* The timer that is due first, or 0. */
static timer *next_timer(HWND filter) {
    timer *best = 0;
    for (int i = 0; i < MAX_TIMERS; ++i) {
        timer *t = &g_timers[i];
        if (t->used && (filter == NULL || t->hwnd == filter) && (best == 0 || (int)(t->due - best->due) < 0)) {
            best = t;
        }
    }
    return best;
}

/* ---- The message queue ----------------------------------------------------------- */

/* An input message carries the key or button it changes: the state
 * GetKeyState reports changes when the message is retrieved. */
typedef struct queued {
    MSG msg;
    int vk;   /* 0, or the virtual key this message presses or releases */
    int down;
} queued;

static queued g_posted[MAX_POSTED];
static u32 g_posted_head;
static u32 g_posted_count;
static queued g_input[MAX_INPUT];
static u32 g_input_head;
static u32 g_input_count;
static int g_quit;
static int g_quit_code;
static unsigned char g_key_state[256];  /* 0x80 down, 0x01 toggled */
static unsigned char g_async_down[256]; /* as input arrives */
static WCHAR g_key_char[256];           /* what the last press of each key typed */
static POINT g_last_point;

static int matches(const MSG *m, HWND filter, UINT first, UINT last) {
    if (filter != NULL && m->hwnd != filter) {
        return 0;
    }
    if (first == 0 && last == 0) {
        return 1;
    }
    return m->message >= first && m->message <= last;
}

static int take(queued *ring, u32 *head, u32 *count, u32 capacity, HWND filter, UINT first, UINT last,
                int remove, queued *out) {
    for (u32 i = 0; i < *count; ++i) {
        queued *q = &ring[(*head + i) % capacity];
        if (!matches(&q->msg, filter, first, last)) {
            continue;
        }
        *out = *q;
        if (remove) {
            for (u32 j = i; j > 0; --j) {
                ring[(*head + j) % capacity] = ring[(*head + j - 1u) % capacity];
            }
            *head = (*head + 1u) % capacity;
            --*count;
        }
        return 1;
    }
    return 0;
}

static int push(queued *ring, u32 head, u32 *count, u32 capacity, const queued *q) {
    if (*count == capacity) {
        return 0;
    }
    ring[(head + *count) % capacity] = *q;
    ++*count;
    return 1;
}

static void drop_messages(window *w) {
    const HWND h = handle_of(w);
    queued kept;
    for (u32 n = g_posted_count; n > 0; --n) {
        if (take(g_posted, &g_posted_head, &g_posted_count, MAX_POSTED, NULL, 0, 0, 1, &kept) &&
            kept.msg.hwnd != h) {
            push(g_posted, g_posted_head, &g_posted_count, MAX_POSTED, &kept);
        }
    }
    for (u32 n = g_input_count; n > 0; --n) {
        if (take(g_input, &g_input_head, &g_input_count, MAX_INPUT, NULL, 0, 0, 1, &kept) &&
            kept.msg.hwnd != h) {
            push(g_input, g_input_head, &g_input_count, MAX_INPUT, &kept);
        }
    }
}

BOOL WINAPI PostMessageW(HWND hWnd, UINT Msg, WPARAM wParam, LPARAM lParam) {
    if (hWnd != NULL && checked(hWnd) == 0) {
        return FALSE;
    }
    queued q;
    zero(&q, sizeof(q));
    q.msg.hwnd = hWnd;
    q.msg.message = Msg;
    q.msg.wParam = wParam;
    q.msg.lParam = lParam;
    q.msg.time = ticks();
    q.msg.pt = g_last_point;
    if (!push(g_posted, g_posted_head, &g_posted_count, MAX_POSTED, &q)) {
        set_last_error(ERROR_NOT_ENOUGH_MEMORY);
        return FALSE;
    }
    return TRUE;
}

void WINAPI PostQuitMessage(int nExitCode) {
    g_quit = 1;
    g_quit_code = nExitCode;
}

static UINT g_registered_count;
static WCHAR g_registered[MAX_REGISTERED][64];

UINT WINAPI RegisterWindowMessageW(LPCWSTR lpString) {
    if (lpString == NULL || lpString[0] == 0) {
        set_last_error(ERROR_INVALID_PARAMETER);
        return 0;
    }
    for (UINT i = 0; i < g_registered_count; ++i) {
        if (same_name(g_registered[i], lpString)) {
            return 0xc000u + 0x100u + i;
        }
    }
    if (g_registered_count == MAX_REGISTERED) {
        set_last_error(ERROR_NOT_ENOUGH_MEMORY);
        return 0;
    }
    copy_text(g_registered[g_registered_count], lpString, 64);
    return 0xc000u + 0x100u + g_registered_count++;
}

/* ---- Input ------------------------------------------------------------------------ */

static window *host_window(u32 host) {
    for (int i = 0; i < MAX_WINDOWS; ++i) {
        if (g_windows[i].used && g_windows[i].parent == 0 && g_windows[i].host == host) {
            return &g_windows[i];
        }
    }
    return 0;
}

/* The deepest visible, enabled window under a point of a top-level window's
 * surface; 0 outside its client area. */
static window *hit(window *w, int x, int y) {
    int ox;
    int oy;
    client_origin(w, &ox, &oy);
    if (x < ox || y < oy || x >= ox + client_width(w) || y >= oy + client_height(w)) {
        return 0;
    }
    for (window *c = w->child; c != 0; c = c->next) {
        if ((c->style & WS_VISIBLE) != 0 && x >= ox + c->rect.left && x < ox + c->rect.right &&
            y >= oy + c->rect.top && y < oy + c->rect.bottom) {
            window *deeper = hit(c, x, y);
            if (deeper != 0 && (deeper->style & WS_DISABLED) == 0) {
                return deeper;
            }
            return (c->style & WS_DISABLED) == 0 ? c : w;
        }
    }
    return w;
}

static WPARAM mouse_keys(void) {
    WPARAM keys = 0;
    keys |= g_async_down[VK_LBUTTON] ? MK_LBUTTON : 0;
    keys |= g_async_down[VK_RBUTTON] ? MK_RBUTTON : 0;
    keys |= g_async_down[VK_MBUTTON] ? MK_MBUTTON : 0;
    keys |= g_async_down[VK_SHIFT] ? MK_SHIFT : 0;
    keys |= g_async_down[VK_CONTROL] ? MK_CONTROL : 0;
    return keys;
}

static void queue_input(window *target, UINT message, WPARAM wParam, LPARAM lParam, u32 time, int vk, int down) {
    queued q;
    zero(&q, sizeof(q));
    q.msg.hwnd = handle_of(target);
    q.msg.message = message;
    q.msg.wParam = wParam;
    q.msg.lParam = lParam;
    q.msg.time = time;
    q.msg.pt = g_last_point;
    q.vk = vk;
    q.down = down;
    push(g_input, g_input_head, &g_input_count, MAX_INPUT, &q);
}

typedef struct click {
    window *w;
    UINT message;
    u32 time;
    int x;
    int y;
} click;

static click g_last_click;
static int g_have_pointer;
static int g_pointer_x;
static int g_pointer_y;
static u32 g_pointer_buttons;

static void pointer_event(const sxw_event *e) {
    window *top = host_window(e->window);
    if (top == 0) {
        return;
    }
    const int x = (int)e->a;
    const int y = (int)e->b;
    window *target = g_capture != 0 && top_of(g_capture) == top ? g_capture : hit(top, x, y);
    const int moved = !g_have_pointer || x != g_pointer_x || y != g_pointer_y;
    g_have_pointer = 1;
    g_pointer_x = x;
    g_pointer_y = y;
    g_last_point.x = top->client.left + x;
    g_last_point.y = top->client.top + y;
    static const struct {
        u32 bit;
        int vk;
        UINT down;
    } k_buttons[3] = {{SXW_BUTTON_LEFT, VK_LBUTTON, WM_LBUTTONDOWN},
                      {SXW_BUTTON_RIGHT, VK_RBUTTON, WM_RBUTTONDOWN},
                      {SXW_BUTTON_MIDDLE, VK_MBUTTON, WM_MBUTTONDOWN}};
    if (target == 0 || (target->style & WS_DISABLED) != 0) {
        /* Outside the client area: only the button state is followed. */
        for (int i = 0; i < 3; ++i) {
            g_async_down[k_buttons[i].vk] = (e->c & k_buttons[i].bit) != 0;
        }
        g_pointer_buttons = e->c;
        return;
    }
    int ox;
    int oy;
    client_origin(target, &ox, &oy);
    const LPARAM where = MAKELPARAM(x - ox, y - oy);
    if (moved) {
        queue_input(target, WM_MOUSEMOVE, mouse_keys(), where, e->time, 0, 0);
    }
    for (int i = 0; i < 3; ++i) {
        const int was = (g_pointer_buttons & k_buttons[i].bit) != 0;
        const int is = (e->c & k_buttons[i].bit) != 0;
        if (was == is) {
            continue;
        }
        g_async_down[k_buttons[i].vk] = (unsigned char)is;
        UINT message = is ? k_buttons[i].down : k_buttons[i].down + 1u;
        if (is) {
            const int dx = x - g_last_click.x;
            const int dy = y - g_last_click.y;
            if ((target->cls->style & CS_DBLCLKS) != 0 && g_last_click.w == target &&
                g_last_click.message == message && e->time - g_last_click.time <= DOUBLE_CLICK_MS &&
                dx >= -DOUBLE_CLICK_DISTANCE && dx <= DOUBLE_CLICK_DISTANCE && dy >= -DOUBLE_CLICK_DISTANCE &&
                dy <= DOUBLE_CLICK_DISTANCE) {
                message = k_buttons[i].down + 2u; /* WM_xBUTTONDBLCLK */
                g_last_click.w = 0;
            } else {
                g_last_click.w = target;
                g_last_click.message = message;
                g_last_click.time = e->time;
                g_last_click.x = x;
                g_last_click.y = y;
            }
        }
        queue_input(target, message, mouse_keys(), where, e->time, k_buttons[i].vk, is);
    }
    g_pointer_buttons = e->c;
}

static const unsigned char k_scan_codes[256] = {
    [VK_BACK] = 0x0e, [VK_TAB] = 0x0f, [VK_RETURN] = 0x1c, [VK_SHIFT] = 0x2a, [VK_CONTROL] = 0x1d,
    [VK_MENU] = 0x38, [VK_ESCAPE] = 0x01, [VK_SPACE] = 0x39, [VK_LEFT] = 0x4b, [VK_UP] = 0x48,
    [VK_RIGHT] = 0x4d, [VK_DOWN] = 0x50, ['0'] = 0x0b, ['1'] = 0x02, ['2'] = 0x03, ['3'] = 0x04,
    ['4'] = 0x05, ['5'] = 0x06, ['6'] = 0x07, ['7'] = 0x08, ['8'] = 0x09, ['9'] = 0x0a,
    ['A'] = 0x1e, ['B'] = 0x30, ['C'] = 0x2e, ['D'] = 0x20, ['E'] = 0x12, ['F'] = 0x21, ['G'] = 0x22,
    ['H'] = 0x23, ['I'] = 0x17, ['J'] = 0x24, ['K'] = 0x25, ['L'] = 0x26, ['M'] = 0x32, ['N'] = 0x31,
    ['O'] = 0x18, ['P'] = 0x19, ['Q'] = 0x10, ['R'] = 0x13, ['S'] = 0x1f, ['T'] = 0x14, ['U'] = 0x16,
    ['V'] = 0x2f, ['W'] = 0x11, ['X'] = 0x2d, ['Y'] = 0x15, ['Z'] = 0x2c, [VK_F1] = 0x3b, [VK_F2] = 0x3c,
    [0x72] = 0x3d, [VK_F4] = 0x3e, [0x74] = 0x3f, [0x75] = 0x40, [0x76] = 0x41, [0x77] = 0x42,
    [0x78] = 0x43, [VK_F10] = 0x44, [0x7a] = 0x57, [0x7b] = 0x58,
};

static void key_event(const sxw_event *e) {
    window *top = host_window(e->window);
    const int vk = (int)(e->a & 0xffu);
    const int down = e->b != 0;
    window *target = g_focus != 0 ? g_focus : g_active != 0 ? g_active : top;
    const int was_down = g_async_down[vk] != 0;
    g_async_down[vk] = (unsigned char)down;
    if (target == 0) {
        return;
    }
    const int alt = g_async_down[VK_MENU] || (vk == VK_MENU && !down);
    const int system = alt || vk == VK_F10;
    UINT message = down ? WM_KEYDOWN : WM_KEYUP;
    if (system && !g_async_down[VK_CONTROL]) {
        message = down ? WM_SYSKEYDOWN : WM_SYSKEYUP;
    }
    LPARAM lParam = 1 | ((LPARAM)k_scan_codes[vk] << 16);
    if (alt && message != WM_KEYDOWN && message != WM_KEYUP) {
        lParam |= 1L << 29;
    }
    if (was_down) {
        lParam |= 1L << 30;
    }
    if (!down) {
        lParam |= (LPARAM)(1UL << 31) | (1L << 30);
    }
    if (down) {
        g_key_char[vk] = (WCHAR)e->c;
    }
    queue_input(target, message, (WPARAM)vk, lParam, e->time, vk, down);
}

/* The frame's close: WM_SYSCOMMAND(SC_CLOSE) arrives as the frame's own
 * Alt+F4 or close button would bring it. */
static void close_event(const sxw_event *e) {
    window *top = host_window(e->window);
    if (top != 0) {
        queue_input(top, WM_SYSCOMMAND, SC_CLOSE, MAKELPARAM(g_last_point.x, g_last_point.y), e->time, 0, 0);
    }
}

static void resize_event(const sxw_event *e) {
    window *top = host_window(e->window);
    if (top == 0 || ((int)e->a == client_width(top) && (int)e->b == client_height(top))) {
        return;
    }
    if (!size_surface(top, (int)e->a, (int)e->b)) {
        return;
    }
    top->rect.right += (int)e->a - client_width(top);
    top->rect.bottom += (int)e->b - client_height(top);
    top->client.right = top->client.left + (int)e->a;
    top->client.bottom = top->client.top + (int)e->b;
    window_pos(top, 0);
    send(top, WM_SIZE, SIZE_RESTORED, MAKELPARAM(client_width(top), client_height(top)));
    invalidate(top, 0, 1, 1);
}

static void handle_event(const sxw_event *e) {
    switch (e->kind) {
    case SXW_EVENT_POINTER:
        pointer_event(e);
        break;
    case SXW_EVENT_KEY:
        key_event(e);
        break;
    case SXW_EVENT_CLOSE:
        close_event(e);
        break;
    case SXW_EVENT_RESIZE:
        resize_event(e);
        break;
    default:
        break;
    }
}

/* Waits up to timeout milliseconds for an event and handles it. 1 when one
 * came. */
static int wait_event(u32 timeout) {
    sxw_event e;
    u32 arguments[2] = {timeout, (u32)&e};
    if (SxwService(SXW_SVC_WAIT_EVENT, arguments) == 0) {
        return 0;
    }
    handle_event(&e);
    return 1;
}

static void poll_events(void) {
    while (wait_event(0)) {
    }
}

/* What retrieving a mouse message sends first: the hit test, the parent's
 * notification of a click, the activation query, and the cursor query. */
static void before_mouse(window *w, const MSG *m) {
    if (g_capture == w) {
        return;
    }
    send(w, WM_NCHITTEST, 0, MAKELPARAM(g_last_point.x, g_last_point.y));
    const int click = m->message != WM_MOUSEMOVE &&
                      (m->message == WM_LBUTTONDOWN || m->message == WM_RBUTTONDOWN ||
                       m->message == WM_MBUTTONDOWN || m->message == WM_LBUTTONDBLCLK ||
                       m->message == WM_RBUTTONDBLCLK || m->message == WM_MBUTTONDBLCLK);
    if (click) {
        if (w->parent != 0) {
            POINT p = {GET_X_LPARAM(m->lParam), GET_Y_LPARAM(m->lParam)};
            for (window *c = w; c->parent != 0; c = c->parent) {
                p.x += c->client.left;
                p.y += c->client.top;
                if ((c->ex_style & WS_EX_NOPARENTNOTIFY) != 0) {
                    break;
                }
                send(c->parent, WM_PARENTNOTIFY, m->message, MAKELPARAM(p.x, p.y));
                if ((c->parent->style & WS_CHILD) == 0) {
                    break;
                }
            }
        }
        if (w != g_active) {
            window *top = top_of(w);
            const LRESULT answer =
                send(w, WM_MOUSEACTIVATE, (WPARAM)handle_of(top), MAKELPARAM(HTCLIENT, m->message));
            if (answer == MA_ACTIVATE || answer == 2 /* MA_ACTIVATEANDEAT */) {
                activate(top);
            }
        }
    }
    send(w, WM_SETCURSOR, (WPARAM)handle_of(w), MAKELPARAM(HTCLIENT, m->message));
}

/* GetMessage and PeekMessage: sent, posted, quit, input, paint, timer, in
 * Windows' order. 1 with a message, 0 when there was none and block is off. */
static int next_message(MSG *msg, HWND filter, UINT first, UINT last, int remove, int block) {
    for (;;) {
        poll_events();
        queued q;
        if (take(g_posted, &g_posted_head, &g_posted_count, MAX_POSTED, filter, first, last, remove, &q)) {
            *msg = q.msg;
            return 1;
        }
        if (g_quit && filter == NULL) {
            zero(msg, sizeof(*msg));
            msg->message = WM_QUIT;
            msg->wParam = (WPARAM)g_quit_code;
            msg->time = ticks();
            if (remove) {
                g_quit = 0;
            }
            return 1;
        }
        if (take(g_input, &g_input_head, &g_input_count, MAX_INPUT, filter, first, last, remove, &q)) {
            *msg = q.msg;
            if (remove && q.vk != 0) {
                g_key_state[q.vk] = (unsigned char)((q.down ? 0x80 : 0) |
                                                    ((g_key_state[q.vk] ^ (q.down ? 1 : 0)) & 1));
            }
            window *w = window_of(q.msg.hwnd);
            if (w != 0 && remove && q.msg.message >= WM_MOUSEMOVE && q.msg.message <= WM_MBUTTONDBLCLK) {
                before_mouse(w, &q.msg);
            }
            if (w == 0 && q.msg.hwnd != NULL) {
                /* Its window is gone: drop it, even when only peeking. */
                if (!remove) {
                    take(g_input, &g_input_head, &g_input_count, MAX_INPUT, filter, first, last, 1, &q);
                }
                continue;
            }
            return 1;
        }
        if (first == 0 || (first <= WM_PAINT && last >= WM_PAINT)) {
            window *w = any_paint(filter);
            if (w != 0) {
                zero(msg, sizeof(*msg));
                msg->hwnd = handle_of(w);
                msg->message = WM_PAINT;
                msg->time = ticks();
                return 1;
            }
        }
        const u32 now = ticks();
        timer *t = next_timer(filter);
        if (t != 0 && expired(t->due, now) && (first == 0 || (first <= WM_TIMER && last >= WM_TIMER))) {
            zero(msg, sizeof(*msg));
            msg->hwnd = t->hwnd;
            msg->message = WM_TIMER;
            msg->wParam = t->id;
            msg->lParam = (LPARAM)t->proc;
            msg->time = now;
            if (remove) {
                t->due = now + t->elapse;
            }
            return 1;
        }
        if (!block) {
            present_all();
            return 0;
        }
        present_all();
        wait_event(t != 0 ? (expired(t->due, now) ? 0u : t->due - now) : SXW_INFINITE);
    }
}

BOOL WINAPI GetMessageW(LPMSG lpMsg, HWND hWnd, UINT wMsgFilterMin, UINT wMsgFilterMax) {
    if (lpMsg == NULL) {
        set_last_error(ERROR_INVALID_PARAMETER);
        return -1;
    }
    next_message(lpMsg, hWnd, wMsgFilterMin, wMsgFilterMax, 1, 1);
    return lpMsg->message != WM_QUIT;
}

BOOL WINAPI PeekMessageW(LPMSG lpMsg, HWND hWnd, UINT wMsgFilterMin, UINT wMsgFilterMax, UINT wRemoveMsg) {
    if (lpMsg == NULL) {
        set_last_error(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    return next_message(lpMsg, hWnd, wMsgFilterMin, wMsgFilterMax, (wRemoveMsg & PM_REMOVE) != 0, 0);
}

/* Waiting on no handle: returns when a message arrives or the time is up. */
DWORD WINAPI MsgWaitForMultipleObjects(DWORD nCount, const HANDLE *pHandles, BOOL fWaitAll, DWORD dwMilliseconds,
                                       DWORD dwWakeMask) {
    (void)pHandles;
    (void)fWaitAll;
    (void)dwWakeMask;
    if (nCount != 0) {
        set_last_error(ERROR_INVALID_PARAMETER);
        return WAIT_FAILED;
    }
    const u32 start = ticks();
    for (;;) {
        poll_events();
        timer *t = next_timer(NULL);
        const u32 now = ticks();
        if (g_posted_count != 0 || g_input_count != 0 || g_quit || any_paint(NULL) != 0 ||
            (t != 0 && expired(t->due, now))) {
            return WAIT_OBJECT_0;
        }
        const u32 waited = now - start;
        if (dwMilliseconds != SXW_INFINITE && waited >= dwMilliseconds) {
            return WAIT_TIMEOUT;
        }
        u32 timeout = dwMilliseconds == SXW_INFINITE ? SXW_INFINITE : dwMilliseconds - waited;
        if (t != 0 && t->due - now < timeout) {
            timeout = t->due - now;
        }
        present_all();
        wait_event(timeout);
    }
}

BOOL WINAPI TranslateMessage(const MSG *lpMsg) {
    if (lpMsg == NULL || (lpMsg->message != WM_KEYDOWN && lpMsg->message != WM_SYSKEYDOWN)) {
        return FALSE;
    }
    const WCHAR c = g_key_char[lpMsg->wParam & 0xffu];
    if (c == 0) {
        return FALSE;
    }
    PostMessageW(lpMsg->hwnd, lpMsg->message == WM_KEYDOWN ? WM_CHAR : WM_SYSCHAR, c, lpMsg->lParam);
    return TRUE;
}

LRESULT WINAPI DispatchMessageW(const MSG *lpMsg) {
    if (lpMsg == NULL) {
        return 0;
    }
    if (lpMsg->message == WM_TIMER && lpMsg->lParam != 0) {
        ((TIMERPROC)lpMsg->lParam)(lpMsg->hwnd, WM_TIMER, lpMsg->wParam, ticks());
        return 0;
    }
    window *w = lpMsg->hwnd != NULL ? checked(lpMsg->hwnd) : 0;
    if (w == 0) {
        return 0;
    }
    return send(w, lpMsg->message, lpMsg->wParam, lpMsg->lParam);
}

short WINAPI GetKeyState(int nVirtKey) {
    const unsigned char s = g_key_state[nVirtKey & 0xff];
    return (short)(((s & 0x80) != 0 ? 0xff80 : 0) | (s & 1));
}

/* The pointer is the window manager's: a program cannot move it. Only
 * keyboard play in Solitaire asks (docs/userland/WIN32.md). */
BOOL WINAPI SetCursorPos(int X, int Y) {
    (void)X;
    (void)Y;
    set_last_error(ERROR_CALL_NOT_IMPLEMENTED);
    return FALSE;
}

static int g_cursor_count;

int WINAPI ShowCursor(BOOL bShow) {
    g_cursor_count += bShow ? 1 : -1;
    return g_cursor_count;
}

/* ---- Resources ------------------------------------------------------------------ */
/*
 * Read from the loaded image itself: its resource directory, type then name
 * then the first language, as the program's own LoadXxx calls find them.
 */

#define RT_CURSOR 1u
#define RT_BITMAP 2u
#define RT_ICON 3u
#define RT_STRING 6u
#define RT_ACCELERATOR 9u
#define RT_GROUP_CURSOR 12u
#define RT_GROUP_ICON 14u
#define ERROR_RESOURCE_DATA_NOT_FOUND 1812u
#define ERROR_RESOURCE_NAME_NOT_FOUND 1814u
#define MAX_ICONS 64
#define MAX_ACCELERATORS 16

__declspec(dllimport) HBITMAP WINAPI SxwCreateBitmapFromDIB(const void *data, u32 size);

static u32 get16(const unsigned char *p) {
    return (u32)p[0] | ((u32)p[1] << 8);
}

static u32 get32(const unsigned char *p) {
    return get16(p) | (get16(p + 2) << 16);
}

static u32 main_module(void) {
    u32 peb;
    __asm__ volatile("movl %%fs:%c1, %0" : "=r"(peb) : "i"(SXW_TEB_PEB));
    return *(u32 *)(peb + SXW_PEB_IMAGE_BASE);
}

/* The entry of a resource directory with this id, or this name (UTF-16, any
 * case); 0 when there is none. The entry's second word, as stored. */
static int find_entry(const unsigned char *root, const unsigned char *dir, LPCWSTR name, u32 *entry) {
    const u32 named = get16(dir + 12);
    const u32 ids = get16(dir + 14);
    for (u32 i = 0; i < named + ids; ++i) {
        const unsigned char *e = dir + 16 + 8 * i;
        const u32 key = get32(e);
        int match;
        if ((u32)name < 0x10000u) {
            match = (key & 0x80000000u) == 0 && key == (u32)name;
        } else if ((key & 0x80000000u) != 0) {
            const unsigned char *string = root + (key & 0x7fffffffu);
            const u32 length = get16(string);
            match = 1;
            u32 k = 0;
            for (; k < length && match; ++k) {
                WCHAR a = (WCHAR)get16(string + 2 + 2 * k);
                WCHAR b = name[k];
                a = a >= 'a' && a <= 'z' ? (WCHAR)(a - 32) : a;
                b = b >= 'a' && b <= 'z' ? (WCHAR)(b - 32) : b;
                match = b != 0 && a == b;
            }
            match = match && name[k] == 0;
        } else {
            match = 0;
        }
        if (match) {
            *entry = get32(e + 4);
            return 1;
        }
    }
    return 0;
}

/* A resource's data and size in the module at base, or 0. */
static const unsigned char *find_resource(u32 base, u32 type, LPCWSTR name, u32 *size) {
    const unsigned char *image = (const unsigned char *)base;
    if (base == 0 || get16(image) != 0x5a4du) {
        return 0;
    }
    const unsigned char *nt = image + get32(image + 0x3c);
    if (get32(nt) != 0x4550u || get16(nt + 24) != 0x10bu) {
        return 0;
    }
    const unsigned char *optional = nt + 24;
    if (get32(optional + 92) <= 2u) {
        return 0;
    }
    const u32 rva = get32(optional + 96 + 2 * 8);
    if (rva == 0) {
        return 0;
    }
    const unsigned char *root = image + rva;
    u32 entry = 0;
    if (!find_entry(root, root, (LPCWSTR)(ULONG_PTR)type, &entry) || (entry & 0x80000000u) == 0) {
        return 0;
    }
    const unsigned char *names = root + (entry & 0x7fffffffu);
    if (!find_entry(root, names, name, &entry) || (entry & 0x80000000u) == 0) {
        return 0;
    }
    const unsigned char *languages = root + (entry & 0x7fffffffu);
    if (get16(languages + 12) + get16(languages + 14) == 0u) {
        return 0;
    }
    entry = get32(languages + 16 + 4);
    if ((entry & 0x80000000u) != 0) {
        return 0;
    }
    const unsigned char *data = root + entry;
    *size = get32(data + 4);
    return image + get32(data);
}

static u32 module_of(HINSTANCE instance) {
    return instance != NULL ? (u32)instance : main_module();
}

/* An ANSI resource name as UTF-16, or the id it already is. */
static LPCWSTR wide_name(LPCSTR name, WCHAR *out, int capacity) {
    if ((u32)name < 0x10000u) {
        return (LPCWSTR)name;
    }
    int n = 0;
    while (name[n] != 0 && n + 1 < capacity) {
        out[n] = sxw_ansi_to_wide((unsigned char)name[n]);
        ++n;
    }
    out[n] = 0;
    return out;
}

static HBITMAP load_bitmap(HINSTANCE instance, LPCWSTR name) {
    u32 size = 0;
    const unsigned char *data = instance != NULL ? find_resource((u32)instance, RT_BITMAP, name, &size) : 0;
    if (data == 0) {
        set_last_error(ERROR_RESOURCE_NAME_NOT_FOUND);
        return NULL;
    }
    HBITMAP bitmap = SxwCreateBitmapFromDIB(data, size);
    if (bitmap == NULL) {
        set_last_error(ERROR_NOT_ENOUGH_MEMORY);
    }
    return bitmap;
}

/* A bitmap of the module; the system's own (a NULL instance) are not here. */
HBITMAP WINAPI LoadBitmapA(HINSTANCE hInstance, LPCSTR lpBitmapName) {
    WCHAR name[256];
    return load_bitmap(hInstance, wide_name(lpBitmapName, name, 256));
}

/* The string, its length in UTF-16 units; with no buffer room, a pointer to
 * the resource itself (not terminated) in *lpBuffer. */
static const unsigned char *find_string(HINSTANCE instance, UINT id, u32 *length) {
    u32 size = 0;
    const unsigned char *block =
        find_resource(module_of(instance), RT_STRING, (LPCWSTR)(ULONG_PTR)((id >> 4) + 1u), &size);
    if (block == 0) {
        return 0;
    }
    const unsigned char *p = block;
    for (u32 i = 0; i < (id & 15u); ++i) {
        p += 2u + 2u * get16(p);
        if (p >= block + size) {
            return 0;
        }
    }
    *length = get16(p);
    return *length != 0u ? p + 2 : 0;
}

int WINAPI LoadStringW(HINSTANCE hInstance, UINT uID, LPWSTR lpBuffer, int cchBufferMax) {
    u32 length = 0;
    const unsigned char *text = find_string(hInstance, uID, &length);
    if (lpBuffer == NULL) {
        return 0;
    }
    if (cchBufferMax == 0) {
        *(const unsigned char **)lpBuffer = text;
        return text != 0 ? (int)length : 0;
    }
    if (text == 0) {
        lpBuffer[0] = 0;
        set_last_error(ERROR_RESOURCE_NAME_NOT_FOUND);
        return 0;
    }
    int n = (int)length < cchBufferMax - 1 ? (int)length : cchBufferMax - 1;
    for (int i = 0; i < n; ++i) {
        lpBuffer[i] = (WCHAR)get16(text + 2 * i);
    }
    lpBuffer[n] = 0;
    return n;
}

int WINAPI LoadStringA(HINSTANCE hInstance, UINT uID, LPSTR lpBuffer, int cchBufferMax) {
    u32 length = 0;
    const unsigned char *text = find_string(hInstance, uID, &length);
    if (lpBuffer == NULL || cchBufferMax <= 0) {
        return 0;
    }
    if (text == 0) {
        lpBuffer[0] = 0;
        set_last_error(ERROR_RESOURCE_NAME_NOT_FOUND);
        return 0;
    }
    int n = (int)length < cchBufferMax - 1 ? (int)length : cchBufferMax - 1;
    for (int i = 0; i < n; ++i) {
        lpBuffer[i] = (char)sxw_wide_to_ansi((unsigned short)get16(text + 2 * i));
    }
    lpBuffer[n] = 0;
    return n;
}

/* Icons and cursors: the image chosen from a group, kept as the resource it
 * is (a DIB with its AND mask under it; a cursor's starts with its hot
 * spot). The system's own, by id with a NULL instance, carry no image. */
typedef struct icon {
    int used;
    int cursor;
    int width;
    int height;
    const unsigned char *data;
    u32 size;
} icon;

static icon g_icons[MAX_ICONS];

#define SYSTEM_CURSOR(id) ((HCURSOR)(0x00010000u | (id)))
#define SYSTEM_ICON(id) ((HICON)(0x00030000u | (id)))

static HICON icon_handle(int slot) {
    return (HICON)(0x00020000u | (u32)(slot + 1) * 4u);
}

/* The group's image nearest width x height, deepest first among equals. */
static HICON load_group(u32 base, LPCWSTR name, int cursor, int width, int height) {
    u32 size = 0;
    const unsigned char *group = find_resource(base, cursor ? RT_GROUP_CURSOR : RT_GROUP_ICON, name, &size);
    if (group == 0 || size < 6u) {
        set_last_error(ERROR_RESOURCE_NAME_NOT_FOUND);
        return NULL;
    }
    const u32 count = get16(group + 4);
    int best = -1;
    long best_score = 0;
    for (u32 i = 0; i < count && 6u + 14u * (i + 1u) <= size; ++i) {
        const unsigned char *e = group + 6 + 14 * i;
        /* An icon entry has byte width and height; a cursor's are words, the
         * height doubled. */
        const int w = cursor ? (int)get16(e) : (e[0] == 0 ? 256 : e[0]);
        const int h = cursor ? (int)get16(e + 2) / 2 : (e[1] == 0 ? 256 : e[1]);
        const int depth = (int)get16(e + 6);
        const long distance = (long)(w - width) * (w - width) + (long)(h - height) * (h - height);
        const long score = distance * 64 - depth;
        if (best < 0 || score < best_score) {
            best = (int)i;
            best_score = score;
        }
    }
    if (best < 0) {
        set_last_error(ERROR_RESOURCE_DATA_NOT_FOUND);
        return NULL;
    }
    const u32 id = get16(group + 6 + 14 * (u32)best + 12);
    u32 data_size = 0;
    const unsigned char *data =
        find_resource(base, cursor ? RT_CURSOR : RT_ICON, (LPCWSTR)(ULONG_PTR)id, &data_size);
    const u32 header = cursor ? 4u : 0u;
    if (data == 0 || data_size < header + 40u) {
        set_last_error(ERROR_RESOURCE_DATA_NOT_FOUND);
        return NULL;
    }
    for (int i = 0; i < MAX_ICONS; ++i) {
        icon *c = &g_icons[i];
        if (!c->used) {
            c->used = 1;
            c->cursor = cursor;
            c->width = (int)get32(data + header + 4);
            c->height = (int)get32(data + header + 8) / 2;
            c->data = data;
            c->size = data_size;
            return icon_handle(i);
        }
    }
    set_last_error(ERROR_NOT_ENOUGH_MEMORY);
    return NULL;
}

/* The system cursors and icons by id. */
static HANDLE system_image(LPCWSTR name, int cursor) {
    const u32 id = (u32)name;
    if (id < 32512u || id > 32650u) {
        set_last_error(ERROR_RESOURCE_NAME_NOT_FOUND);
        return NULL;
    }
    return cursor ? (HANDLE)SYSTEM_CURSOR(id) : (HANDLE)SYSTEM_ICON(id);
}

HCURSOR WINAPI LoadCursorW(HINSTANCE hInstance, LPCWSTR lpCursorName) {
    return hInstance == NULL ? (HCURSOR)system_image(lpCursorName, 1)
                             : (HCURSOR)load_group((u32)hInstance, lpCursorName, 1, 32, 32);
}

HICON WINAPI LoadIconW(HINSTANCE hInstance, LPCWSTR lpIconName) {
    return hInstance == NULL ? (HICON)system_image(lpIconName, 0)
                             : load_group((u32)hInstance, lpIconName, 0, 32, 32);
}

/* Bitmaps, icons and cursors from the module; not from files. A size of 0
 * is the image's own for a bitmap and 32x32 for the others. */
HANDLE WINAPI LoadImageW(HINSTANCE hInst, LPCWSTR name, UINT type, int cx, int cy, UINT fuLoad) {
    if ((fuLoad & ~(UINT)(LR_DEFAULTSIZE | LR_SHARED)) != 0u) {
        set_last_error(ERROR_INVALID_PARAMETER);
        return NULL;
    }
    switch (type) {
    case IMAGE_BITMAP:
        return load_bitmap(hInst, name);
    case IMAGE_ICON:
    case IMAGE_CURSOR:
        if (hInst == NULL) {
            return system_image(name, type == IMAGE_CURSOR);
        }
        return load_group((u32)hInst, name, type == IMAGE_CURSOR, cx != 0 ? cx : 32, cy != 0 ? cy : 32);
    default:
        set_last_error(ERROR_INVALID_PARAMETER);
        return NULL;
    }
}

/* Accelerator tables: the resource itself, eight bytes an entry (flags, key,
 * command, padding), 0x80 in the flags of the last. */
typedef struct accelerators {
    int used;
    const unsigned char *entries;
    u32 count;
} accelerators;

static accelerators g_accelerators[MAX_ACCELERATORS];

HACCEL WINAPI LoadAcceleratorsW(HINSTANCE hInstance, LPCWSTR lpTableName) {
    u32 size = 0;
    const unsigned char *table = find_resource(module_of(hInstance), RT_ACCELERATOR, lpTableName, &size);
    if (table == 0) {
        set_last_error(ERROR_RESOURCE_NAME_NOT_FOUND);
        return NULL;
    }
    u32 count = 0;
    while (8u * (count + 1u) <= size) {
        ++count;
        if ((table[8u * (count - 1u)] & 0x80u) != 0) {
            break;
        }
    }
    for (int i = 0; i < MAX_ACCELERATORS; ++i) {
        if (!g_accelerators[i].used) {
            g_accelerators[i].used = 1;
            g_accelerators[i].entries = table;
            g_accelerators[i].count = count;
            return (HACCEL)(0x00040000u | (u32)(i + 1) * 4u);
        }
    }
    set_last_error(ERROR_NOT_ENOUGH_MEMORY);
    return NULL;
}

/* A key message that matches an entry becomes WM_COMMAND (the high word 1,
 * "from an accelerator") sent to the window. Virtual-key entries match
 * WM_KEYDOWN and WM_SYSKEYDOWN with exactly their Shift, Ctrl and Alt;
 * character entries match WM_CHAR and WM_SYSCHAR. */
int WINAPI TranslateAcceleratorW(HWND hWnd, HACCEL hAccTable, LPMSG lpMsg) {
    const u32 slot = (((u32)hAccTable & 0xffffu) / 4u) - 1u;
    if (((u32)hAccTable >> 16) != 4u || slot >= MAX_ACCELERATORS || !g_accelerators[slot].used || lpMsg == NULL) {
        set_last_error(ERROR_INVALID_PARAMETER);
        return 0;
    }
    window *w = checked(hWnd);
    if (w == 0) {
        return 0;
    }
    const UINT m = lpMsg->message;
    const int key_message = m == WM_KEYDOWN || m == WM_SYSKEYDOWN;
    const int char_message = m == WM_CHAR || m == WM_SYSCHAR;
    if (!key_message && !char_message) {
        return 0;
    }
    const accelerators *a = &g_accelerators[slot];
    for (u32 i = 0; i < a->count; ++i) {
        const unsigned char *e = a->entries + 8u * i;
        const u32 flags = e[0];
        const u32 key = get16(e + 2);
        const u32 command = get16(e + 4);
        if ((flags & FVIRTKEY) != 0) {
            if (!key_message || key != (lpMsg->wParam & 0xffffu)) {
                continue;
            }
            const int shift = GetKeyState(VK_SHIFT) < 0;
            const int control = GetKeyState(VK_CONTROL) < 0;
            const int alt = GetKeyState(VK_MENU) < 0;
            if (shift != ((flags & FSHIFT) != 0) || control != ((flags & FCONTROL) != 0) ||
                alt != ((flags & FALT) != 0)) {
                continue;
            }
        } else {
            if (!char_message || key != (lpMsg->wParam & 0xffffu) ||
                ((flags & FALT) != 0) != (m == WM_SYSCHAR)) {
                continue;
            }
        }
        send(w, WM_COMMAND, MAKELPARAM(command, 1), 0);
        return 1;
    }
    return 0;
}

/* ---- DefWindowProc --------------------------------------------------------------- */

DWORD WINAPI GetSysColor(int nIndex);

static void erase_with_class_brush(window *w, HDC dc) {
    HBRUSH brush = w->cls->background;
    HBRUSH made = NULL;
    if ((u32)brush >= 1u && (u32)brush <= 31u) {
        brush = made = CreateSolidBrush(GetSysColor((int)(u32)brush - 1));
    }
    HGDIOBJ old = SelectObject(dc, brush);
    PatBlt(dc, 0, 0, client_width(w), client_height(w), PATCOPY);
    SelectObject(dc, old);
    if (made != NULL) {
        DeleteObject(made);
    }
}

LRESULT WINAPI DefWindowProcW(HWND hWnd, UINT Msg, WPARAM wParam, LPARAM lParam) {
    window *w = window_of(hWnd);
    if (w == 0) {
        return 0;
    }
    switch (Msg) {
    case WM_NCCREATE: {
        const CREATESTRUCTW *cs = (const CREATESTRUCTW *)lParam;
        if (cs != NULL && cs->lpszName != NULL && (u32)cs->lpszName >= 0x10000u) {
            copy_text(w->text, cs->lpszName, MAX_TEXT);
        }
        return TRUE;
    }
    case WM_NCCALCSIZE:
        if (!wParam && lParam != 0) {
            RECT *r = (RECT *)lParam;
            if (w->parent == 0) {
                int f[4];
                frame_of(w->style, 0, f);
                r->left += f[0];
                r->top += f[1];
                r->right -= f[2];
                r->bottom -= f[3];
            } else if ((w->style & WS_BORDER) != 0) {
                r->left += 1;
                r->top += 1;
                r->right -= 1;
                r->bottom -= 1;
            }
        }
        return 0;
    case WM_NCHITTEST:
        return HTCLIENT;
    case WM_NCACTIVATE:
        return TRUE;
    case WM_ACTIVATE:
        if (LOWORD(wParam) != WA_INACTIVE && !IsIconic(hWnd)) {
            SetFocus(hWnd);
        }
        return 0;
    case WM_MOUSEACTIVATE:
        if (w->parent != 0) {
            const LRESULT answer = send(w->parent, WM_MOUSEACTIVATE, wParam, lParam);
            if (answer != 0) {
                return answer;
            }
        }
        return MA_ACTIVATE;
    case WM_SETCURSOR:
        if (w->parent != 0 && send(w->parent, WM_SETCURSOR, wParam, lParam)) {
            return TRUE;
        }
        return LOWORD(lParam) == HTCLIENT && w->cls->cursor != NULL;
    case WM_ERASEBKGND:
        if (w->cls->background == NULL) {
            return 0;
        }
        erase_with_class_brush(w, (HDC)wParam);
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        BeginPaint(hWnd, &ps);
        EndPaint(hWnd, &ps);
        return 0;
    }
    case WM_WINDOWPOSCHANGED: {
        const WINDOWPOS *pos = (const WINDOWPOS *)lParam;
        if (w->need_size && (w->style & WS_VISIBLE) != 0) {
            w->need_size = 0;
            send_size_move(w);
        }
        (void)pos;
        return 0;
    }
    case WM_CLOSE:
        DestroyWindow(hWnd);
        return 0;
    case WM_SYSCOMMAND:
        if ((wParam & 0xfff0u) == SC_CLOSE) {
            send(w, WM_CLOSE, 0, 0);
        }
        return 0;
    case WM_SYSKEYDOWN:
        if (wParam == VK_F4 && (lParam & (1L << 29)) != 0) {
            send(top_of(w), WM_SYSCOMMAND, SC_CLOSE, 0);
        }
        return 0;
    case WM_RBUTTONUP:
        send(w, 0x007b /* WM_CONTEXTMENU */, (WPARAM)hWnd, MAKELPARAM(g_last_point.x, g_last_point.y));
        return 0;
    case 0x007b: /* WM_CONTEXTMENU */
        if (w->parent != 0) {
            send(w->parent, Msg, wParam, lParam);
        }
        return 0;
    case WM_GETTEXTLENGTH: {
        int n = 0;
        while (w->text[n] != 0) {
            ++n;
        }
        return n;
    }
    case WM_GETTEXT:
        if (lParam == 0 || wParam == 0) {
            return 0;
        }
        return copy_text((WCHAR *)lParam, w->text, (int)wParam);
    case WM_SETTEXT:
        copy_text(w->text, (const WCHAR *)lParam, MAX_TEXT);
        if (w->host != 0) {
            u32 arguments[2] = {w->host, (u32)w->text};
            SxwService(SXW_SVC_WINDOW_TITLE, arguments);
        }
        return TRUE;
    default:
        return 0;
    }
}

/* ---- Rectangles ------------------------------------------------------------------ */

BOOL WINAPI CopyRect(LPRECT lprcDst, const RECT *lprcSrc) {
    if (lprcDst == NULL || lprcSrc == NULL) {
        return FALSE;
    }
    *lprcDst = *lprcSrc;
    return TRUE;
}

BOOL WINAPI InflateRect(LPRECT lprc, int dx, int dy) {
    if (lprc == NULL) {
        return FALSE;
    }
    lprc->left -= dx;
    lprc->top -= dy;
    lprc->right += dx;
    lprc->bottom += dy;
    return TRUE;
}

BOOL WINAPI IntersectRect(LPRECT lprcDst, const RECT *lprcSrc1, const RECT *lprcSrc2) {
    if (lprcDst == NULL || lprcSrc1 == NULL || lprcSrc2 == NULL) {
        return FALSE;
    }
    sxw_rect a = {lprcSrc1->left, lprcSrc1->top, lprcSrc1->right, lprcSrc1->bottom};
    sxw_rect b = {lprcSrc2->left, lprcSrc2->top, lprcSrc2->right, lprcSrc2->bottom};
    sxw_rect r;
    const int any = sxw_rect_intersect(&r, &a, &b);
    lprcDst->left = r.left;
    lprcDst->top = r.top;
    lprcDst->right = r.right;
    lprcDst->bottom = r.bottom;
    return any;
}

BOOL WINAPI PtInRect(const RECT *lprc, POINT pt) {
    return lprc != NULL && pt.x >= lprc->left && pt.x < lprc->right && pt.y >= lprc->top && pt.y < lprc->bottom;
}

/* ---- Drawing helpers ------------------------------------------------------------- */

/* The classic Windows XP colours, the ones its reference captures show. */
DWORD WINAPI GetSysColor(int nIndex) {
    static const COLORREF k_colors[31] = {
        RGB(212, 208, 200), RGB(58, 110, 165), RGB(10, 36, 106),    RGB(128, 128, 128), RGB(212, 208, 200),
        RGB(255, 255, 255), RGB(0, 0, 0),      RGB(0, 0, 0),        RGB(0, 0, 0),       RGB(255, 255, 255),
        RGB(212, 208, 200), RGB(212, 208, 200), RGB(128, 128, 128), RGB(10, 36, 106),   RGB(255, 255, 255),
        RGB(212, 208, 200), RGB(128, 128, 128), RGB(128, 128, 128), RGB(0, 0, 0),       RGB(212, 208, 200),
        RGB(255, 255, 255), RGB(64, 64, 64),    RGB(212, 208, 200), RGB(0, 0, 0),       RGB(255, 255, 225),
        RGB(181, 181, 181), RGB(0, 0, 128),     RGB(166, 202, 240), RGB(192, 192, 192), RGB(49, 106, 197),
        RGB(236, 233, 216),
    };
    return nIndex >= 0 && nIndex < 31 ? k_colors[nIndex] : 0;
}

int WINAPI FrameRect(HDC hDC, const RECT *lprc, HBRUSH hbr) {
    if (lprc == NULL || lprc->right <= lprc->left || lprc->bottom <= lprc->top) {
        return 0;
    }
    HGDIOBJ old = SelectObject(hDC, hbr);
    if (old == NULL) {
        return 0;
    }
    const int width = lprc->right - lprc->left;
    const int height = lprc->bottom - lprc->top;
    PatBlt(hDC, lprc->left, lprc->top, 1, height, PATCOPY);
    PatBlt(hDC, lprc->right - 1, lprc->top, 1, height, PATCOPY);
    PatBlt(hDC, lprc->left, lprc->top, width, 1, PATCOPY);
    PatBlt(hDC, lprc->left, lprc->bottom - 1, width, 1, PATCOPY);
    SelectObject(hDC, old);
    return 1;
}

BOOL WINAPI InvertRect(HDC hDC, const RECT *lprc) {
    if (lprc == NULL) {
        return FALSE;
    }
    return PatBlt(hDC, lprc->left, lprc->top, lprc->right - lprc->left, lprc->bottom - lprc->top, DSTINVERT);
}

/* One line of DrawTextW, '&' handled unless DT_NOPREFIX: its width, and
 * drawn at (x, y) when draw is set. */
static int text_line(HDC dc, const WCHAR *text, int length, UINT format, int x, int y, int draw) {
    WCHAR shown[MAX_TEXT];
    int n = 0;
    int underline = -1;
    for (int i = 0; i < length && n < MAX_TEXT; ++i) {
        if ((format & DT_NOPREFIX) == 0 && text[i] == '&') {
            if (i + 1 < length && text[i + 1] == '&') {
                shown[n++] = '&';
                ++i;
            } else if (i + 1 < length) {
                underline = n;
            }
            continue;
        }
        shown[n++] = text[i];
    }
    SIZE size;
    GetTextExtentPoint32W(dc, shown, n, &size);
    if (draw) {
        TextOutW(dc, x, y, shown, n);
        if (underline >= 0) {
            SIZE before;
            SIZE glyph;
            GetTextExtentPoint32W(dc, shown, underline, &before);
            GetTextExtentPoint32W(dc, shown + underline, 1, &glyph);
            TEXTMETRICW tm;
            GetTextMetricsW(dc, &tm);
            HGDIOBJ old = SelectObject(dc, GetStockObject(BLACK_BRUSH));
            PatBlt(dc, x + before.cx, y + tm.tmAscent + 1, glyph.cx - 1, 1, PATCOPY);
            SelectObject(dc, old);
        }
    }
    return size.cx;
}

/* Lines split at '\n' (and at blanks with DT_WORDBREAK), aligned in the
 * rectangle. The text is not clipped to it. */
int WINAPI DrawTextW(HDC hdc, LPCWSTR lpchText, int cchText, LPRECT lprc, UINT format) {
    if (lpchText == NULL || lprc == NULL) {
        return 0;
    }
    if (cchText < 0) {
        cchText = lstrlenW(lpchText);
    }
    TEXTMETRICW tm;
    GetTextMetricsW(hdc, &tm);
    const int line_height = tm.tmHeight;
    const int width = lprc->right - lprc->left;
    /* Line breaks first, so the height is known before vertical alignment. */
    int starts[64];
    int lengths[64];
    int lines = 0;
    int widest = 0;
    for (int i = 0; i <= cchText && lines < 64;) {
        int end = i;
        while (end < cchText && lpchText[end] != '\n' && ((format & DT_SINGLELINE) != 0 || lpchText[end] != '\r')) {
            ++end;
        }
        if ((format & DT_SINGLELINE) != 0) {
            end = cchText;
        }
        int length = end - i;
        if ((format & DT_WORDBREAK) != 0 && (format & DT_SINGLELINE) == 0) {
            while (length > 0 && text_line(hdc, lpchText + i, length, format, 0, 0, 0) > width) {
                int cut = length - 1;
                while (cut > 0 && lpchText[i + cut] != ' ') {
                    --cut;
                }
                if (cut == 0) {
                    break;
                }
                length = cut;
            }
        }
        starts[lines] = i;
        lengths[lines] = length;
        const int w = text_line(hdc, lpchText + i, length, format, 0, 0, 0);
        widest = w > widest ? w : widest;
        ++lines;
        i += length;
        while (i < cchText && (lpchText[i] == ' ' && length != end - starts[lines - 1])) {
            ++i;
        }
        if (i < cchText && lpchText[i] == '\r') {
            ++i;
        }
        if (i < cchText && lpchText[i] == '\n') {
            ++i;
        } else if (i >= cchText) {
            break;
        }
    }
    const int height = lines * line_height;
    if ((format & DT_CALCRECT) != 0) {
        lprc->right = lprc->left + widest;
        lprc->bottom = lprc->top + height;
        return height;
    }
    int y = lprc->top;
    if ((format & DT_SINGLELINE) != 0) {
        if ((format & DT_VCENTER) != 0) {
            y = lprc->top + (lprc->bottom - lprc->top - line_height) / 2;
        } else if ((format & DT_BOTTOM) != 0) {
            y = lprc->bottom - line_height;
        }
    }
    for (int l = 0; l < lines; ++l) {
        const int w = text_line(hdc, lpchText + starts[l], lengths[l], format, 0, 0, 0);
        int x = lprc->left;
        if ((format & DT_CENTER) != 0) {
            x = lprc->left + (width - w) / 2;
        } else if ((format & DT_RIGHT) != 0) {
            x = lprc->right - w;
        }
        text_line(hdc, lpchText + starts[l], lengths[l], format, x, y, 1);
        y += line_height;
    }
    return height;
}
