/*
 * user32.dll -- Windows and messages (docs/userland/WIN32.md, "user32 and
 * gdi32"): window classes, the window tree, the message queue, painting,
 * timers, input, capture, focus and activation, and DefWindowProc, for the
 * functions of the W0 matrix, and the resources a program loads: bitmaps,
 * strings, icons, cursors and accelerator tables. Menus are menu.c's,
 * dialogs dialog.c's and the system's controls controls.c's; what they share
 * is in user32_private.h.
 *
 * A top-level window is a host window (SXW_SVC_WINDOW_CREATE) whose frame
 * and caption are the host's; its menu bar and client area are a surface in
 * guest memory that every window of the tree draws on through gdi32. Child
 * windows exist only here. One thread: a message is "sent" by calling the
 * window procedure.
 *
 * The order of the messages each call sends is Windows XP's, as
 * fixtures/window recorded it there.
 */
#include "user32_private.h"

#include "codepage.h"
#include "resource.h"

#define MAX_CLASSES 64
#define MAX_TIMERS 32
#define MAX_POSTED 256
#define MAX_INPUT 128
#define MAX_REGISTERED 128
#define DOUBLE_CLICK_MS 500u
#define DOUBLE_CLICK_DISTANCE 2 /* half of SM_CXDOUBLECLK */
#define DESKTOP_HANDLE ((HWND)0x00010010)
#define HTMENU 5
#define WAIT_OBJECT_0 0u
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

void set_last_error(DWORD error) {
    __asm__ volatile("movl %0, %%fs:%c1" : : "r"(error), "i"(SXW_TEB_LAST_ERROR) : "memory");
}

u32 ticks(void) {
    return SxwService(SXW_SVC_TICKS, 0);
}

void zero(void *p, u32 size) {
    unsigned char *b = p;
    for (u32 i = 0; i < size; ++i) {
        b[i] = 0;
    }
}

int same_name(const WCHAR *a, const WCHAR *b) {
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

int copy_text(WCHAR *out, const WCHAR *text, int capacity) {
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

int text_length(const WCHAR *text) {
    int n = 0;
    while (text != NULL && text[n] != 0) {
        ++n;
    }
    return n;
}

/* ---- Classes ---------------------------------------------------------------- */

static wclass g_classes[MAX_CLASSES];
static int g_system_classes;

wclass *find_class(LPCWSTR name) {
    if (!g_system_classes) {
        g_system_classes = 1;
        register_controls();
    }
    for (int i = 0; i < MAX_CLASSES; ++i) {
        wclass *c = &g_classes[i];
        if (c->used && ((u32)name < 0x10000u ? c->atom == (ATOM)(u32)name : same_name(c->name, name))) {
            return c;
        }
    }
    return 0;
}

static ATOM register_class(UINT style, WNDPROC proc, int window_extra, HINSTANCE instance, HICON icon,
                           HICON small_icon, HCURSOR cursor, HBRUSH background, LPCWSTR name,
                           LPCWSTR menu_name) {
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
            c->small_icon = small_icon;
            c->cursor = cursor;
            c->background = background;
            if ((u32)menu_name >= 0x10000u) {
                copy_text(c->menu_text, menu_name, 64);
                c->menu_name = c->menu_text;
            } else {
                c->menu_name = menu_name;
            }
            return c->atom;
        }
    }
    set_last_error(ERROR_NOT_ENOUGH_MEMORY);
    return 0;
}

wclass *class_of_atom(ATOM atom) {
    for (int i = 0; i < MAX_CLASSES; ++i) {
        if (g_classes[i].used && g_classes[i].atom == atom) {
            return &g_classes[i];
        }
    }
    return 0;
}

void remove_class(wclass *c) {
    c->used = 0;
}

/* One of the system's classes (controls.c), with the atom, style and
 * window bytes Windows XP gives it. */
ATOM register_system_class(UINT style, WNDPROC proc, LPCWSTR name, ATOM atom, HBRUSH background, int extra) {
    const ATOM made = register_class(style, proc, extra, NULL, NULL, NULL, NULL, background, name, NULL);
    for (int i = 0; i < MAX_CLASSES && made != 0; ++i) {
        if (g_classes[i].used && g_classes[i].atom == made) {
            g_classes[i].atom = atom;
            g_classes[i].cursor = (HCURSOR)1;
        }
    }
    return made != 0 ? atom : 0;
}

ATOM WINAPI RegisterClassW(const WNDCLASSW *lpWndClass) {
    return register_class(lpWndClass->style, lpWndClass->lpfnWndProc, lpWndClass->cbWndExtra,
                          lpWndClass->hInstance, lpWndClass->hIcon, NULL, lpWndClass->hCursor,
                          lpWndClass->hbrBackground, lpWndClass->lpszClassName, lpWndClass->lpszMenuName);
}

ATOM WINAPI RegisterClassExW(const WNDCLASSEXW *lpWndClass) {
    if (lpWndClass->cbSize != sizeof(WNDCLASSEXW)) {
        set_last_error(ERROR_INVALID_PARAMETER);
        return 0;
    }
    return register_class(lpWndClass->style, lpWndClass->lpfnWndProc, lpWndClass->cbWndExtra,
                          lpWndClass->hInstance, lpWndClass->hIcon, lpWndClass->hIconSm, lpWndClass->hCursor,
                          lpWndClass->hbrBackground, lpWndClass->lpszClassName, lpWndClass->lpszMenuName);
}

/* ---- Windows ----------------------------------------------------------------- */

window g_windows[MAX_WINDOWS];
static u32 g_generation = 1;
window *g_active;
window *g_focus;
window *g_capture;
window *g_focus_target; /* where SetFocus is putting the focus while it activates */
int g_keyboard_last = 1; /* as if started from the keyboard, as a command line starts it */

HWND handle_of(const window *w) {
    return w == 0 ? NULL : (HWND)((w->generation << 16) | (u32)((w - g_windows) + 1) * 4u);
}

window *window_of(HWND h) {
    const u32 v = (u32)h;
    const u32 index = (v & 0xffffu) / 4u;
    if ((v & 3u) != 0 || index == 0 || index > MAX_WINDOWS) {
        return 0;
    }
    window *w = &g_windows[index - 1u];
    return w->used && w->generation == (v >> 16) ? w : 0;
}

window *checked(HWND h) {
    window *w = window_of(h);
    if (w == 0) {
        set_last_error(ERROR_INVALID_WINDOW_HANDLE);
    }
    return w;
}

window *top_of(window *w) {
    while (w->parent != 0) {
        w = w->parent;
    }
    return w;
}

/* A window procedure called straight: DispatchMessage, CallWindowProc. */
LRESULT call_proc(window *w, UINT message, WPARAM wParam, LPARAM lParam) {
    return w != 0 && w->proc != NULL ? w->proc(handle_of(w), message, wParam, lParam) : 0;
}

/* A message sent: the window procedure hooks see it before and after, as
 * they see every SendMessage on Windows (fixtures/hooks). wParam of the
 * hook is 0 for a message from the window's own thread, as on Windows XP. */
LRESULT send(window *w, UINT message, WPARAM wParam, LPARAM lParam) {
    if (w == 0 || w->proc == NULL) {
        return 0;
    }
    const HWND h = handle_of(w);
    if (g_hook_count != 0 && hook_installed(WH_CALLWNDPROC)) {
        CWPSTRUCT c = {lParam, wParam, message, h};
        call_hook(WH_CALLWNDPROC, HC_ACTION, 0, (LPARAM)&c);
    }
    const LRESULT result = w->proc(h, message, wParam, lParam);
    if (g_hook_count != 0 && hook_installed(WH_CALLWNDPROCRET)) {
        CWPRETSTRUCT c = {result, lParam, wParam, message, h};
        call_hook(WH_CALLWNDPROCRET, HC_ACTION, 0, (LPARAM)&c);
    }
    return result;
}

LRESULT send_handle(HWND h, UINT message, WPARAM wParam, LPARAM lParam) {
    return send(window_of(h), message, wParam, lParam);
}

int visible_chain(const window *w) {
    for (; w != 0; w = w->parent) {
        if ((w->style & WS_VISIBLE) == 0) {
            return 0;
        }
    }
    return 1;
}

/* A top-level popup without a caption, owned by another window (a combo
 * box's list, a tooltip): its frame is not the window manager's, so its
 * surface holds the whole window, frame and scroll bars included. */
int own_frame(const window *w) {
    return w->parent == 0 && w->owner != 0 && !w->message_only && (w->style & WS_POPUP) != 0 &&
           (w->style & WS_CAPTION) != WS_CAPTION;
}

/* Where a window's client area starts on its top-level window's surface. */
void client_origin(const window *w, int *x, int *y) {
    *x = 0;
    *y = 0;
    for (; w->parent != 0; w = w->parent) {
        *x += w->client.left;
        *y += w->client.top;
    }
    *y += w->menu_height;
    if (own_frame(w)) {
        *x += w->client.left - w->rect.left;
        *y += w->client.top - w->rect.top;
    }
}

int client_width(const window *w) {
    return w->client.right - w->client.left;
}

