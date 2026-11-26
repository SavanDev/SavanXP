/*
 * winpos.c -- What MFC does to every window (docs/userland/WIN32.md, "Hooks
 * and subclassing", "Positions"): SetWindowPos and deferred positions,
 * SetParent, the z-order of a top-level window and the windows it owns,
 * window longs and subclassing, window properties, the window tree, class
 * queries, window text, and where a window is on the screen.
 *
 * The messages SetWindowPos sends, and the flags it adds to what it was
 * given, are the ones Windows XP showed fixtures/hooks: WM_WINDOWPOSCHANGING
 * always; when the size or the frame changes, WM_NCCALCSIZE; then, when
 * anything changed at all, WM_WINDOWPOSCHANGED with SWP_NOMOVE, SWP_NOSIZE
 * and SWP_NOZORDER for what did not change, SWP_NOCLIENTSIZE and
 * SWP_NOCLIENTMOVE for what the client area did not do, and SWP_NOREDRAW
 * for a window whose parent is hidden. DefWindowProc turns that into
 * WM_MOVE and WM_SIZE.
 */
#include "user32_private.h"

#define DESKTOP_HANDLE ((HWND)0x00010010)
#define ERROR_NO_SCROLLBARS 1447u

static u32 g_z;

u32 next_z(void) {
    return ++g_z;
}

/* ---- Screen coordinates ------------------------------------------------------------------- */

/* Where a window's client area is on the screen, from the top-level
 * window's own idea of its position: on SavanXP the window manager places
 * top-level windows and does not say where. */
void client_screen_origin(const window *w, int *x, int *y) {
    *x = 0;
    *y = 0;
    for (; w != 0; w = w->parent) {
        *x += w->client.left;
        *y += w->client.top;
    }
}

static void window_screen_rect(const window *w, RECT *r) {
    int x = 0;
    int y = 0;
    if (w->parent != 0) {
        client_screen_origin(w->parent, &x, &y);
    }
    r->left = w->rect.left + x;
    r->top = w->rect.top + y;
    r->right = w->rect.right + x;
    r->bottom = w->rect.bottom + y;
}

BOOL WINAPI GetWindowRect(HWND hWnd, LPRECT lpRect) {
    if (hWnd == DESKTOP_HANDLE) {
        lpRect->left = lpRect->top = 0;
        lpRect->right = GetSystemMetrics(SM_CXSCREEN);
        lpRect->bottom = GetSystemMetrics(SM_CYSCREEN);
        return TRUE;
    }
    window *w = checked(hWnd);
    if (w == 0 || lpRect == NULL) {
        return FALSE;
    }
    window_screen_rect(w, lpRect);
    return TRUE;
}

BOOL WINAPI ScreenToClient(HWND hWnd, LPPOINT lpPoint) {
    window *w = checked(hWnd);
    if (w == 0 || lpPoint == NULL) {
        return FALSE;
    }
    int x;
    int y;
    client_screen_origin(w, &x, &y);
    lpPoint->x -= x;
    lpPoint->y -= y;
    return TRUE;
}

/* The offset from one window's client area to another's in the low and
 * high words, as Windows answers it. */
int WINAPI MapWindowPoints(HWND hWndFrom, HWND hWndTo, LPPOINT lpPoints, UINT cPoints) {
    int fx = 0;
    int fy = 0;
    int tx = 0;
    int ty = 0;
    if (hWndFrom != NULL && hWndFrom != DESKTOP_HANDLE) {
        window *from = checked(hWndFrom);
        if (from == 0) {
            return 0;
        }
        client_screen_origin(from, &fx, &fy);
    }
    if (hWndTo != NULL && hWndTo != DESKTOP_HANDLE) {
        window *to = checked(hWndTo);
        if (to == 0) {
            return 0;
        }
        client_screen_origin(to, &tx, &ty);
    }
    const int dx = fx - tx;
    const int dy = fy - ty;
    for (UINT i = 0; i < cPoints && lpPoints != NULL; ++i) {
        lpPoints[i].x += dx;
        lpPoints[i].y += dy;
    }
    set_last_error(0);
    return (int)MAKELPARAM(dx, dy);
}

/* The deepest visible window at a screen point among the program's. */
static window *window_at(window *w, int x, int y) {
    RECT r;
    window_screen_rect(w, &r);
    if ((w->style & WS_VISIBLE) == 0 || x < r.left || y < r.top || x >= r.right || y >= r.bottom) {
        return 0;
    }
    for (window *c = w->child; c != 0; c = c->next) {
        window *found = window_at(c, x, y);
        if (found != 0) {
            return found;
        }
    }
    return w;
}

HWND WINAPI WindowFromPoint(POINT Point) {
    window *best = 0;
    for (int i = 0; i < MAX_WINDOWS; ++i) {
        window *w = &g_windows[i];
        if (w->used && w->parent == 0 && !w->message_only && (best == 0 || w->z > best->z)) {
            window *found = window_at(w, Point.x, Point.y);
            if (found != 0) {
                best = w;
            }
        }
    }
    return best != 0 ? handle_of(window_at(best, Point.x, Point.y)) : NULL;
}

/* The child of a window at a point of its client area, hidden ones
 * included, or the window itself. */
HWND WINAPI ChildWindowFromPoint(HWND hWndParent, POINT Point) {
    window *w = checked(hWndParent);
    if (w == 0) {
        return NULL;
    }
    if (Point.x < 0 || Point.y < 0 || Point.x >= client_width(w) || Point.y >= client_height(w)) {
        return NULL;
    }
    for (window *c = w->child; c != 0; c = c->next) {
        if (Point.x >= c->rect.left && Point.x < c->rect.right && Point.y >= c->rect.top &&
            Point.y < c->rect.bottom) {
            return handle_of(c);
        }
    }
    return hWndParent;
}

/* ---- The z-order of top-level windows ------------------------------------------------------ */

static window *root_of(window *w) {
    while (w->owner != 0) {
        w = w->owner;
    }
    return w;
}

static int owned_by(const window *w, const window *root) {
    for (const window *o = w->owner; o != 0; o = o->owner) {
        if (o == root) {
            return 1;
        }
    }
    return 0;
}

/* Brings a top-level window to the top with the windows of its owner's
 * group, hidden ones too: each hears WM_WINDOWPOSCHANGING in the order they
 * end up, top first, w with its own flags and the others with
 * SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE. This is what Windows XP showed
 * fixtures/hooks when it showed or activated a window with an owned one. */
static void z_group(window *w, WINDOWPOS *own) {
    window *root = root_of(w);
    window *group[MAX_WINDOWS];
    int n = 0;
    if (w != root) {
        group[n++] = w;
    }
    for (;;) {
        window *next = 0;
        for (int i = 0; i < MAX_WINDOWS; ++i) {
            window *o = &g_windows[i];
            if (!o->used || o->parent != 0 || o == w || o == root || o->destroying || !owned_by(o, root)) {
                continue;
            }
            int listed = 0;
            for (int k = 0; k < n; ++k) {
                listed |= group[k] == o;
            }
            if (!listed && (next == 0 || o->z > next->z)) {
                next = o;
            }
        }
        if (next == 0) {
            break;
        }
        group[n++] = next;
    }
    group[n++] = root;
    for (int k = 0; k < n; ++k) {
        window *g = group[k];
        if (g == w) {
            send(g, WM_WINDOWPOSCHANGING, 0, (LPARAM)own);
        } else {
            WINDOWPOS p;
            zero(&p, sizeof(p));
            p.hwnd = handle_of(g);
            p.flags = SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE;
            send(g, WM_WINDOWPOSCHANGING, 0, (LPARAM)&p);
        }
    }
    for (int k = n - 1; k >= 0; --k) {
        if (group[k]->used) {
            group[k]->z = next_z();
        }
    }
}

/* The activation of a top-level window: the CBT hook may refuse it; the
 * window that was active hears it lost, the group comes to the top, and w
 * hears it won. */
void activate(window *w) {
    if (g_active == w) {
        return;
    }
    window *old = g_active;
    if (g_hook_count != 0) {
        CBTACTIVATESTRUCT a = {FALSE, handle_of(old)};
        if (cbt_hook(HCBT_ACTIVATE, (WPARAM)handle_of(w), (LPARAM)&a) != 0) {
            return;
        }
    }
    if (old != 0) {
        send(old, WM_NCACTIVATE, FALSE, 0);
        send(old, WM_ACTIVATE, WA_INACTIVE, (LPARAM)handle_of(w));
    }
    if (!w->used) {
        return;
    }
    WINDOWPOS own;
    zero(&own, sizeof(own));
    own.hwnd = handle_of(w);
    own.flags = SWP_NOMOVE | SWP_NOSIZE;
    z_group(w, &own);
    g_active = w;
    if (old == 0) {
        send(w, WM_ACTIVATEAPP, TRUE, 0);
    }
    send(w, WM_NCACTIVATE, TRUE, 0);
    send(w, WM_ACTIVATE, WA_ACTIVE, (LPARAM)handle_of(old));
}

/* ---- SetWindowPos ------------------------------------------------------------------------- */

typedef struct pos_job {
    window *w;
    HWND hwnd;
    WINDOWPOS pos;
    int changed;    /* anything changed: WM_WINDOWPOSCHANGED follows */
    int deactivate; /* a top-level window hidden while active: after it */
} pos_job;

static int sibling_index(const window *w) {
    int i = 0;
    for (const window *c = w->parent->child; c != 0; c = c->next, ++i) {
        if (c == w) {
            return i;
        }
    }
    return -1;
}

/* The first half of SetWindowPos: WM_WINDOWPOSCHANGING, the new place and
 * size with WM_NCCALCSIZE, the z-order, and showing or hiding. moved
 * forces a move (SetParent: same numbers, another parent). */