int client_height(const window *w) {
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

/* Which window each device context user32 made is on, for WindowFromDC. */
#define MAX_DCS 64
static struct {
    HDC dc;
    HWND hwnd;
} g_dcs[MAX_DCS];
static int g_dc_next;

static void remember_dc(HDC dc, window *w) {
    g_dcs[g_dc_next].dc = dc;
    g_dcs[g_dc_next].hwnd = handle_of(w);
    g_dc_next = (g_dc_next + 1) % MAX_DCS;
}

/* A device context on a window's client area, clipped to clip (client
 * coordinates) when it is given. */
HDC window_dc(window *w, const sxw_region *clip) {
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
    HDC dc = (HDC)SxwCreateSurfaceDC(&top->surface, ox, oy, &visible);
    remember_dc(dc, w);
    return dc;
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
    out[1] = edge + ((style & WS_CAPTION) == WS_CAPTION ? METRIC_CAPTION : 0) + (menu ? METRIC_MENU : 0);
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

int size_surface(window *top, int width, int height) {
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
void present_all(void) {
    for (int i = 0; i < MAX_WINDOWS; ++i) {
        present_window(&g_windows[i]);
    }
}

void present_window(window *w) {
    {
        if (!w->used || w->parent != 0 || w->host == 0 || sxw_region_empty(&w->surface.dirty)) {
            return;
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
void invalidate(window *w, const sxw_rect *rect, int erase, int children) {
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

/* Shown, moved or resized: the frame of the window and of its children is
 * painted again (WM_NCPAINT) before their next WM_PAINT. */
void invalidate_frame(window *w) {
    w->frame_dirty = 1;
    for (window *c = w->child; c != 0; c = c->next) {
        invalidate_frame(c);
    }
}

/* The thickness of a child's non-client area, left, top, right, bottom:
 * its edges (WS_BORDER one pixel of COLOR_WINDOWFRAME, WS_EX_STATICEDGE
 * one sunken, WS_EX_CLIENTEDGE two, a dialog frame three) and its scroll
 * bars, as DefWindowProc's WM_NCCALCSIZE gives it on Windows XP. */
void frame_of_child(const window *w, int window_width, int window_height, int out[4]) {
    int edge = 0;
    if ((w->style & WS_THICKFRAME) != 0) {
        edge += 4;
    } else if ((w->style & WS_DLGFRAME) != 0 || (w->ex_style & WS_EX_DLGMODALFRAME) != 0) {
        edge += 3;
    } else if ((w->style & WS_BORDER) != 0) {
        edge += 1;
    }
    if ((w->ex_style & WS_EX_CLIENTEDGE) != 0) {
        edge += 2;
    }
    if ((w->ex_style & WS_EX_STATICEDGE) != 0) {
        edge += 1;
    }
    out[0] = out[1] = out[2] = out[3] = edge;
    const int width = window_width - 2 * edge;
    const int height = window_height - 2 * edge;
    if ((w->style & WS_VSCROLL) != 0 && width >= SCROLL_EXTENT) {
        out[2] += SCROLL_EXTENT;
    }
    if ((w->style & WS_HSCROLL) != 0 && height >= SCROLL_EXTENT) {
        out[3] += SCROLL_EXTENT;
    }
}

/* DefWindowProc's WM_NCPAINT for a child: its edges and scroll bars,
 * clipped like its client area. A top-level window's frame is the window
 * manager's. */
void nc_paint(window *w) {
    if ((w->parent == 0 && !own_frame(w)) || !visible_chain(w)) {
        return;
    }
    int px = 0;
    int py = 0;
    if (w->parent != 0) {
        client_origin(w->parent, &px, &py);
    } else {
        px = -w->rect.left;
        py = -w->rect.top;
    }
    sxw_rect frame = w->rect;
    sxw_rect_offset(&frame, px, py);
    sxw_rect inner = w->client;
    sxw_rect_offset(&inner, px, py);
    if (frame.left == inner.left && frame.top == inner.top && frame.right == inner.right &&
        frame.bottom == inner.bottom) {
        return;
    }
    sxw_region visible;
    sxw_region_set(&visible, &frame);
    sxw_region_subtract_rect(&visible, &inner);
    for (const window *a = w->parent; a != 0; a = a->parent) {
        int ax;
        int ay;
        client_origin(a, &ax, &ay);
        sxw_rect ar = {ax, ay, ax + client_width(a), ay + client_height(a)};
        sxw_region_intersect_rect(&visible, &ar);
    }
    if ((w->style & WS_CLIPSIBLINGS) != 0 && w->parent != 0) {
        for (const window *s = w->parent->child; s != 0 && s != w; s = s->next) {
            if ((s->style & WS_VISIBLE) != 0) {
                sxw_rect sr = s->rect;
                sxw_rect_offset(&sr, px, py);
                sxw_region_subtract_rect(&visible, &sr);
            }
        }
    }
    HDC dc = (HDC)SxwCreateSurfaceDC(&top_of(w)->surface, frame.left, frame.top, &visible);
    RECT r = {0, 0, frame.right - frame.left, frame.bottom - frame.top};
    if ((w->style & WS_THICKFRAME) != 0) {
        DrawEdge(dc, &r, EDGE_RAISED, BF_RECT | BF_ADJUST);
        FrameRect(dc, &r, system_brush(COLOR_BTNFACE));
        InflateRect(&r, -1, -1);
        FrameRect(dc, &r, system_brush(COLOR_BTNFACE));
        InflateRect(&r, -1, -1);
    } else if ((w->style & WS_DLGFRAME) != 0 || (w->ex_style & WS_EX_DLGMODALFRAME) != 0) {
        DrawEdge(dc, &r, EDGE_RAISED, BF_RECT | BF_ADJUST);
        FrameRect(dc, &r, system_brush(COLOR_BTNFACE));
        InflateRect(&r, -1, -1);
    } else if ((w->style & WS_BORDER) != 0) {
        FrameRect(dc, &r, system_brush(COLOR_WINDOWFRAME));
        InflateRect(&r, -1, -1);
    }
    if ((w->ex_style & WS_EX_CLIENTEDGE) != 0) {
        DrawEdge(dc, &r, EDGE_SUNKEN, BF_RECT | BF_ADJUST);
    }
    if ((w->ex_style & WS_EX_STATICEDGE) != 0) {
        DrawEdge(dc, &r, BDR_SUNKENOUTER, BF_RECT | BF_ADJUST);
    }
    nc_paint_scroll_bars(w, dc, w->client.left - w->rect.left, w->client.top - w->rect.top);
    DeleteObject(dc);
}

/* WM_ERASEBKGND for the invalid part; 1 when the window erased it. */
static int erase_background(window *w) {
    HDC dc = window_dc(w, &w->invalid);
    const int erased = send(w, WM_ERASEBKGND, (WPARAM)dc, 0) != 0;
    DeleteObject(dc);
    return erased;
}

/* What showing a window paints at once (RDW_ERASENOW): the frame and the
 * background of the window, then of its children down to depth levels.
 * Windows XP leaves grandchildren to their own WM_PAINT (fixtures/hooks). */
void erase_now(window *w, int depth) {
    if (!visible_chain(w)) {
        return;
    }
    if (w->frame_dirty) {
        w->frame_dirty = 0;
        send(w, WM_NCPAINT, 1, 0);
    }
    if (w->erase && !sxw_region_empty(&w->invalid)) {
        w->erase = !erase_background(w);
    }
    if (depth > 0) {
        for (window *c = w->child; c != 0; c = c->next) {
            erase_now(c, depth - 1);
        }
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
    const u32 self = GetCurrentThreadId();
    for (int i = 0; i < MAX_WINDOWS; ++i) {
        window *w = &g_windows[i];
        if (w->used && w->parent == 0 && !w->destroying && w->thread == self) {
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
    caret_hide_for_paint(w);
    if (w->frame_dirty) {
        w->frame_dirty = 0;
        send(w, WM_NCPAINT, 1, 0);
    }
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
    if (lpPaint != NULL && lpPaint->hdc != NULL) {
        DeleteObject(lpPaint->hdc);
    }
    window *w = window_of(hWnd);
    if (w != 0) {
        caret_show_after_paint(w);
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

static void to_sxw(const RECT *r, sxw_rect *out) {
    out->left = r->left;
    out->top = r->top;
    out->right = r->right;
    out->bottom = r->bottom;
}

static void validate(window *w, const sxw_rect *rect, int children) {
    if (rect == 0) {
        sxw_region_clear(&w->invalid);
        w->erase = 0;
    } else {
        sxw_region_subtract_rect(&w->invalid, rect);
    }
    if (children) {
        for (window *c = w->child; c != 0; c = c->next) {
            if (rect == 0) {
                validate(c, 0, 1);
            } else {
                sxw_rect cr = *rect;
                sxw_rect_offset(&cr, -c->client.left, -c->client.top);
                validate(c, &cr, 1);
            }
        }
    }
}

BOOL WINAPI ValidateRect(HWND hWnd, const RECT *lpRect) {
    window *w = checked(hWnd);
    if (w == 0) {
        return FALSE;
    }
    sxw_rect r;
    if (lpRect != NULL) {
        to_sxw(lpRect, &r);
    }
    validate(w, lpRect != NULL ? &r : 0, (w->style & WS_CLIPCHILDREN) == 0);
    return TRUE;
}

/* A region is only ever the whole client area here: gdi32 has no regions
 * a program can make yet. */
BOOL WINAPI InvalidateRgn(HWND hWnd, HRGN hRgn, BOOL bErase) {
    (void)hRgn;
    return InvalidateRect(hWnd, NULL, bErase);
}

/* The bounds of the part waiting to be painted; with bErase, it is erased
 * first when it is waiting for that. */
BOOL WINAPI GetUpdateRect(HWND hWnd, LPRECT lpRect, BOOL bErase) {
    window *w = checked(hWnd);
    if (w == 0) {
        return FALSE;
    }
    if (bErase && w->erase && !sxw_region_empty(&w->invalid)) {
        w->erase = !erase_background(w);
    }
    const sxw_rect bounds = sxw_region_bounds(&w->invalid);
    if (lpRect != NULL) {
        lpRect->left = bounds.left;
        lpRect->top = bounds.top;
        lpRect->right = bounds.right;
        lpRect->bottom = bounds.bottom;
    }
    return !sxw_region_empty(&w->invalid);
}

static void validate_frames(window *w, int children) {
    w->frame_dirty = 0;
    for (window *c = w->child; children && c != 0; c = c->next) {
        validate_frames(c, 1);
    }
}

BOOL WINAPI RedrawWindow(HWND hWnd, const RECT *lprcUpdate, HRGN hrgnUpdate, UINT flags) {
    (void)hrgnUpdate;
    window *w = hWnd != NULL ? checked(hWnd) : 0;
    if (w == 0) {
        return hWnd == NULL;
    }
    const int children = (flags & RDW_ALLCHILDREN) != 0 ||
                         ((flags & RDW_NOCHILDREN) == 0 && (w->style & WS_CLIPCHILDREN) == 0);
    sxw_rect r;
    if (lprcUpdate != NULL) {
        to_sxw(lprcUpdate, &r);
    }
    if ((flags & RDW_INVALIDATE) != 0) {
        invalidate(w, lprcUpdate != NULL ? &r : 0, (flags & RDW_ERASE) != 0, children);
        if ((flags & RDW_FRAME) != 0) {
            w->frame_dirty = 1;
            if ((flags & RDW_ALLCHILDREN) != 0) {
                invalidate_frame(w);
            }
        }
    } else if ((flags & RDW_VALIDATE) != 0) {
        validate(w, lprcUpdate != NULL ? &r : 0, children);
        if ((flags & RDW_NOFRAME) != 0) {
            validate_frames(w, children);
        }
    }
    if ((flags & RDW_NOERASE) != 0) {
        w->erase = 0;
    }
    if ((flags & RDW_UPDATENOW) != 0) {
        if (children) {
            update_now(w);
        } else if (visible_chain(w) && !sxw_region_empty(&w->invalid)) {
            send(w, WM_PAINT, 0, 0);
        }
    } else if ((flags & RDW_ERASENOW) != 0) {
        erase_now(w, children ? 64 : 0);
    }
    return TRUE;
}

/* A device context on the whole window, its non-client area included. */
HDC WINAPI GetWindowDC(HWND hWnd) {
    window *w = checked(hWnd);
    if (w == 0) {
        return NULL;
    }
    if (w->parent == 0) {
        return window_dc(w, 0);
    }
    int px;
    int py;
    client_origin(w->parent, &px, &py);
    sxw_rect frame = w->rect;
    sxw_rect_offset(&frame, px, py);
    sxw_region visible;
    sxw_region_set(&visible, &frame);
    for (const window *a = w->parent; a != 0; a = a->parent) {
        int ax;
        int ay;
        client_origin(a, &ax, &ay);
        sxw_rect ar = {ax, ay, ax + client_width(a), ay + client_height(a)};
        sxw_region_intersect_rect(&visible, &ar);
    }
    if (!visible_chain(w)) {
        sxw_region_clear(&visible);
    }
    HDC dc = (HDC)SxwCreateSurfaceDC(&top_of(w)->surface, frame.left, frame.top, &visible);
    remember_dc(dc, w);
    return dc;
}

#define DCX_WINDOW 0x00000001u

HDC WINAPI GetDCEx(HWND hWnd, HRGN hrgnClip, DWORD flags) {
    (void)hrgnClip;
    return (flags & DCX_WINDOW) != 0 ? GetWindowDC(hWnd) : GetDC(hWnd);
}

HWND WINAPI WindowFromDC(HDC hDC) {
    for (int i = 0; i < MAX_DCS; ++i) {
        if (g_dcs[i].dc == hDC && hDC != NULL) {
            return window_of(g_dcs[i].hwnd) != 0 ? g_dcs[i].hwnd : NULL;
        }
    }
    return NULL;
}

/* Drawing is never deferred to a lock here: the window draws as it is
 * asked. */
BOOL WINAPI LockWindowUpdate(HWND hWndLock) {
    (void)hWndLock;
    return TRUE;
}

/* ---- Activation, focus and capture --------------------------------------------- */

HWND WINAPI SetFocus(HWND hWnd) {
    window *w = hWnd != NULL ? checked(hWnd) : 0;
    if (hWnd != NULL && w == 0) {
        return NULL;
    }
    window *top = w != 0 ? top_of(w) : 0;
    if (top != 0 && (top->style & WS_DISABLED) != 0) {
        return NULL;
    }
    if (w == g_focus) {
        return handle_of(g_focus);
    }
    /* The CBT hook hears it first and may refuse it; then the focus goes
     * with activation: its top-level window is activated, which may move
     * the focus itself. */
    if (g_hook_count != 0 && cbt_hook(HCBT_SETFOCUS, (WPARAM)hWnd, (LPARAM)handle_of(g_focus)) != 0) {
        return NULL;
    }
    if (top != 0 && top != g_active) {
        window *outer = g_focus_target;
        g_focus_target = w;
        activate(top);
        g_focus_target = outer;
        if (window_of(hWnd) == 0) {
            return NULL;
        }
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

int within(const window *w, const window *ancestor) {
    for (; w != 0; w = w->parent) {
        if (w == ancestor) {
            return 1;
        }
    }
    return 0;
}

/* The active window goes away: its owner, or another of the program's
 * windows, is activated; with none, the program stops being active. The
 * focus leaves it. */
void deactivate(window *w) {
    if (g_active == w) {
        window *next = 0;
        if (w->owner != 0 && w->owner->used && (w->owner->style & (WS_VISIBLE | WS_DISABLED)) == WS_VISIBLE) {
            next = w->owner;
        }
        for (int i = 0; i < MAX_WINDOWS && next == 0; ++i) {
            window *o = &g_windows[i];
            if (o != w && o->used && o->parent == 0 && !o->destroying && o->owner == 0 &&
                (o->style & (WS_VISIBLE | WS_DISABLED)) == WS_VISIBLE) {
                next = o;
            }
        }
        if (next != 0) {
            activate(next);
        } else {
            send(w, WM_NCACTIVATE, FALSE, 0);
            send(w, WM_ACTIVATE, WA_INACTIVE, 0);
            g_active = 0;
            send(w, WM_ACTIVATEAPP, FALSE, 0);
        }
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

/* A new child goes to the bottom of its siblings: the first one created is
 * on top, so a dialog's controls are in their template's order. */
static void link_child(window *parent, window *w) {
    link_child_at(parent, w, (window *)1);
}

/* Links w among parent's children: at the top when after is 0, at the
 * bottom when it is 1, else below the sibling after. */
void link_child_at(window *parent, window *w, window *after) {
    w->parent = parent;
    if (after == 0) {
        w->next = parent->child;
        parent->child = w;
        return;
    }
    window **link = &parent->child;
    while (*link != 0 && (after == (window *)1 || *link != after)) {
        link = &(*link)->next;
    }
    if (*link != 0) {
        link = &(*link)->next;
    }
    w->next = *link;
    *link = w;
}

void unlink_child(window *w) {
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
    /* Creation and destruction are told to the parent only, as Windows
     * XP tells them (fixtures/hooks); a click goes up the chain. */
    if (w->parent != 0) {
        send(w->parent, WM_PARENTNOTIFY, event == WM_CREATE || event == WM_DESTROY ? (event | (w->id << 16)) : event,
             lParam);
    }
}

static void free_window(window *w) {
    if (w->extra != 0) {
        SxwHeapFree(w->extra);
    }
    if (w->surface.pixels != 0) {
        SxwHeapFree(w->surface.pixels);
    }
    free_props(w);
    if (w->cls != 0 && w->cls->windows > 0) {
        --w->cls->windows;
    }
    w->used = 0;
}

void send_size_move(window *w) {
    const DWORD kind = (w->style & WS_MINIMIZE) != 0 ? SIZE_MINIMIZED : SIZE_RESTORED;
    send(w, WM_SIZE, kind, MAKELPARAM(client_width(w), client_height(w)));
    send(w, WM_MOVE, 0, MAKELPARAM(w->client.left, w->client.top));
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
    window *owner = 0;
    const int message_only = hWndParent == HWND_MESSAGE;
    if (message_only) {
        dwStyle &= ~(DWORD)(WS_CHILD | WS_VISIBLE);
    } else if ((dwStyle & WS_CHILD) != 0) {
        parent = checked(hWndParent);
        if (parent == 0) {
            return NULL;
        }
    } else if (hWndParent != NULL && hWndParent != DESKTOP_HANDLE) {
        owner = checked(hWndParent);
        if (owner == 0) {
            return NULL;
        }
        owner = top_of(owner);
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
    /* A framed window gets WS_EX_WINDOWEDGE, which the CBT hook already
     * sees, as on Windows XP. */
    if ((dwStyle & (WS_DLGFRAME | WS_THICKFRAME)) != 0 || (dwExStyle & WS_EX_DLGMODALFRAME) != 0 ||
        ((dwStyle & (WS_CHILD | WS_POPUP)) == 0 && !message_only)) {
        dwExStyle |= WS_EX_WINDOWEDGE;
    }
    zero(w, sizeof(*w));
    w->used = 1;
    w->generation = g_generation;
    g_generation = g_generation == 0x7fffu ? 1u : g_generation + 1u;
    w->cls = cls;
    w->proc = cls->proc;
    w->thread = GetCurrentThreadId();
    w->message_only = message_only;
    w->style = dwStyle & ~(DWORD)WS_VISIBLE;
    w->ex_style = dwExStyle;
    w->parent = parent;
    w->owner = owner;
    w->instance = hInstance;
    w->id = parent != 0 ? (UINT_PTR)hMenu : 0;
    w->ui_state = parent != 0 ? parent->ui_state : 0;
    w->extra_size = cls->window_extra;
    for (int b = 0; b < 2; ++b) {
        w->bars[b].max = 100;
    }
    if (cls->window_extra > 0) {
        w->extra = SxwHeapAllocate((u32)cls->window_extra, 1);
        if (w->extra == 0) {
            w->used = 0;
            set_last_error(ERROR_NOT_ENOUGH_MEMORY);
            return NULL;
        }
    }
    ++cls->windows;
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
            /* An overlapped window hears its first WM_SIZE and WM_MOVE when
             * it is first shown; a popup at once, as a child does. */
            w->need_size = 1;
        }
        w->z = next_z();
        w->menu = hMenu != NULL ? hMenu
                  : cls->menu_name != NULL ? menu_load(hInstance != NULL ? hInstance : cls->instance, cls->menu_name)
                                           : NULL;
        w->menu_height = w->menu != NULL ? MENU_BAR_HEIGHT : 0;
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
    cs.hwndParent = message_only ? NULL : hWndParent;
    cs.cy = nHeight;
    cs.cx = nWidth;
    cs.y = Y;
    cs.x = X;
    cs.style = (LONG)dwStyle;
    cs.lpszName = lpWindowName;
    cs.lpszClass = lpClassName;
    cs.dwExStyle = dwExStyle;

    if (g_hook_count != 0) {
        CBT_CREATEWNDW c = {&cs, HWND_TOP};
        if (cbt_hook(HCBT_CREATEWND, (WPARAM)hwnd, (LPARAM)&c) != 0) {
            /* Refused: the window hears only that it is gone. */
            send(w, WM_NCDESTROY, 0, 0);
            if (window_of(hwnd) != 0) {
                destroy_tree(w);
            }
            return NULL;
        }
        if (window_of(hwnd) == 0) {
            return NULL;
        }
    }
    if ((dwStyle & WS_THICKFRAME) != 0 || (dwStyle & (WS_POPUP | WS_CHILD)) == 0) {
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
    if (parent == 0 && !message_only &&
        !(own_frame(w) ? size_surface(w, w->rect.right - w->rect.left, w->rect.bottom - w->rect.top)
                       : size_surface(w, client_width(w), client_height(w) + w->menu_height))) {
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
    if (!w->need_size) {
        send_size_move(w);
    }
    if (parent != 0) {
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
    clipboard_window_gone(w);
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
    caret_window_gone(w);
    unlink_child(w);
    if (w->parent == 0 && w->menu != NULL) {
        menu_destroy(w->menu);
        w->menu = NULL;
    }
    dialog_free(w);
    if (w->host != 0) {
        u32 arguments[1] = {w->host};
        SxwService(SXW_SVC_WINDOW_DESTROY, arguments);
    }
    for (int i = 0; i < MAX_WINDOWS; ++i) {
        if (g_windows[i].used && g_windows[i].owner == w) {
            g_windows[i].owner = 0;
        }
    }
    free_window(w);
}

/* The windows a top-level window owns go before it, newest above first, as
 * Windows XP destroys them (fixtures/hooks). */
void destroy_owned(window *owner) {
    for (;;) {
        window *found = 0;
        for (int i = 0; i < MAX_WINDOWS; ++i) {
            window *o = &g_windows[i];
            if (o->used && o->parent == 0 && o->owner == owner && !o->destroying && (found == 0 || o->z > found->z)) {
                found = o;
            }
        }
        if (found == 0) {
            return;
        }
        if (!DestroyWindow(handle_of(found))) {
            found->owner = 0;
        }
    }
}

BOOL WINAPI DestroyWindow(HWND hWnd) {
    window *w = checked(hWnd);
    if (w == 0) {
        return FALSE;
    }
    if (w->thread != GetCurrentThreadId()) {
        /* Only the thread that made a window destroys it, as on Windows. */
        set_last_error(ERROR_ACCESS_DENIED);
        return FALSE;
    }
    if (w->destroying) {
        return TRUE;
    }
    if (g_hook_count != 0 && cbt_hook(HCBT_DESTROYWND, (WPARAM)hWnd, 0) != 0) {
        return FALSE;
    }
    if (window_of(hWnd) == 0) {
        return TRUE;
    }
    if ((w->style & WS_CHILD) != 0) {
        notify_parents(w, WM_DESTROY, (LPARAM)hWnd);
    }
    /* Hidden, and the focus taken away, before the windows it owns go, as
     * Windows XP orders it (fixtures/controls). */
    if ((w->style & WS_VISIBLE) != 0) {
        set_window_pos(w, NULL, 0, 0, 0, 0,
                       SWP_HIDEWINDOW | SWP_NOACTIVATE | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER);
    }
    if (w->parent == 0) {
        deactivate(w);
    } else if (g_focus != 0 && within(g_focus, w)) {
        SetFocus(NULL);
    }
    if (window_of(hWnd) == 0) {
        return TRUE;
    }
    if (w->parent == 0) {
        destroy_owned(w);
        if (window_of(hWnd) == 0) {
            return TRUE;
        }
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

static void send_icon(window *w);

/* A top-level window's host window: a window of the program's own, or one
 * its owner owns (a dialog, modal or not), with the menu bar above the
 * client area. */
int show_host(window *w) {
    if (w->host != 0) {
        return 1;
    }
    u32 size[2] = {0, 0};
    const u32 owner = w->owner != 0 ? w->owner->host : 0;
    u32 kind = owner == 0 ? SXW_WINDOW_MAIN : w->modeless ? SXW_WINDOW_MODELESS : SXW_WINDOW_OWNED;
    u32 arguments[8] = {(u32)w->text, (u32)client_width(w), (u32)(client_height(w) + w->menu_height), (u32)size,
                        owner, kind, 0, 0};
    if (owner != 0 && own_frame(w)) {
        /* A popup at its place over its owner's surface, its frame its own. */
        window *o = w->owner;
        kind = SXW_WINDOW_POPUP;
        arguments[1] = (u32)(w->rect.right - w->rect.left);
        arguments[2] = (u32)(w->rect.bottom - w->rect.top);
        arguments[5] = kind;
        arguments[6] = (u32)(w->rect.left - o->client.left);
        arguments[7] = (u32)(w->rect.top - o->client.top + o->menu_height);
        w->host = arguments[1] > 0 && arguments[2] > 0 ? SxwService(SXW_SVC_WINDOW_CREATE, arguments) : 0;
        return w->host != 0;
    }
    if (client_width(w) <= 0 || client_height(w) + w->menu_height <= 0) {
        arguments[1] = arguments[2] = 1;
    }
    w->host = SxwService(SXW_SVC_WINDOW_CREATE, arguments);
    if (w->host == 0) {
        return 0;
    }
    const int height = (int)size[1] - w->menu_height;
    if ((int)size[0] != client_width(w) || height != client_height(w)) {
        /* The window manager gave another size. */
        w->rect.right += (int)size[0] - client_width(w);
        w->rect.bottom += height - client_height(w);
        w->client.right = w->client.left + (int)size[0];
        w->client.bottom = w->client.top + height;
        size_surface(w, (int)size[0], (int)size[1]);
    }
    menu_draw_bar(w);
    if (owner == 0) {
        send_icon(w);
    }
    return 1;
}

void drop_host(window *w) {
    if (w->host != 0) {
        u32 arguments[1] = {w->host};
        SxwService(SXW_SVC_WINDOW_DESTROY, arguments);
        w->host = 0;
    }
}

/* SetWindowPos with SWP_HIDEWINDOW: hidden without WM_SHOWWINDOW, and if it
 * was active another window is activated. */
void hide_window(window *w) {
    set_window_pos(w, NULL, 0, 0, 0, 0, SWP_HIDEWINDOW | SWP_NOACTIVATE | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER);
}

/* WM_SHOWWINDOW, then SetWindowPos; a child of a hidden parent only changes
 * its style, as on Windows XP. */
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
    if (window_of(hWnd) == 0) {
        return was_visible;
    }
    if (w->parent != 0 && !visible_chain(w->parent)) {
        if (show) {
            w->style |= WS_VISIBLE;
        } else {
            w->style &= ~(DWORD)WS_VISIBLE;
        }
        return was_visible;
    }
    if (!show) {
        hide_window(w);
        return was_visible;
    }
    UINT flags = SWP_SHOWWINDOW | SWP_NOSIZE | SWP_NOMOVE;
    if (w->parent != 0 || w == g_active || nCmdShow == SW_SHOWNA || nCmdShow == SW_SHOWNOACTIVATE ||
        nCmdShow == SW_SHOWMINNOACTIVE) {
        flags |= SWP_NOACTIVATE | SWP_NOZORDER;
    }
    if (!set_window_pos(w, HWND_TOP, 0, 0, 0, 0, flags)) {
        return FALSE;
    }
    return was_visible;
}

/* SetWindowPos without SWP_NOMOVE and SWP_NOSIZE. A top-level window is
 * moved only, since the window manager owns its size on SavanXP. */
BOOL WINAPI MoveWindow(HWND hWnd, int X, int Y, int nWidth, int nHeight, BOOL bRepaint) {
    window *w = checked(hWnd);
    if (w == 0) {
        return FALSE;
    }
    if (w->parent == 0) {
        nWidth = w->rect.right - w->rect.left;
        nHeight = w->rect.bottom - w->rect.top;
    }
    return set_window_pos(w, NULL, X, Y, nWidth, nHeight,
                          SWP_NOZORDER | SWP_NOACTIVATE | (bRepaint ? 0 : SWP_NOREDRAW));
}

BOOL WINAPI EnableWindow(HWND hWnd, BOOL bEnable) {
    window *w = checked(hWnd);
    if (w == 0) {
        return FALSE;
    }
    const BOOL was_disabled = (w->style & WS_DISABLED) != 0;
    if (bEnable && was_disabled) {
        w->style &= ~(DWORD)WS_DISABLED;
        if (w->parent == 0 && w->menu != NULL) {
            menu_draw_bar(w);
        }
        send(w, WM_ENABLE, TRUE, 0);
    } else if (!bEnable && !was_disabled) {
        send(w, WM_CANCELMODE, 0, 0);
        w->style |= WS_DISABLED;
        if (g_focus != 0 && within(g_focus, w)) {
            SetFocus(NULL);
        }
        if (w->parent == 0 && w->menu != NULL) {
            menu_draw_bar(w);
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
    u32 thread; /* whose messages it is */
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
    slot->thread = w != 0 ? w->thread : GetCurrentThreadId();
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
    const u32 self = GetCurrentThreadId();
    for (int i = 0; i < MAX_TIMERS; ++i) {
        timer *t = &g_timers[i];
        if (t->used && t->thread == self && (filter == NULL || t->hwnd == filter) &&
            (best == 0 || (int)(t->due - best->due) < 0)) {
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

/* A message sent from another thread, waiting for the window's own thread
 * to call the window procedure. It lives on the sender's stack until done. */
typedef struct sent_message {
    struct sent_message *next;
    HWND hwnd;
    UINT message;
    WPARAM wParam;
    LPARAM lParam;
    LRESULT result;
    volatile int done;
    u32 sender;
} sent_message;

/* A thread's message queue: what is posted to it and to its windows, the
 * input for its windows, its quit request, and the messages other threads
 * send it. Made the first time the thread needs one (TEB.Win32ThreadInfo). */
typedef struct thread_queue {
    u32 thread;
    queued posted[MAX_POSTED];
    u32 posted_head;
    u32 posted_count;
    queued input[MAX_INPUT];
    u32 input_head;
    u32 input_count;
    int quit;
    int quit_code;
    sent_message *sent_first;
    sent_message *sent_last;
} thread_queue;

#define MAX_QUEUES 32
static thread_queue *g_queues[MAX_QUEUES];
static int g_in_send; /* inside a message another thread sent */

/* The queue of a thread, made when make is set; 0 for a thread without one. */
static thread_queue *queue_of(u32 thread, int make) {
    for (int i = 0; i < MAX_QUEUES; ++i) {
        if (g_queues[i] != 0 && g_queues[i]->thread == thread) {
            return g_queues[i];
        }
    }
    if (!make) {
        return 0;
    }
    for (int i = 0; i < MAX_QUEUES; ++i) {
        if (g_queues[i] == 0) {
            thread_queue *q = SxwHeapAllocate(sizeof(thread_queue), 1);
            if (q == 0) {
                return 0;
            }
            q->thread = thread;
            g_queues[i] = q;
            return q;
        }
    }
    return 0;
}

/* The calling thread's queue. */
static thread_queue *my_queue(void) {
    thread_queue *q;
    __asm__ volatile("movl %%fs:%c1, %0" : "=r"(q) : "i"(SXW_TEB_WIN32_THREAD));
    if (q == 0) {
        q = queue_of(GetCurrentThreadId(), 1);
        __asm__ volatile("movl %0, %%fs:%c1" : : "r"(q), "i"(SXW_TEB_WIN32_THREAD) : "memory");
    }
    return q;
}

/* Wakes a thread from its wait for messages. */
static void wake_thread(u32 thread) {
    if (thread != GetCurrentThreadId()) {
        u32 arguments[1] = {thread};
        SxwService(SXW_SVC_WAKE_THREAD, arguments);
    }
}
static unsigned char g_key_state[256];  /* 0x80 down, 0x01 toggled */
static unsigned char g_async_down[256]; /* as input arrives */
static WCHAR g_key_char[256];           /* what the last press of each key typed */
POINT g_last_point;

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
    thread_queue *q = queue_of(w->thread, 0);
    if (q == 0) {
        return;
    }
    queued kept;
    for (u32 n = q->posted_count; n > 0; --n) {
        if (take(q->posted, &q->posted_head, &q->posted_count, MAX_POSTED, NULL, 0, 0, 1, &kept) &&
            kept.msg.hwnd != h) {
            push(q->posted, q->posted_head, &q->posted_count, MAX_POSTED, &kept);
        }
    }
    for (u32 n = q->input_count; n > 0; --n) {
        if (take(q->input, &q->input_head, &q->input_count, MAX_INPUT, NULL, 0, 0, 1, &kept) &&
            kept.msg.hwnd != h) {
            push(q->input, q->input_head, &q->input_count, MAX_INPUT, &kept);
        }
    }
}

/* Posts to a thread's queue and wakes the thread. */
static BOOL post_to(u32 thread, HWND hWnd, UINT Msg, WPARAM wParam, LPARAM lParam) {
    thread_queue *q = queue_of(thread, thread == GetCurrentThreadId());
    if (q == 0) {
        set_last_error(1444u); /* ERROR_INVALID_THREAD_ID */
        return FALSE;
    }
    queued m;
    zero(&m, sizeof(m));
    m.msg.hwnd = hWnd;
    m.msg.message = Msg;
    m.msg.wParam = wParam;
    m.msg.lParam = lParam;
    m.msg.time = ticks();
    m.msg.pt = g_last_point;
    if (!push(q->posted, q->posted_head, &q->posted_count, MAX_POSTED, &m)) {
        set_last_error(ERROR_NOT_ENOUGH_MEMORY);
        return FALSE;
    }
    wake_thread(thread);
    return TRUE;
}

BOOL WINAPI PostMessageW(HWND hWnd, UINT Msg, WPARAM wParam, LPARAM lParam) {
    window *w = 0;
    if (hWnd != NULL && (w = checked(hWnd)) == 0) {
        return FALSE;
    }
    return post_to(w != 0 ? w->thread : GetCurrentThreadId(), hWnd, Msg, wParam, lParam);
}

BOOL WINAPI PostThreadMessageW(DWORD idThread, UINT Msg, WPARAM wParam, LPARAM lParam) {
    return post_to(idThread, NULL, Msg, wParam, lParam);
}

void WINAPI PostQuitMessage(int nExitCode) {
    thread_queue *q = my_queue();
    if (q != 0) {
        q->quit = 1;
        q->quit_code = nExitCode;
    }
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

/* The name a registered message or clipboard format was given: its length
 * copied into out, or 0 for a number that is not one. */
int registered_name(UINT id, WCHAR *out, int capacity) {
    if (id < 0xc100u || id - 0xc100u >= g_registered_count) {
        return 0;
    }
    return copy_text(out, g_registered[id - 0xc100u], capacity);
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
            y >= oy + c->rect.top && y < oy + c->rect.bottom &&
            !control_transparent(c)) {
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
    thread_queue *queue = queue_of(target != 0 ? target->thread : GetCurrentThreadId(), 1);
    if (queue != 0) {
        push(queue->input, queue->input_head, &queue->input_count, MAX_INPUT, &q);
        wake_thread(queue->thread);
    }
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
    int x = (int)e->a;
    int y = (int)e->b;
    if (top == 0 && !menu_host_pointer(e->window, &top, &x, &y)) {
        return;
    }
    const int moved = !g_have_pointer || x != g_pointer_x || y != g_pointer_y;
    g_have_pointer = 1;
    g_pointer_x = x;
    g_pointer_y = y;
    {
        /* The screen point: the surface point less where the client area
         * starts on it, from the client area's place. */
        int ox;
        int oy;
        client_origin(top, &ox, &oy);
        g_last_point.x = top->client.left + x - ox;
        g_last_point.y = top->client.top + y - oy;
    }
    if ((e->c & ~g_pointer_buttons) != 0) {
        g_keyboard_last = 0;
    }
    if (g_capture == 0 && top->menu_height > 0 && y >= 0 && y < top->menu_height &&
        (top->style & WS_DISABLED) == 0) {
        /* The menu bar, outside the client area: the non-client messages. */
        const LPARAM screen = MAKELPARAM(g_last_point.x, g_last_point.y);
        if (moved) {
            queue_input(top, WM_NCMOUSEMOVE, HTMENU, screen, e->time, 0, 0);
        }
        static const struct {
            u32 bit;
            int vk;
            UINT down;
        } k_nc[2] = {{SXW_BUTTON_LEFT, VK_LBUTTON, WM_NCLBUTTONDOWN}, {SXW_BUTTON_RIGHT, VK_RBUTTON, 0x00a4}};
        for (int i = 0; i < 2; ++i) {
            const int was = (g_pointer_buttons & k_nc[i].bit) != 0;
            const int is = (e->c & k_nc[i].bit) != 0;
            if (was != is) {
                g_async_down[k_nc[i].vk] = (unsigned char)is;
                queue_input(top, is ? k_nc[i].down : k_nc[i].down + 1u, HTMENU, screen, e->time, k_nc[i].vk, is);
            }
        }
        g_pointer_buttons = e->c;
        return;
    }
    /* The window that has the pointer captured gets it wherever it is, in
     * its own client coordinates; otherwise the window under it, and in
     * its frame or scroll bars as non-client messages. */
    window *target = g_capture != 0 ? g_capture : hit(top, x, y);
    if (target == 0 && g_capture == 0 && own_frame(top)) {
        target = top;
    }
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
    int sx;
    int sy;
    client_screen_origin(target, &sx, &sy);
    const int cx = g_last_point.x - sx;
    const int cy = g_last_point.y - sy;
    const LPARAM where = MAKELPARAM(cx, cy);
    if (g_capture == 0 && (cx < 0 || cy < 0 || cx >= client_width(target) || cy >= client_height(target))) {
        const LPARAM screen = MAKELPARAM(g_last_point.x, g_last_point.y);
        const WPARAM code = (WPARAM)send(target, WM_NCHITTEST, 0, screen);
        if (moved) {
            queue_input(target, WM_NCMOUSEMOVE, code, screen, e->time, 0, 0);
        }
        for (int i = 0; i < 3; ++i) {
            const int was = (g_pointer_buttons & k_buttons[i].bit) != 0;
            const int is = (e->c & k_buttons[i].bit) != 0;
            if (was != is) {
                g_async_down[k_buttons[i].vk] = (unsigned char)is;
                const UINT base = WM_NCLBUTTONDOWN + 3u * (u32)i;
                queue_input(target, is ? base : base + 1u, code, screen, e->time, k_buttons[i].vk, is);
            }
        }
        g_pointer_buttons = e->c;
        return;
    }
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
    if (down) {
        g_keyboard_last = 1;
    }
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
    /* Alt released is WM_SYSKEYUP only when no other key went down or up
     * while it was held. */
    static int alt_alone;
    if (vk == VK_MENU) {
        if (down) {
            alt_alone = !g_async_down[VK_CONTROL];
        } else {
            message = alt_alone ? WM_SYSKEYUP : WM_KEYUP;
            alt_alone = 0;
        }
    } else {
        alt_alone = 0;
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
    if (menu_host_close(e->window)) {
        return;
    }
    window *top = host_window(e->window);
    if (top != 0) {
        queue_input(top, WM_SYSCOMMAND, SC_CLOSE, MAKELPARAM(g_last_point.x, g_last_point.y), e->time, 0, 0);
    }
}

static void resize_event(const sxw_event *e) {
    window *top = host_window(e->window);
    const int height = top != 0 ? (int)e->b - top->menu_height : 0;
    if (top == 0 || ((int)e->a == client_width(top) && height == client_height(top))) {
        return;
    }
    if (!size_surface(top, (int)e->a, (int)e->b)) {
        return;
    }
    top->rect.right += (int)e->a - client_width(top);
    top->rect.bottom += height - client_height(top);
    top->client.right = top->client.left + (int)e->a;
    top->client.bottom = top->client.top + height;
    menu_draw_bar(top);
    WINDOWPOS pos;
    zero(&pos, sizeof(pos));
    pos.hwnd = handle_of(top);
    pos.x = top->rect.left;
    pos.y = top->rect.top;
    pos.cx = top->rect.right - top->rect.left;
    pos.cy = top->rect.bottom - top->rect.top;
    pos.flags = SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOCLIENTMOVE | SWP_NOCLIENTSIZE;
    send(top, WM_WINDOWPOSCHANGING, 0, (LPARAM)&pos);
    send(top, WM_WINDOWPOSCHANGED, 0, (LPARAM)&pos);
    send(top, WM_SIZE, SIZE_RESTORED, MAKELPARAM(client_width(top), client_height(top)));
    invalidate_frame(top);
    invalidate(top, 0, 1, 1);
}

/* Whether the drag's set offers a file list. */
static int drag_has_files(void) {
    for (u32 i = 0; i < SXW_CLIPBOARD_FORMATS; ++i) {
        char name[SXW_CLIPBOARD_NAME];
        u32 length = 0;
        u32 arguments[3] = {i, (u32)name, (u32)&length};
        if (SxwService(SXW_SVC_DRAG_FORMAT, arguments) != 0) {
            return 0;
        }
        const char *want = "text/uri-list";
        u32 k = 0;
        while (want[k] != 0 && name[k] == want[k]) {
            ++k;
        }
        if (want[k] == 0 && name[k] == 0) {
            return 1;
        }
    }
    return 0;
}

/* A drag over a top-level window (docs/desktop/DRAG_AND_DROP.md): the
 * window under the point that takes files, itself or its nearest ancestor
 * with WS_EX_ACCEPTFILES, as Windows looks, answers copy when files are
 * offered; a drop on it is posted as WM_DROPFILES. */
static void drag_event(const sxw_event *e) {
    const u32 phase = e->c & 0xffu;
    const u32 effects = (e->c >> 8) & 0xffu;
    if (phase == SXW_DRAG_LEAVE) {
        return;
    }
    window *top = host_window(e->window);
    window *target = 0;
    int nonclient = 0;
    if (top != 0 && (top->style & WS_DISABLED) == 0) {
        target = hit(top, (int)e->a, (int)e->b);
        if (target == 0) {
            target = top;
            nonclient = 1;
        }
        while (target != 0 && (target->ex_style & WS_EX_ACCEPTFILES) == 0) {
            target = target->parent;
        }
    }
    const int files = target != 0 && (effects & SXW_DRAG_EFFECT_COPY) != 0 && drag_has_files();
    if (phase == SXW_DRAG_ENTER || phase == SXW_DRAG_OVER) {
        u32 arguments[2] = {e->window, files ? SXW_DRAG_EFFECT_COPY : 0u};
        SxwService(SXW_SVC_DRAG_REPLY, arguments);
        return;
    }
    if (phase != SXW_DRAG_DROP || !files) {
        return;
    }
    u32 length = 0;
    u32 probe[4] = {(u32)"text/uri-list", 0, 0, (u32)&length};
    if (SxwService(SXW_SVC_DRAG_GET, probe) == SXW_FAILED || length == 0) {
        return;
    }
    char *bytes = SxwHeapAllocate(length + 1u, 0);
    if (bytes == 0) {
        return;
    }
    u32 got = length;
    u32 arguments[4] = {(u32)"text/uri-list", (u32)bytes, length, (u32)&got};
    if (SxwService(SXW_SVC_DRAG_GET, arguments) != SXW_FAILED) {
        /* The point in the target's client coordinates, through the screen. */
        int ox;
        int oy;
        client_origin(top, &ox, &oy);
        POINT point = {top->client.left + (int)e->a - ox, top->client.top + (int)e->b - oy};
        ScreenToClient(handle_of(target), &point);
        HGLOBAL drop = make_drop(bytes, length, point, nonclient);
        if (drop != NULL) {
            PostMessageW(handle_of(target), WM_DROPFILES, (WPARAM)drop, 0);
        }
    }
    SxwHeapFree(bytes);
}

static void handle_event(const sxw_event *e) {
    switch (e->kind) {
    case SXW_EVENT_DRAG:
        drag_event(e);
        break;
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
    const u32 got = SxwService(SXW_SVC_WAIT_EVENT, arguments);
    if (got == 1u) {
        handle_event(&e);
    }
    /* 2: another thread posted or sent this one a message. */
    return got != 0u;
}

/* Calls the window procedures other threads sent messages to, on this
 * thread, as Windows does whenever the thread looks at its queue. */
static int receive_sent(void) {
    thread_queue *q = my_queue();
    int any = 0;
    while (q != 0 && q->sent_first != 0) {
        sent_message *m = q->sent_first;
        q->sent_first = m->next;
        if (q->sent_first == 0) {
            q->sent_last = 0;
        }
        window *w = window_of(m->hwnd);
        ++g_in_send;
        m->result = w != 0 ? send(w, m->message, m->wParam, m->lParam) : 0;
        --g_in_send;
        m->done = 1;
        wake_thread(m->sender);
        any = 1;
    }
    return any;
}

/* SendMessage to a window of another thread: queued there, and this thread
 * waits for the answer, taking the messages sent to it meanwhile. */
static LRESULT send_across(window *w, UINT message, WPARAM wParam, LPARAM lParam) {
    thread_queue *q = queue_of(w->thread, 1);
    if (q == 0) {
        return 0;
    }
    sent_message m;
    zero(&m, sizeof(m));
    m.hwnd = handle_of(w);
    m.message = message;
    m.wParam = wParam;
    m.lParam = lParam;
    m.sender = GetCurrentThreadId();
    if (q->sent_last != 0) {
        q->sent_last->next = &m;
    } else {
        q->sent_first = &m;
    }
    q->sent_last = &m;
    wake_thread(w->thread);
    while (!m.done) {
        if (!receive_sent()) {
            wait_event(SXW_INFINITE);
        }
    }
    return m.result;
}

LRESULT WINAPI SendMessageW(HWND hWnd, UINT Msg, WPARAM wParam, LPARAM lParam) {
    window *w = checked(hWnd);
    if (w == 0) {
        return 0;
    }
    if (w->thread != GetCurrentThreadId()) {
        return send_across(w, Msg, wParam, lParam);
    }
    return send(w, Msg, wParam, lParam);
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
    if (m->message >= WM_NCMOUSEMOVE && m->message <= 0x00a9) {
        const LRESULT where = send(w, WM_NCHITTEST, 0, m->lParam);
        /* A press on a child's frame activates its top-level window, as one
         * in its client area does, and leaves the focus where it is. */
        window *top = top_of(w);
        if (m->message == WM_NCLBUTTONDOWN && top != g_active) {
            const LRESULT answer = send(w, WM_MOUSEACTIVATE, (WPARAM)handle_of(top), MAKELPARAM(where, m->message));
            if (answer == MA_ACTIVATE || answer == 2 /* MA_ACTIVATEANDEAT */) {
                activate(top);
            }
        }
        send(w, WM_SETCURSOR, (WPARAM)handle_of(w), MAKELPARAM(where, m->message == WM_NCMOUSEMOVE
                                                                           ? WM_MOUSEMOVE
                                                                           : m->message - WM_NCMOUSEMOVE +
                                                                                 WM_MOUSEMOVE));
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
        window *top = top_of(w);
        if (top != g_active) {
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
static MSG g_last_message; /* GetMessageTime and GetMessagePos */

static int next_message_raw(MSG *msg, HWND filter, UINT first, UINT last, int remove, int block) {
    thread_queue *queue = my_queue();
    for (;;) {
        poll_events();
        receive_sent();
        queued q;
        if (take(queue->posted, &queue->posted_head, &queue->posted_count, MAX_POSTED, filter, first, last, remove,
                 &q)) {
            *msg = q.msg;
            return 1;
        }
        if (queue->quit && filter == NULL) {
            zero(msg, sizeof(*msg));
            msg->message = WM_QUIT;
            msg->wParam = (WPARAM)queue->quit_code;
            msg->time = ticks();
            if (remove) {
                queue->quit = 0;
            }
            return 1;
        }
        if (take(queue->input, &queue->input_head, &queue->input_count, MAX_INPUT, filter, first, last, remove, &q)) {
            *msg = q.msg;
            if (remove && q.vk != 0) {
                g_key_state[q.vk] = (unsigned char)((q.down ? 0x80 : 0) |
                                                    ((g_key_state[q.vk] ^ (q.down ? 1 : 0)) & 1));
            }
            window *w = window_of(q.msg.hwnd);
            if (w != 0 && remove &&
                ((q.msg.message >= WM_MOUSEMOVE && q.msg.message <= WM_MBUTTONDBLCLK) ||
                 (q.msg.message >= WM_NCMOUSEMOVE && q.msg.message <= 0x00a9))) {
                before_mouse(w, &q.msg);
            }
            if (w == 0 && q.msg.hwnd != NULL) {
                /* Its window is gone: drop it, even when only peeking. */
                if (!remove) {
                    take(queue->input, &queue->input_head, &queue->input_count, MAX_INPUT, filter, first, last, 1,
                         &q);
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
        caret_blink(now);
        if (!block) {
            present_all();
            return 0;
        }
        present_all();
        u32 timeout = t != 0 ? (expired(t->due, now) ? 0u : t->due - now) : SXW_INFINITE;
        const u32 blink = caret_next_blink(now);
        if (blink != 0) {
            const u32 until = expired(blink, now) ? 0u : blink - now;
            timeout = until < timeout ? until : timeout;
        }
        wait_event(timeout);
    }
}

/* GetMessage and PeekMessage, then the WH_GETMESSAGE hook, which sees what
 * the caller will get and may change it. */
int next_message(MSG *msg, HWND filter, UINT first, UINT last, int remove, int block) {
    if (!next_message_raw(msg, filter, first, last, remove, block)) {
        return 0;
    }
    g_last_message = *msg;
    if (g_hook_count != 0 && hook_installed(WH_GETMESSAGE)) {
        call_hook(WH_GETMESSAGE, HC_ACTION, remove ? PM_REMOVE : PM_NOREMOVE, (LPARAM)msg);
    }
    return 1;
}

LONG WINAPI GetMessageTime(void) {
    return (LONG)g_last_message.time;
}

DWORD WINAPI GetMessagePos(void) {
    return (DWORD)MAKELPARAM(g_last_message.pt.x, g_last_message.pt.y);
}

BOOL WINAPI WaitMessage(void) {
    MSG m;
    next_message_raw(&m, NULL, 0, 0, 0, 1);
    return TRUE;
}

/* Whether a key is down now, as input arrives, and whether it went down
 * since the last time it was asked (bit 0). */
short WINAPI GetAsyncKeyState(int vKey) {
    poll_events();
    return (short)(g_async_down[vKey & 0xff] ? 0x8000 : 0);
}

BOOL WINAPI InSendMessage(void) {
    return g_in_send != 0;
}

/* A sent message with a time-out: on one thread it is a plain send. */
LRESULT WINAPI SendMessageTimeoutW(HWND hWnd, UINT Msg, WPARAM wParam, LPARAM lParam, UINT fuFlags, UINT uTimeout,
                                   ULONG_PTR *lpdwResult) {
    (void)fuFlags;
    (void)uTimeout;
    window *w = checked(hWnd);
    if (w == 0) {
        return 0;
    }
    const LRESULT result = SendMessageW(hWnd, Msg, wParam, lParam);
    if (lpdwResult != NULL) {
        *lpdwResult = (ULONG_PTR)result;
    }
    return 1;
}

/* There is no sound device for the subsystem to beep on. */
BOOL WINAPI MessageBeep(UINT uType) {
    (void)uType;
    return TRUE;
}

int input_waiting(void) {
    poll_events();
    receive_sent();
    const thread_queue *q = my_queue();
    return q->posted_count != 0 || q->input_count != 0 || q->quit || any_paint(NULL) != 0;
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
        receive_sent();
        const thread_queue *q = my_queue();
        if (q->posted_count != 0 || q->input_count != 0 || q->quit || any_paint(NULL) != 0 ||
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

/* Posted by TranslateMessage for F1, turned into WM_HELP by DefWindowProc. */
#define WM_KEYF1 0x004d

BOOL WINAPI TranslateMessage(const MSG *lpMsg) {
    if (lpMsg == NULL || (lpMsg->message != WM_KEYDOWN && lpMsg->message != WM_SYSKEYDOWN)) {
        return FALSE;
    }
    if (lpMsg->message == WM_KEYDOWN && lpMsg->wParam == VK_F1) {
        PostMessageW(lpMsg->hwnd, WM_KEYF1, 0, 0);
        return TRUE;
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
    return call_proc(w, lpMsg->message, lpMsg->wParam, lpMsg->lParam);
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
#define MAX_ICONS 64
#define MAX_ACCELERATORS 16

__declspec(dllimport) HBITMAP WINAPI SxwCreateBitmapFromDIB(const void *data, u32 size);

u32 get16(const unsigned char *p) {
    return (u32)p[0] | ((u32)p[1] << 8);
}

u32 get32(const unsigned char *p) {
    return get16(p) | (get16(p + 2) << 16);
}

static u32 main_module(void) {
    u32 peb;
    __asm__ volatile("movl %%fs:%c1, %0" : "=r"(peb) : "i"(SXW_TEB_PEB));
    return *(u32 *)(peb + SXW_PEB_IMAGE_BASE);
}

/* A resource's data and size in the module at base, or 0. */
const unsigned char *find_resource(u32 base, u32 type, LPCWSTR name, u32 *size) {
    const unsigned char *entry = sxw_resource_entry(base, (LPCWSTR)(ULONG_PTR)type, name, SXW_ANY_LANGUAGE);
    if (entry == 0) {
        return 0;
    }
    *size = get32(entry + 4);
    return (const unsigned char *)base + get32(entry);
}

u32 module_of(HINSTANCE instance) {
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

/* An icon's image as 16x16 pixels 0xAARRGGBB, top row first, for the host
 * (SXW_SVC_WINDOW_ICON): a 1, 4, 8, 24 or 32-bit DIB with its AND mask,
 * sampled by nearest pixel when it is another size. A 32-bit image with any
 * alpha uses it; otherwise the mask makes pixels transparent. 0 when the
 * handle is not an image of the program's. */
static int icon_pixels(HICON handle, u32 *out) {
    const u32 h = (u32)handle;
    const int slot = (int)((h & 0xffffu) / 4u) - 1;
    if ((h & 0xffff0000u) != 0x00020000u || slot < 0 || slot >= MAX_ICONS || !g_icons[slot].used ||
        g_icons[slot].cursor) {
        return 0;
    }
    const icon *c = &g_icons[slot];
    const unsigned char *dib = c->data;
    const u32 header = get32(dib);
    const int bits = (int)get16(dib + 14);
    const int width = c->width;
    const int height = c->height;
    if (header < 40u || width <= 0 || height <= 0 ||
        (bits != 1 && bits != 4 && bits != 8 && bits != 24 && bits != 32)) {
        return 0;
    }
    u32 colors = bits <= 8 ? get32(dib + 32) : 0;
    if (bits <= 8 && colors == 0) {
        colors = 1u << bits;
    }
    const unsigned char *palette = dib + header;
    const unsigned char *xor_bits = palette + 4u * colors;
    const u32 xor_stride = (((u32)width * (u32)bits + 31u) / 32u) * 4u;
    const unsigned char *and_bits = xor_bits + xor_stride * (u32)height;
    const u32 and_stride = (((u32)width + 31u) / 32u) * 4u;
    if (and_bits + and_stride * (u32)height > c->data + c->size) {
        return 0;
    }
    int alpha = 0;
    for (int y = 0; bits == 32 && y < height && !alpha; ++y) {
        for (int x = 0; x < width && !alpha; ++x) {
            alpha = xor_bits[(u32)y * xor_stride + 4u * (u32)x + 3u] != 0;
        }
    }
    for (int oy = 0; oy < (int)SXW_ICON_EXTENT; ++oy) {
        for (int ox = 0; ox < (int)SXW_ICON_EXTENT; ++ox) {
            const int x = ox * width / (int)SXW_ICON_EXTENT;
            const u32 row = (u32)(height - 1 - oy * height / (int)SXW_ICON_EXTENT); /* bottom-up */
            const unsigned char *line = xor_bits + row * xor_stride;
            u32 pixel;
            if (bits == 32) {
                pixel = get32(line + 4u * (u32)x);
            } else if (bits == 24) {
                pixel = (u32)line[3 * x] | (u32)line[3 * x + 1] << 8 | (u32)line[3 * x + 2] << 16;
            } else {
                const u32 bit = (u32)x * (u32)bits;
                const u32 index = (line[bit / 8u] >> (8u - (u32)bits - bit % 8u)) & ((1u << bits) - 1u);
                pixel = index < colors ? get32(palette + 4u * index) & 0xffffffu : 0;
            }
            if (!alpha) {
                const int masked = (and_bits[row * and_stride + (u32)x / 8u] >> (7u - (u32)x % 8u)) & 1;
                pixel = masked ? 0 : (pixel & 0xffffffu) | 0xff000000u;
            }
            out[oy * (int)SXW_ICON_EXTENT + ox] = pixel;
        }
    }
    return 1;
}

/* The class's small icon, or its icon, to the host of a program's window. */
static void send_icon(window *w) {
    static u32 pixels[SXW_ICON_EXTENT * SXW_ICON_EXTENT];
    if (w->cls == 0 || !(icon_pixels(w->cls->small_icon, pixels) || icon_pixels(w->cls->icon, pixels))) {
        return;
    }
    u32 arguments[2] = {w->host, (u32)pixels};
    SxwService(SXW_SVC_WINDOW_ICON, arguments);
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
        /* A command the menu has: WM_INITMENU and WM_INITMENUPOPUP first,
         * nothing when the item is grayed, and WM_UNINITMENUPOPUP after, as
         * Windows XP sends them. */
        window *top = top_of(w);
        HMENU popup = NULL;
        int index = 0;
        UINT state = 0;
        const int in_menu = top->menu != NULL && (top->style & WS_DISABLED) == 0 && g_capture == 0 &&
                            menu_command_popup(top->menu, command, &popup, &index, &state);
        if (in_menu) {
            send(top, WM_INITMENU, (WPARAM)top->menu, 0);
            send(top, WM_INITMENUPOPUP, (WPARAM)popup, MAKELPARAM(index, 0));
            menu_command_popup(top->menu, command, &popup, &index, &state);
        }
        if (!in_menu || (state & (MF_GRAYED | MF_DISABLED)) == 0) {
            send(w, WM_COMMAND, MAKELPARAM(command, 1), 0);
        }
        if (in_menu && window_of(handle_of(top)) != 0) {
            send(top, WM_UNINITMENUPOPUP, (WPARAM)popup, 0);
        }
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

static int g_menu_key;

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
        /* lParam is a RECT, or with wParam an NCCALCSIZE_PARAMS whose first
         * rectangle is the window's new one: either way it becomes the
         * client area. */
        if (lParam != 0) {
            RECT *r = (RECT *)lParam;
            int f[4];
            if (w->parent == 0 && !w->message_only && !own_frame(w)) {
                frame_of(w->style, w->menu != NULL, f);
            } else {
                frame_of_child(w, r->right - r->left, r->bottom - r->top, f);
            }
            r->left += f[0];
            r->top += f[1];
            r->right -= f[2];
            r->bottom -= f[3];
            if (r->right < r->left) {
                r->right = r->left;
            }
            if (r->bottom < r->top) {
                r->bottom = r->top;
            }
        }
        return 0;
    case WM_NCHITTEST:
        if (w->parent == 0 && w->menu_height > 0 && GET_Y_LPARAM(lParam) < w->client.top &&
            GET_Y_LPARAM(lParam) >= w->client.top - w->menu_height) {
            return HTMENU;
        }
        if (w->parent != 0 || own_frame(w)) {
            int sx;
            int sy;
            client_screen_origin(w, &sx, &sy);
            const int px = GET_X_LPARAM(lParam) - sx;
            const int py = GET_Y_LPARAM(lParam) - sy;
            if (px >= 0 && py >= 0 && px < client_width(w) && py < client_height(w)) {
                return HTCLIENT;
            }
            const LRESULT bar = scroll_nc_hit(w, GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
            return bar != HTNOWHERE ? bar : HTBORDER;
        }
        return HTCLIENT;
    case WM_NCLBUTTONDOWN:
    case WM_NCLBUTTONDBLCLK:
        if (wParam == HTMENU && Msg == WM_NCLBUTTONDOWN) {
            send(top_of(w), WM_SYSCOMMAND, SC_MOUSEMENU + HTMENU, lParam);
        } else if (wParam == HTVSCROLL || wParam == HTHSCROLL) {
            RECT r;
            GetWindowRect(hWnd, &r);
            scroll_track(w, wParam == HTVSCROLL ? SB_VERT : SB_HORZ, GET_X_LPARAM(lParam) - r.left,
                         GET_Y_LPARAM(lParam) - r.top);
        }
        return 0;
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
    case WM_NCPAINT:
        nc_paint(w);
        return 0;
    case WM_KEYF1: {
        /* Help about the window, where the pointer is. */
        HELPINFO help;
        zero(&help, sizeof(help));
        help.cbSize = sizeof(help);
        help.iContextType = HELPINFO_WINDOW;
        help.iCtrlId = w->parent != 0 ? (int)w->id : 0;
        help.hItemHandle = hWnd;
        help.MousePos = g_last_point;
        send(w, WM_HELP, 0, (LPARAM)&help);
        return 0;
    }
    case WM_HELP:
        /* A child's goes to its parent; a top-level window's stops. */
        return w->parent != 0 && (w->style & WS_CHILD) != 0 ? send(w->parent, WM_HELP, wParam, lParam) : 0;
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
        /* WM_MOVE, then WM_SIZE, for what the client area did; an
         * overlapped window shown for the first time hears both. */
        const WINDOWPOS *pos = (const WINDOWPOS *)lParam;
        if (w->need_size && (w->style & WS_VISIBLE) != 0) {
            w->need_size = 0;
            send_size_move(w);
        } else if (pos != NULL) {
            if ((pos->flags & SWP_NOCLIENTMOVE) == 0) {
                send(w, WM_MOVE, 0, MAKELPARAM(w->client.left, w->client.top));
            }
            if ((pos->flags & SWP_NOCLIENTSIZE) == 0 && window_of(hWnd) != 0) {
                const DWORD kind = (w->style & WS_MINIMIZE) != 0   ? SIZE_MINIMIZED
                                   : (w->style & WS_MAXIMIZE) != 0 ? SIZE_MAXIMIZED
                                                                    : SIZE_RESTORED;
                send(w, WM_SIZE, kind, MAKELPARAM(client_width(w), client_height(w)));
            }
        }
        return 0;
    }
    case WM_CLOSE:
        DestroyWindow(hWnd);
        return 0;
    case WM_SYSCOMMAND:
        if (g_hook_count != 0 && cbt_hook(HCBT_SYSCOMMAND, wParam, lParam) != 0) {
            return 0;
        }
        switch (wParam & 0xfff0u) {
        case SC_CLOSE:
            send(w, WM_CLOSE, 0, 0);
            break;
        case SC_MOUSEMENU:
            menu_track(top_of(w), 1, 0);
            break;
        case SC_KEYMENU:
            menu_track(top_of(w), 0, (WCHAR)lParam);
            break;
        default:
            break;
        }
        return 0;
    case WM_SYSKEYDOWN:
    case WM_KEYDOWN:
        /* Alt or F10 pressed and released alone opens the menu bar. */
        if ((lParam & (1L << 30)) == 0) {
            g_menu_key = (wParam == VK_MENU || wParam == VK_F10) ? (int)wParam : 0;
        }
        if (Msg == WM_SYSKEYDOWN && wParam == VK_F4 && (lParam & (1L << 29)) != 0) {
            send(top_of(w), WM_SYSCOMMAND, SC_CLOSE, 0);
        }
        return 0;
    case WM_SYSKEYUP:
    case WM_KEYUP:
        if ((wParam == VK_MENU || wParam == VK_F10) && g_menu_key == (int)wParam) {
            g_menu_key = 0;
            send(top_of(w), WM_SYSCOMMAND, SC_KEYMENU, 0);
        }
        g_menu_key = 0;
        return 0;
    case WM_SYSCHAR:
        if (wParam != '\r' && wParam != '\t' && wParam != 0x1b && wParam != ' ') {
            send(top_of(w), WM_SYSCOMMAND, SC_KEYMENU, (LPARAM)wParam);
        }
        return 0;
    case WM_CTLCOLORMSGBOX:
    case WM_CTLCOLORBTN:
    case WM_CTLCOLORDLG:
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX:
    case WM_CTLCOLORSCROLLBAR:
        return default_control_color(Msg, (HDC)wParam);
    case WM_QUERYUISTATE:
        return (LRESULT)w->ui_state;
    case WM_CHANGEUISTATE: {
        WORD action = LOWORD(wParam);
        WORD flags = HIWORD(wParam);
        if (w->parent != 0) {
            send(w->parent, Msg, wParam, lParam);
            return 0;
        }
        if (action == UIS_INITIALIZE) {
            action = g_keyboard_last ? UIS_CLEAR : UIS_SET;
            flags = UISF_HIDEFOCUS | UISF_HIDEACCEL;
        }
        const u32 changed = action == UIS_SET ? flags & ~w->ui_state : flags & w->ui_state;
        if (changed != 0) {
            send(w, WM_UPDATEUISTATE, MAKELPARAM(action, flags), 0);
        }
        return 0;
    }
    case WM_UPDATEUISTATE:
        if (LOWORD(wParam) == UIS_SET) {
            w->ui_state |= HIWORD(wParam);
        } else if (LOWORD(wParam) == UIS_CLEAR) {
            w->ui_state &= ~(u32)HIWORD(wParam);
        }
        for (window *c = w->child; c != 0; c = c->next) {
            send(c, WM_UPDATEUISTATE, wParam, lParam);
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

/* The brushes of the system colours, made once and never deleted. */
HBRUSH system_brush(int index) {
    static HBRUSH brushes[31];
    if (index < 0 || index >= 31) {
        return NULL;
    }
    if (brushes[index] == NULL) {
        brushes[index] = CreateSolidBrush(GetSysColor(index));
    }
    return brushes[index];
}

void fill_brush(HDC dc, const RECT *r, HBRUSH brush) {
    if (r->right <= r->left || r->bottom <= r->top || brush == NULL) {
        return;
    }
    HGDIOBJ old = SelectObject(dc, brush);
    PatBlt(dc, r->left, r->top, r->right - r->left, r->bottom - r->top, PATCOPY);
    SelectObject(dc, old);
}

void fill_rect(HDC dc, const RECT *r, COLORREF color) {
    HBRUSH brush = CreateSolidBrush(color);
    fill_brush(dc, r, brush);
    DeleteObject(brush);
}

/* DefWindowProc's WM_CTLCOLOR*: text in the window text colour on the face
 * colour, or on the window colour for edit and list boxes. */
LRESULT default_control_color(UINT message, HDC dc) {
    const int window_colors = message == WM_CTLCOLOREDIT || message == WM_CTLCOLORLISTBOX;
    SetTextColor(dc, GetSysColor(COLOR_WINDOWTEXT));
    SetBkColor(dc, GetSysColor(window_colors ? COLOR_WINDOW : COLOR_BTNFACE));
    return (LRESULT)system_brush(window_colors ? COLOR_WINDOW : COLOR_BTNFACE);
}

/* WM_CHANGEUISTATE to a dialog, which DefWindowProc turns into
 * WM_UPDATEUISTATE for the window and its children when it changes. */
void change_ui_state(window *w, WORD action, WORD flags) {
    w = top_of(w);
    /* Sent when anything would change, with every flag asked for. */
    const u32 changed = action == UIS_SET ? flags & ~w->ui_state : flags & w->ui_state;
    if (changed != 0) {
        send(w, WM_CHANGEUISTATE, MAKELPARAM(action, flags), 0);
    }
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
        if (underline >= 0 && (format & 0x00100000u /* DT_HIDEPREFIX */) == 0) {
            SIZE before;
            SIZE glyph;
            GetTextExtentPoint32W(dc, shown, underline, &before);
            GetTextExtentPoint32W(dc, shown + underline, 1, &glyph);
            TEXTMETRICW tm;
            GetTextMetricsW(dc, &tm);
            const COLORREF color = SetTextColor(dc, 0);
            SetTextColor(dc, color);
            RECT bar = {x + before.cx, y + tm.tmAscent + 1, x + before.cx + glyph.cx, y + tm.tmAscent + 2};
            fill_rect(dc, &bar, color);
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

BOOL WINAPI IsWindow(HWND hWnd) {
    return window_of(hWnd) != 0;
}

DWORD WINAPI GetWindowThreadProcessId(HWND hWnd, LPDWORD lpdwProcessId) {
    window *w = checked(hWnd);
    if (w == 0) {
        return 0;
    }
    if (lpdwProcessId != NULL) {
        *lpdwProcessId = GetCurrentProcessId();
    }
    return w->thread;
}