static int begin_pos(pos_job *job, window *w, HWND after, int x, int y, int cx, int cy, UINT flags, int moved) {
    zero(job, sizeof(*job));
    job->w = w;
    job->hwnd = handle_of(w);
    if ((flags & SWP_SHOWWINDOW) != 0 && (flags & SWP_HIDEWINDOW) != 0) {
        flags &= ~(UINT)SWP_HIDEWINDOW;
    }
    WINDOWPOS *pos = &job->pos;
    pos->hwnd = job->hwnd;
    pos->hwndInsertAfter = after;
    pos->x = x;
    pos->y = y;
    pos->cx = cx;
    pos->cy = cy;
    pos->flags = flags;
    const int top_z = w->parent == 0 && (flags & SWP_NOZORDER) == 0;
    if ((flags & SWP_NOSENDCHANGING) == 0) {
        if (top_z) {
            z_group(w, pos);
        } else {
            send(w, WM_WINDOWPOSCHANGING, 0, (LPARAM)pos);
        }
    }
    if (window_of(job->hwnd) == 0) {
        return 0;
    }
    flags = pos->flags;
    const sxw_rect old = w->rect;
    const sxw_rect old_client = w->client;
    sxw_rect next = old;
    if ((flags & SWP_NOMOVE) == 0) {
        sxw_rect_offset(&next, pos->x - old.left, pos->y - old.top);
    }
    if ((flags & SWP_NOSIZE) == 0 && (w->parent != 0 || own_frame(w))) {
        next.right = next.left + (pos->cx > 0 ? pos->cx : 0);
        next.bottom = next.top + (pos->cy > 0 ? pos->cy : 0);
    }
    moved |= next.left != old.left || next.top != old.top;
    const int sized = next.right - next.left != old.right - old.left || next.bottom - next.top != old.bottom - old.top;
    int z_changed = 0;
    if ((flags & SWP_NOZORDER) == 0) {
        if (w->parent == 0) {
            z_changed = 1;
        } else {
            const int before = sibling_index(w);
            window *target = pos->hwndInsertAfter == HWND_TOP || pos->hwndInsertAfter == HWND_TOPMOST ? 0
                             : pos->hwndInsertAfter == HWND_BOTTOM ? (window *)1
                             : pos->hwndInsertAfter == HWND_NOTOPMOST ? 0
                                                                       : window_of(pos->hwndInsertAfter);
            if (target != (window *)1 && target != 0 && target->parent != w->parent) {
                target = 0;
            }
            if (target != w) {
                unlink_child(w);
                link_child_at(w->parent, w, target);
                z_changed = sibling_index(w) != before;
            }
        }
    }
    const int visible = (w->style & WS_VISIBLE) != 0;
    const int show = (flags & SWP_SHOWWINDOW) != 0 && !visible;
    const int hide = (flags & SWP_HIDEWINDOW) != 0 && visible;
    const int frame = (flags & SWP_FRAMECHANGED) != 0;
    if (!moved && !sized && !z_changed && !show && !hide && !frame) {
        job->deactivate = (flags & SWP_HIDEWINDOW) != 0 && w->parent == 0 && g_active == w;
        return 1;
    }
    job->changed = 1;
    sxw_rect client = old_client;
    if (sized || frame) {
        NCCALCSIZE_PARAMS p;
        p.rgrc[0].left = next.left;
        p.rgrc[0].top = next.top;
        p.rgrc[0].right = next.right;
        p.rgrc[0].bottom = next.bottom;
        p.rgrc[1].left = old.left;
        p.rgrc[1].top = old.top;
        p.rgrc[1].right = old.right;
        p.rgrc[1].bottom = old.bottom;
        p.rgrc[2].left = old_client.left;
        p.rgrc[2].top = old_client.top;
        p.rgrc[2].right = old_client.right;
        p.rgrc[2].bottom = old_client.bottom;
        p.lppos = pos;
        send(w, WM_NCCALCSIZE, TRUE, (LPARAM)&p);
        if (window_of(job->hwnd) == 0) {
            return 0;
        }
        client.left = p.rgrc[0].left;
        client.top = p.rgrc[0].top;
        client.right = p.rgrc[0].right > p.rgrc[0].left ? p.rgrc[0].right : p.rgrc[0].left;
        client.bottom = p.rgrc[0].bottom > p.rgrc[0].top ? p.rgrc[0].bottom : p.rgrc[0].top;
    } else {
        sxw_rect_offset(&client, next.left - old.left, next.top - old.top);
    }
    if (w->parent == 0 && own_frame(w)) {
        /* A popup with its own frame: its surface is the whole window, and
         * the host window follows it. */
        if (sized) {
            size_surface(w, next.right - next.left, next.bottom - next.top);
        }
    } else if (w->parent == 0) {
        /* The window manager owns the size: the surface stays. */
        client.right = client.left + (old_client.right - old_client.left);
        client.bottom = client.top + (old_client.bottom - old_client.top);
        next.right = next.left + (old.right - old.left);
        next.bottom = next.top + (old.bottom - old.top);
    }
    const int client_moved = moved || client.left - next.left != old_client.left - old.left ||
                             client.top - next.top != old_client.top - old.top;
    const int client_sized = client.right - client.left != old_client.right - old_client.left ||
                             client.bottom - client.top != old_client.bottom - old_client.top;
    const int redraw = (flags & SWP_NOREDRAW) == 0 && w->parent != 0 && visible_chain(w->parent);
    if (redraw && visible && (moved || sized || hide)) {
        invalidate(w->parent, &old, 1, 0);
    }
    w->rect = next;
    w->client = client;
    if (w->parent == 0 && own_frame(w) && w->host != 0 && (moved || sized) && !hide) {
        drop_host(w);
        invalidate_frame(w);
        invalidate(w, 0, 1, 1);
        show_host(w);
    }
    if (w->parent != 0 && (flags & SWP_NOACTIVATE) == 0) {
        send(w, WM_CHILDACTIVATE, 0, 0);
        if (window_of(job->hwnd) == 0) {
            return 0;
        }
    }
    if (show) {
        w->style |= WS_VISIBLE;
        if (w->parent == 0) {
            if (!w->message_only && !show_host(w)) {
                w->style &= ~(DWORD)WS_VISIBLE;
                return 0;
            }
            if ((flags & SWP_NOACTIVATE) == 0) {
                activate(w);
            }
        }
        invalidate_frame(w);
        invalidate(w, 0, 1, 1);
        if (visible_chain(w)) {
            erase_now(w, 1);
        }
    } else if (hide) {
        w->style &= ~(DWORD)WS_VISIBLE;
        if (w->parent == 0) {
            drop_host(w);
            job->deactivate = 1;
        } else if (redraw) {
            /* The parent's uncovered part is erased now, before
             * WM_WINDOWPOSCHANGED, as Windows XP does. */
            erase_now(w->parent, 0);
        }
    } else if (redraw && visible && (moved || sized || frame)) {
        invalidate_frame(w);
        invalidate(w, 0, 1, 1);
    } else if (frame) {
        invalidate_frame(w);
    }
    if (sized && (w->cls->style & (CS_HREDRAW | CS_VREDRAW)) != 0) {
        invalidate(w, 0, 1, 0);
    }
    if (!moved) {
        pos->flags |= SWP_NOMOVE;
    }
    if (!sized) {
        pos->flags |= SWP_NOSIZE;
    }
    if (!z_changed) {
        pos->flags |= SWP_NOZORDER;
    }
    if (!client_sized) {
        pos->flags |= SWP_NOCLIENTSIZE;
    }
    if (!client_moved) {
        pos->flags |= SWP_NOCLIENTMOVE;
    }
    if (w->parent != 0 && !visible_chain(w->parent)) {
        pos->flags |= SWP_NOREDRAW;
    }
    pos->x = next.left;
    pos->y = next.top;
    pos->cx = next.right - next.left;
    pos->cy = next.bottom - next.top;
    return 1;
}

/* WM_WINDOWPOSCHANGED; then, for a top-level window hidden while it was
 * active, the activation moves on, as Windows XP does it after. */
static void end_pos(pos_job *job) {
    if (job->changed && window_of(job->hwnd) != 0) {
        send(job->w, WM_WINDOWPOSCHANGED, 0, (LPARAM)&job->pos);
    }
    if (job->deactivate && window_of(job->hwnd) != 0) {
        deactivate(job->w);
    }
}

BOOL set_window_pos(window *w, HWND after, int x, int y, int cx, int cy, UINT flags) {
    pos_job job;
    if (!begin_pos(&job, w, after, x, y, cx, cy, flags, 0)) {
        return window_of(job.hwnd) != 0 ? FALSE : TRUE;
    }
    end_pos(&job);
    return TRUE;
}

BOOL WINAPI SetWindowPos(HWND hWnd, HWND hWndInsertAfter, int X, int Y, int cx, int cy, UINT uFlags) {
    window *w = checked(hWnd);
    if (w == 0) {
        return FALSE;
    }
    if (w->parent == 0 && !own_frame(w) && (uFlags & SWP_NOSIZE) == 0) {
        /* The window manager owns a top-level window's size. */
        cx = w->rect.right - w->rect.left;
        cy = w->rect.bottom - w->rect.top;
    }
    return set_window_pos(w, hWndInsertAfter, X, Y, cx, cy, uFlags);
}

BOOL WINAPI BringWindowToTop(HWND hWnd) {
    return SetWindowPos(hWnd, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
}

/* ---- Deferred positions ---------------------------------------------------------------------- */

#define MAX_DEFERRED 64

typedef struct deferred {
    int used;
    int count;
    struct {
        HWND hwnd;
        HWND after;
        int x;
        int y;
        int cx;
        int cy;
        UINT flags;
    } items[MAX_DEFERRED];
} deferred;

static deferred g_deferred[4];

static deferred *deferred_of(HDWP h) {
    const u32 index = (u32)h;
    return index >= 1 && index <= 4 && g_deferred[index - 1].used ? &g_deferred[index - 1] : 0;
}

HDWP WINAPI BeginDeferWindowPos(int nNumWindows) {
    if (nNumWindows < 0) {
        set_last_error(ERROR_INVALID_PARAMETER);
        return NULL;
    }
    for (int i = 0; i < 4; ++i) {
        if (!g_deferred[i].used) {
            g_deferred[i].used = 1;
            g_deferred[i].count = 0;
            return (HDWP)(u32)(i + 1);
        }
    }
    set_last_error(ERROR_NOT_ENOUGH_MEMORY);
    return NULL;
}

HDWP WINAPI DeferWindowPos(HDWP hWinPosInfo, HWND hWnd, HWND hWndInsertAfter, int x, int y, int cx, int cy,
                           UINT uFlags) {
    deferred *d = deferred_of(hWinPosInfo);
    if (d == 0 || checked(hWnd) == 0) {
        if (d != 0) {
            d->used = 0;
        }
        return NULL;
    }
    int slot = d->count;
    for (int i = 0; i < d->count; ++i) {
        if (d->items[i].hwnd == hWnd) {
            slot = i;
        }
    }
    if (slot == MAX_DEFERRED) {
        set_last_error(ERROR_NOT_ENOUGH_MEMORY);
        d->used = 0;
        return NULL;
    }
    if (slot == d->count) {
        ++d->count;
    }
    d->items[slot].hwnd = hWnd;
    d->items[slot].after = hWndInsertAfter;
    d->items[slot].x = x;
    d->items[slot].y = y;
    d->items[slot].cx = cx;
    d->items[slot].cy = cy;
    d->items[slot].flags = uFlags;
    return hWinPosInfo;
}

/* Every window hears WM_WINDOWPOSCHANGING (and WM_NCCALCSIZE) first, then
 * every one WM_WINDOWPOSCHANGED, in the order they were given. */
BOOL WINAPI EndDeferWindowPos(HDWP hWinPosInfo) {
    deferred *d = deferred_of(hWinPosInfo);
    if (d == 0) {
        set_last_error(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    static pos_job jobs[MAX_DEFERRED];
    const int count = d->count;
    d->used = 0;
    for (int i = 0; i < count; ++i) {
        window *w = window_of(d->items[i].hwnd);
        jobs[i].changed = 0;
        jobs[i].hwnd = d->items[i].hwnd;
        if (w != 0) {
            int cx = d->items[i].cx;
            int cy = d->items[i].cy;
            if (w->parent == 0) {
                cx = w->rect.right - w->rect.left;
                cy = w->rect.bottom - w->rect.top;
            }
            begin_pos(&jobs[i], w, d->items[i].after, d->items[i].x, d->items[i].y, cx, cy, d->items[i].flags, 0);
        }
    }
    for (int i = 0; i < count; ++i) {
        end_pos(&jobs[i]);
    }
    return TRUE;
}

/* ---- SetParent --------------------------------------------------------------------------- */

/* A child moves to another parent at the same numbers, on top of its new
 * siblings: hidden first and shown again, as Windows XP does it. Only
 * children move between parents here. */
HWND WINAPI SetParent(HWND hWndChild, HWND hWndNewParent) {
    window *w = checked(hWndChild);
    if (w == 0) {
        return NULL;
    }
    window *parent = hWndNewParent != NULL && hWndNewParent != DESKTOP_HANDLE ? checked(hWndNewParent) : 0;
    if (parent == 0 || w->parent == 0 || within(parent, w)) {
        set_last_error(parent == 0 && hWndNewParent != NULL ? ERROR_INVALID_WINDOW_HANDLE : ERROR_INVALID_PARAMETER);
        return NULL;
    }
    window *old = w->parent;
    const HWND old_handle = handle_of(old);
    const int was_visible = (w->style & WS_VISIBLE) != 0;
    if (was_visible) {
        ShowWindow(hWndChild, SW_HIDE);
    }
    if (window_of(hWndChild) == 0) {
        return old_handle;
    }
    unlink_child(w);
    link_child_at(parent, w, 0);
    if (top_of(parent)->surface.pixels == 0 && top_of(old)->surface.pixels != 0) {
        /* Nothing to draw on until the new top-level window has a surface. */
    }
    pos_job job;
    if (begin_pos(&job, w, HWND_TOP, w->rect.left, w->rect.top, 0, 0, SWP_NOSIZE | SWP_NOZORDER, 1)) {
        end_pos(&job);
    }
    if (was_visible && window_of(hWndChild) != 0) {
        ShowWindow(hWndChild, SW_SHOW);
    }
    return old_handle;
}

/* ---- Window longs and subclassing ------------------------------------------------------------ */

static int is_dialog(const window *w) {
    return w->dialog != 0 || w->cls->atom == 0x8002;
}

static LONG get_extra(window *w, int index) {
    LONG v = 0;
    for (int i = 0; i < 4; ++i) {
        v |= (LONG)((u32)w->extra[index + i] << (8 * i));
    }
    return v;
}

static void set_extra(window *w, int index, LONG value) {
    for (int i = 0; i < 4; ++i) {
        w->extra[index + i] = (unsigned char)((u32)value >> (8 * i));
    }
}

static DLGPROC *dialog_proc_slot(window *w);

LONG WINAPI GetWindowLongW(HWND hWnd, int nIndex) {
    window *w = checked(hWnd);
    if (w == 0) {
        return 0;
    }
    switch (nIndex) {
    case GWL_WNDPROC:
        return (LONG)w->proc;
    case GWL_HINSTANCE:
        return (LONG)w->instance;
    case GWL_HWNDPARENT:
        return (LONG)handle_of(w->parent != 0 ? w->parent : w->owner);
    case GWL_ID:
        return (LONG)w->id;
    case GWL_STYLE:
        return (LONG)w->style;
    case GWL_EXSTYLE:
        return (LONG)w->ex_style;
    case GWL_USERDATA:
        return w->user_data;
    default:
        break;
    }
    if (nIndex == DWL_DLGPROC && is_dialog(w) && dialog_proc_slot(w) != 0) {
        return (LONG)*dialog_proc_slot(w);
    }
    if (nIndex < 0 || nIndex + 4 > w->extra_size || w->extra == 0) {
        set_last_error(ERROR_INVALID_INDEX);
        return 0;
    }
    return get_extra(w, nIndex);
}

static void change_style(window *w, int index, DWORD style) {
    STYLESTRUCT s;
    s.styleOld = index == GWL_STYLE ? w->style : w->ex_style;
    s.styleNew = style;
    const HWND h = handle_of(w);
    send(w, WM_STYLECHANGING, (WPARAM)index, (LPARAM)&s);
    if (window_of(h) == 0) {
        return;
    }
    if (index == GWL_STYLE) {
        w->style = s.styleNew;
    } else {
        w->ex_style = s.styleNew;
    }
    send(w, WM_STYLECHANGED, (WPARAM)index, (LPARAM)&s);
}

LONG WINAPI SetWindowLongW(HWND hWnd, int nIndex, LONG dwNewLong) {
    window *w = checked(hWnd);
    if (w == 0) {
        return 0;
    }
    LONG old;
    switch (nIndex) {
    case GWL_WNDPROC:
        old = (LONG)w->proc;
        w->proc = (WNDPROC)dwNewLong;
        return old;
    case GWL_HINSTANCE:
        old = (LONG)w->instance;
        w->instance = (HINSTANCE)dwNewLong;
        return old;
    case GWL_HWNDPARENT:
        if (w->parent != 0) {
            return (LONG)SetParent(hWnd, (HWND)dwNewLong);
        }
        old = (LONG)handle_of(w->owner);
        w->owner = dwNewLong != 0 ? window_of((HWND)dwNewLong) : 0;
        if (w->owner != 0) {
            w->owner = top_of(w->owner);
        }
        return old;
    case GWL_ID:
        old = (LONG)w->id;
        w->id = (UINT_PTR)dwNewLong;
        return old;
    case GWL_STYLE:
    case GWL_EXSTYLE:
        old = (LONG)(nIndex == GWL_STYLE ? w->style : w->ex_style);
        change_style(w, nIndex, (DWORD)dwNewLong);
        return old;
    case GWL_USERDATA:
        old = w->user_data;
        w->user_data = dwNewLong;
        return old;
    default:
        break;
    }
    if (nIndex == DWL_DLGPROC && is_dialog(w) && dialog_proc_slot(w) != 0) {
        DLGPROC *slot = dialog_proc_slot(w);
        old = (LONG)*slot;
        *slot = (DLGPROC)dwNewLong;
        return old;
    }
    if (nIndex < 0 || nIndex + 4 > w->extra_size || w->extra == 0) {
        set_last_error(ERROR_INVALID_INDEX);
        return 0;
    }
    old = get_extra(w, nIndex);
    set_extra(w, nIndex, dwNewLong);
    return old;
}

static DLGPROC *dialog_proc_slot(window *w) {
    return dialog_proc_of(w);
}

LRESULT WINAPI CallWindowProcW(WNDPROC lpPrevWndFunc, HWND hWnd, UINT Msg, WPARAM wParam, LPARAM lParam) {
    if (lpPrevWndFunc == NULL) {
        return 0;
    }
    return lpPrevWndFunc(hWnd, Msg, wParam, lParam);
}

/* ---- Properties ------------------------------------------------------------------------------ */

static prop *find_prop(window *w, ATOM atom) {
    for (int i = 0; i < w->prop_count; ++i) {
        if (w->props[i].atom == atom) {
            return &w->props[i];
        }
    }
    return 0;
}

/* The atom a property name stands for, without adding one: 0 when the
 * name was never added. */
static ATOM prop_atom(LPCWSTR name) {
    if ((ULONG_PTR)name < 0x10000u) {
        return (ATOM)(ULONG_PTR)name;
    }
    return GlobalFindAtomW(name);
}

BOOL WINAPI SetPropW(HWND hWnd, LPCWSTR lpString, HANDLE hData) {
    window *w = checked(hWnd);
    if (w == 0 || lpString == NULL) {
        return FALSE;
    }
    ATOM atom = prop_atom(lpString);
    prop *p = atom != 0 ? find_prop(w, atom) : 0;
    if (p != 0) {
        p->data = hData;
        return TRUE;
    }
    const int named = (ULONG_PTR)lpString >= 0x10000u;
    if (named) {
        atom = GlobalAddAtomW(lpString);
        if (atom == 0) {
            return FALSE;
        }
    }
    if (w->prop_count == w->prop_capacity) {
        const int capacity = w->prop_capacity == 0 ? 4 : w->prop_capacity * 2;
        prop *grown = SxwHeapAllocate((u32)capacity * sizeof(prop), 0);
        if (grown == 0) {
            if (named) {
                GlobalDeleteAtom(atom);
            }
            set_last_error(ERROR_NOT_ENOUGH_MEMORY);
            return FALSE;
        }
        for (int i = 0; i < w->prop_count; ++i) {
            grown[i] = w->props[i];
        }
        if (w->props != 0) {
            SxwHeapFree(w->props);
        }
        w->props = grown;
        w->prop_capacity = capacity;
    }
    p = &w->props[w->prop_count++];
    p->atom = atom;
    p->named = named;
    p->data = hData;
    return TRUE;
}

HANDLE WINAPI GetPropW(HWND hWnd, LPCWSTR lpString) {
    window *w = window_of(hWnd);
    const ATOM atom = lpString != NULL ? prop_atom(lpString) : 0;
    prop *p = w != 0 && atom != 0 ? find_prop(w, atom) : 0;
    return p != 0 ? p->data : NULL;
}

HANDLE WINAPI RemovePropW(HWND hWnd, LPCWSTR lpString) {
    window *w = checked(hWnd);
    const ATOM atom = w != 0 && lpString != NULL ? prop_atom(lpString) : 0;
    prop *p = atom != 0 ? find_prop(w, atom) : 0;
    if (p == 0) {
        return NULL;
    }
    const HANDLE data = p->data;
    if (p->named) {
        GlobalDeleteAtom(p->atom);
    }
    *p = w->props[--w->prop_count];
    return data;
}

void free_props(window *w) {
    for (int i = 0; i < w->prop_count; ++i) {
        if (w->props[i].named) {
            GlobalDeleteAtom(w->props[i].atom);
        }
    }
    if (w->props != 0) {
        SxwHeapFree(w->props);
    }
    w->props = 0;
    w->prop_count = w->prop_capacity = 0;
}

/* ---- The window tree --------------------------------------------------------------------------- */

HWND WINAPI GetParent(HWND hWnd) {
    window *w = checked(hWnd);
    if (w == 0) {
        return NULL;
    }
    if ((w->style & WS_CHILD) != 0) {
        return handle_of(w->parent);
    }
    return (w->style & WS_POPUP) != 0 ? handle_of(w->owner) : NULL;
}

static window *last_sibling(window *w) {
    while (w != 0 && w->next != 0) {
        w = w->next;
    }
    return w;
}

/* The top-level windows by z-order: the one above or below w. */
static window *top_level_near(window *w, int below) {
    window *best = 0;
    for (int i = 0; i < MAX_WINDOWS; ++i) {
        window *o = &g_windows[i];
        if (!o->used || o->parent != 0 || o->message_only || o == w) {
            continue;
        }
        if (below ? (o->z < w->z && (best == 0 || o->z > best->z)) : (o->z > w->z && (best == 0 || o->z < best->z))) {
            best = o;
        }
    }
    return best;
}

HWND WINAPI GetWindow(HWND hWnd, UINT uCmd) {
    window *w = checked(hWnd);
    if (w == 0) {
        return NULL;
    }
    switch (uCmd) {
    case GW_CHILD:
        return handle_of(w->child);
    case GW_OWNER:
        return w->parent == 0 ? handle_of(w->owner) : NULL;
    case GW_HWNDFIRST:
        if (w->parent != 0) {
            return handle_of(w->parent->child);
        }
        {
            window *first = w;
            for (window *o = top_level_near(w, 0); o != 0; o = top_level_near(o, 0)) {
                first = o;
            }
            return handle_of(first);
        }
    case GW_HWNDLAST:
        if (w->parent != 0) {
            return handle_of(last_sibling(w->parent->child));
        }
        {
            window *last = w;
            for (window *o = top_level_near(w, 1); o != 0; o = top_level_near(o, 1)) {
                last = o;
            }
            return handle_of(last);
        }
    case GW_HWNDNEXT:
        return w->parent != 0 ? handle_of(w->next) : handle_of(top_level_near(w, 1));
    case GW_HWNDPREV:
        if (w->parent != 0) {
            window *prev = 0;
            for (window *c = w->parent->child; c != 0 && c != w; c = c->next) {
                prev = c;
            }
            return handle_of(prev);
        }
        return handle_of(top_level_near(w, 0));
    case GW_ENABLEDPOPUP:
        for (int i = 0; i < MAX_WINDOWS; ++i) {
            window *o = &g_windows[i];
            if (o->used && o->parent == 0 && o->owner == w && (o->style & (WS_VISIBLE | WS_DISABLED)) == WS_VISIBLE) {
                return handle_of(o);
            }
        }
        return NULL;
    default:
        set_last_error(ERROR_INVALID_PARAMETER);
        return NULL;
    }
}

HWND WINAPI GetTopWindow(HWND hWnd) {
    if (hWnd == NULL || hWnd == DESKTOP_HANDLE) {
        window *best = 0;
        for (int i = 0; i < MAX_WINDOWS; ++i) {
            window *o = &g_windows[i];
            if (o->used && o->parent == 0 && !o->message_only && (best == 0 || o->z > best->z)) {
                best = o;
            }
        }
        return handle_of(best);
    }
    window *w = checked(hWnd);
    return w != 0 ? handle_of(w->child) : NULL;
}

BOOL WINAPI IsChild(HWND hWndParent, HWND hWnd) {
    window *parent = window_of(hWndParent);
    window *w = window_of(hWnd);
    if (parent == 0 || w == 0) {
        return FALSE;
    }
    for (window *p = w->parent; p != 0; p = p->parent) {
        if (p == parent) {
            return TRUE;
        }
        if ((p->style & WS_CHILD) == 0) {
            break;
        }
    }
    return FALSE;
}

BOOL WINAPI IsWindowVisible(HWND hWnd) {
    window *w = window_of(hWnd);
    return w != 0 && visible_chain(w);
}

BOOL WINAPI IsWindowEnabled(HWND hWnd) {
    window *w = window_of(hWnd);
    return w != 0 && (w->style & WS_DISABLED) == 0;
}

BOOL WINAPI IsZoomed(HWND hWnd) {
    window *w = window_of(hWnd);
    return w != 0 && (w->style & WS_MAXIMIZE) != 0;
}

/* Children depth first, each before its own children; the handles are
 * taken first, so the callback may destroy windows. */
static int enum_children(window *w, WNDENUMPROC proc, LPARAM lParam) {
    HWND list[MAX_WINDOWS];
    int n = 0;
    for (window *c = w->child; c != 0 && n < MAX_WINDOWS; c = c->next) {
        list[n++] = handle_of(c);
    }
    for (int i = 0; i < n; ++i) {
        window *c = window_of(list[i]);
        if (c == 0) {
            continue;
        }
        if (!proc(list[i], lParam)) {
            return 0;
        }
        c = window_of(list[i]);
        if (c != 0 && !enum_children(c, proc, lParam)) {
            return 0;
        }
    }
    return 1;
}

BOOL WINAPI EnumChildWindows(HWND hWndParent, WNDENUMPROC lpEnumFunc, LPARAM lParam) {
    if (hWndParent == NULL) {
        return EnumWindows(lpEnumFunc, lParam);
    }
    window *w = checked(hWndParent);
    if (w == 0 || lpEnumFunc == NULL) {
        return FALSE;
    }
    return enum_children(w, lpEnumFunc, lParam);
}

BOOL WINAPI EnumWindows(WNDENUMPROC lpEnumFunc, LPARAM lParam) {
    HWND list[MAX_WINDOWS];
    int n = 0;
    for (HWND h = GetTopWindow(NULL); h != NULL && n < MAX_WINDOWS; h = GetWindow(h, GW_HWNDNEXT)) {
        list[n++] = h;
    }
    for (int i = 0; i < n; ++i) {
        if (window_of(list[i]) != 0 && !lpEnumFunc(list[i], lParam)) {
            return FALSE;
        }
    }
    return TRUE;
}

int WINAPI GetDlgCtrlID(HWND hWnd) {
    window *w = checked(hWnd);
    return w != 0 && (w->style & WS_CHILD) != 0 ? (int)w->id : 0;
}

/* ---- Classes ---------------------------------------------------------------------------------- */

int WINAPI GetClassNameW(HWND hWnd, LPWSTR lpClassName, int nMaxCount) {
    window *w = checked(hWnd);
    if (w == 0 || lpClassName == NULL || nMaxCount <= 0) {
        return 0;
    }
    return copy_text(lpClassName, w->cls->name, nMaxCount);
}

BOOL WINAPI GetClassInfoW(HINSTANCE hInstance, LPCWSTR lpClassName, WNDCLASSW *lpWndClass) {
    (void)hInstance;
    wclass *c = lpClassName != NULL ? find_class(lpClassName) : 0;
    if (c == 0 || lpWndClass == NULL) {
        set_last_error(ERROR_CLASS_DOES_NOT_EXIST);
        return FALSE;
    }
    lpWndClass->style = c->style;
    lpWndClass->lpfnWndProc = c->proc;
    lpWndClass->cbClsExtra = 0;
    lpWndClass->cbWndExtra = c->window_extra;
    lpWndClass->hInstance = c->instance;
    lpWndClass->hIcon = c->icon;
    lpWndClass->hCursor = c->cursor;
    lpWndClass->hbrBackground = c->background;
    lpWndClass->lpszMenuName = c->menu_name;
    lpWndClass->lpszClassName = c->name;
    return c->atom;
}

BOOL WINAPI UnregisterClassW(LPCWSTR lpClassName, HINSTANCE hInstance) {
    (void)hInstance;
    wclass *c = lpClassName != NULL ? find_class(lpClassName) : 0;
    if (c == 0 || c->instance == NULL) {
        set_last_error(ERROR_CLASS_DOES_NOT_EXIST);
        return FALSE;
    }
    if (c->windows != 0) {
        set_last_error(ERROR_CLASS_HAS_WINDOWS);
        return FALSE;
    }
    remove_class(c);
    return TRUE;
}

/* ---- Text --------------------------------------------------------------------------------------- */

int WINAPI GetWindowTextW(HWND hWnd, LPWSTR lpString, int nMaxCount) {
    window *w = checked(hWnd);
    if (w == 0 || lpString == NULL || nMaxCount <= 0) {
        return 0;
    }
    lpString[0] = 0;
    return (int)send(w, WM_GETTEXT, (WPARAM)nMaxCount, (LPARAM)lpString);
}

int WINAPI GetWindowTextLengthW(HWND hWnd) {
    window *w = checked(hWnd);
    return w != 0 ? (int)send(w, WM_GETTEXTLENGTH, 0, 0) : 0;
}

BOOL WINAPI SetWindowTextW(HWND hWnd, LPCWSTR lpString) {
    window *w = checked(hWnd);
    if (w == 0) {
        return FALSE;
    }
    static const WCHAR k_empty[] = {0};
    return send(w, WM_SETTEXT, 0, (LPARAM)(lpString != NULL ? lpString : k_empty)) != 0;
}

/* ---- Activation and focus queries ---------------------------------------------------------------- */

HWND WINAPI GetFocus(void) {
    return handle_of(g_focus);
}

HWND WINAPI GetForegroundWindow(void) {
    return handle_of(g_active);
}

HWND WINAPI SetActiveWindow(HWND hWnd) {
    window *w = checked(hWnd);
    if (w == 0) {
        return NULL;
    }
    window *old = g_active;
    w = top_of(w);
    if ((w->style & (WS_VISIBLE | WS_DISABLED)) == WS_VISIBLE) {
        activate(w);
    }
    return handle_of(old);
}

BOOL WINAPI SetForegroundWindow(HWND hWnd) {
    return SetActiveWindow(hWnd) != NULL || g_active == window_of(hWnd);
}

/* The owned window last active, or the window itself. */
HWND WINAPI GetLastActivePopup(HWND hWnd) {
    window *w = checked(hWnd);
    if (w == 0) {
        return NULL;
    }
    if (g_active != 0 && g_active->owner == w) {
        return handle_of(g_active);
    }
    return hWnd;
}

/* Owned windows are hidden with their owner by the window manager; there
 * is nothing more to do here. */
BOOL WINAPI ShowOwnedPopups(HWND hWnd, BOOL fShow) {
    (void)fShow;
    return checked(hWnd) != 0;
}

BOOL WINAPI GetWindowPlacement(HWND hWnd, WINDOWPLACEMENT *lpwndpl) {
    window *w = checked(hWnd);
    if (w == 0 || lpwndpl == NULL) {
        return FALSE;
    }
    lpwndpl->length = sizeof(WINDOWPLACEMENT);
    lpwndpl->flags = 0;
    lpwndpl->showCmd = (w->style & WS_MINIMIZE) != 0   ? SW_SHOWMINIMIZED
                       : (w->style & WS_MAXIMIZE) != 0 ? SW_SHOWMAXIMIZED
                                                        : SW_SHOWNORMAL;
    lpwndpl->ptMinPosition.x = lpwndpl->ptMinPosition.y = -1;
    lpwndpl->ptMaxPosition.x = lpwndpl->ptMaxPosition.y = -1;
    lpwndpl->rcNormalPosition.left = w->rect.left;
    lpwndpl->rcNormalPosition.top = w->rect.top;
    lpwndpl->rcNormalPosition.right = w->rect.right;
    lpwndpl->rcNormalPosition.bottom = w->rect.bottom;
    return TRUE;
}

/* The window manager places and sizes a top-level window; a placement
 * moves a child and shows the window as it says. */
BOOL WINAPI SetWindowPlacement(HWND hWnd, const WINDOWPLACEMENT *lpwndpl) {
    window *w = checked(hWnd);
    if (w == 0 || lpwndpl == NULL) {
        return FALSE;
    }
    if (w->parent != 0) {
        const RECT *r = &lpwndpl->rcNormalPosition;
        set_window_pos(w, NULL, r->left, r->top, r->right - r->left, r->bottom - r->top,
                       SWP_NOZORDER | SWP_NOACTIVATE);
    }
    ShowWindow(hWnd, lpwndpl->showCmd == SW_SHOWMINIMIZED || lpwndpl->showCmd == SW_SHOWMAXIMIZED
                         ? SW_SHOWNORMAL
                         : (int)lpwndpl->showCmd);
    return TRUE;
}
