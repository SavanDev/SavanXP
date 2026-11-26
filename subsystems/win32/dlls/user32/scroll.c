/*
 * scroll.c -- Scrolling (docs/userland/WIN32.md, "Scroll bars"): the scroll
 * bars of a window with WS_HSCROLL or WS_VSCROLL, in its non-client area,
 * the ScrollBar control, both followed with the pointer, and
 * ScrollWindowEx.
 *
 * The bars keep a range, a page and a position as Windows XP does
 * (fixtures/hooks): the page is cut to the range, the position to what the
 * page leaves, and a window's bar is hidden when its page covers its range
 * and shown again when a range is set. They look like Windows XP's classic
 * bars: arrow buttons, a checkered track and a raised thumb.
 */
#include "user32_private.h"

#define ATOM_SCROLLBAR 0x8004
#define ERROR_NO_SCROLLBARS 1447u
#define MIN_THUMB 8
#define FIRST_REPEAT_MS 400u
#define REPEAT_MS 50u
#define SBM_SETPOS 0x00e0
#define SBM_GETPOS 0x00e1
#define SBM_SETRANGE 0x00e2
#define SBM_GETRANGE 0x00e3
#define SBM_ENABLE_ARROWS 0x00e4
#define SBM_SETRANGEREDRAW 0x00e6
#define SBM_SETSCROLLINFO 0x00e9
#define SBM_GETSCROLLINFO 0x00ea
#define SC_HSCROLL 0xf080
#define SC_VSCROLL 0xf070

/* A window's own bars live in bars[SB_HORZ], bars[SB_VERT]; a control's in
 * bars[0]. has_bars: the window had a bar at some point, so its scroll
 * information exists (fixtures/hooks: a window that never had one answers
 * ERROR_NO_SCROLLBARS). */
#define HAS_BARS 0x10000u

static int is_control(const window *w) {
    return w->cls->atom == ATOM_SCROLLBAR;
}

static int has_info(const window *w) {
    return is_control(w) || (w->control_state & HAS_BARS) != 0 || (w->style & (WS_HSCROLL | WS_VSCROLL)) != 0;
}

static scroll_bar *bar_of(window *w, int bar) {
    if (bar == SB_CTL) {
        return &w->bars[0];
    }
    if (bar != SB_HORZ && bar != SB_VERT) {
        return 0;
    }
    if (is_control(w) || !has_info(w)) {
        return 0;
    }
    w->control_state |= HAS_BARS;
    return &w->bars[bar];
}

static int vertical(const window *w, int bar) {
    return bar == SB_CTL ? (w->style & SBS_VERT) != 0 : bar == SB_VERT;
}

/* a * b / c rounded to the nearest, as Windows XP places and follows the
 * thumb (fixtures/controls); 0 for a nonsensical division. */
static int scale(int a, int b, int c) {
    if (c <= 0 || a < 0 || b < 0) {
        return 0;
    }
    return MulDiv(a, b, c);
}

static int limit(const scroll_bar *b) {
    const int page = b->page > 0 ? b->page - 1 : 0;
    const int top = b->max - page;
    return top > b->min ? top : b->min;
}

static void clamp(scroll_bar *b) {
    if (b->pos < b->min) {
        b->pos = b->min;
    }
    if (b->pos > limit(b)) {
        b->pos = limit(b);
    }
}

/* ---- Geometry ---------------------------------------------------------------------------- */

typedef struct bar_layout {
    RECT whole;  /* in the coordinates of the dc it is drawn on */
    int vertical;
    int arrow;   /* each arrow's length */
    int track;   /* from the end of the first arrow */
    int track_length;
    int thumb;   /* offset from the start of the track */
    int thumb_length; /* 0: no thumb */
} bar_layout;

static void layout(const scroll_bar *b, const RECT *whole, int is_vertical, bar_layout *l) {
    l->whole = *whole;
    l->vertical = is_vertical;
    const int length = is_vertical ? whole->bottom - whole->top : whole->right - whole->left;
    const int thickness = is_vertical ? whole->right - whole->left : whole->bottom - whole->top;
    l->arrow = thickness;
    if (2 * l->arrow > length) {
        l->arrow = length / 2;
    }
    l->track = l->arrow;
    l->track_length = length - 2 * l->arrow;
    l->thumb = 0;
    l->thumb_length = 0;
    const int range = b->max - b->min;
    if (range <= 0 || l->track_length <= 0 || (b->disabled & ESB_DISABLE_BOTH) == ESB_DISABLE_BOTH) {
        return;
    }
    int thumb = b->page > 0 ? scale(l->track_length, b->page, range + 1) : thickness;
    if (thumb < MIN_THUMB) {
        thumb = MIN_THUMB;
    }
    if (thumb > l->track_length) {
        return;
    }
    const int span = limit(b) - b->min;
    l->thumb_length = thumb;
    l->thumb = span > 0 ? scale(l->track_length - thumb, b->pos - b->min, span) : 0;
}

/* Where a window's bar is, in the window's own coordinates (its top-left
 * corner at 0, 0). */
static int window_bar_rect(const window *w, int bar, RECT *r) {
    const int x = w->client.left - w->rect.left;
    const int y = w->client.top - w->rect.top;
    if (bar == SB_VERT && (w->style & WS_VSCROLL) != 0) {
        r->left = x + client_width(w);
        r->right = r->left + SCROLL_EXTENT;
        r->top = y;
        r->bottom = y + client_height(w);
        return r->right <= w->rect.right - w->rect.left;
    }
    if (bar == SB_HORZ && (w->style & WS_HSCROLL) != 0) {
        r->left = x;
        r->right = x + client_width(w);
        r->top = y + client_height(w);
        r->bottom = r->top + SCROLL_EXTENT;
        return r->bottom <= w->rect.bottom - w->rect.top;
    }
    return 0;
}

/* ---- Drawing ------------------------------------------------------------------------------- */

static void raised(HDC dc, RECT r, int pressed) {
    if (pressed) {
        FrameRect(dc, &r, system_brush(COLOR_BTNSHADOW));
        InflateRect(&r, -1, -1);
        fill_rect(dc, &r, GetSysColor(COLOR_BTNFACE));
        return;
    }
    draw_edge(dc, &r, GetSysColor(COLOR_3DLIGHT), GetSysColor(COLOR_3DDKSHADOW), GetSysColor(COLOR_BTNHIGHLIGHT),
              GetSysColor(COLOR_BTNSHADOW));
    fill_rect(dc, &r, GetSysColor(COLOR_BTNFACE));
}

/* The classic arrow as Windows XP draws it (fixtures/controls): for a
 * 16-pixel button a triangle 4 rows deep, 7 pixels at its base, its base
 * centred across the button and its depth from (length - 4) / 2 along it;
 * the one pointing left sits a pixel further left. Direction 0 up, 1 down,
 * 2 left, 3 right. Disabled, it is grey over a highlight one pixel down and
 * right; pressed, it moves a pixel down and right. */
void draw_arrow(HDC dc, const RECT *button, int direction, int disabled, int pressed) {
    const int width = button->right - button->left;
    const int height = button->bottom - button->top;
    const int extent = width < height ? width : height;
    int rows = extent / 4;
    if (rows < 1) {
        rows = 1;
    }
    const int base = 2 * rows - 1;
    const int vertical = direction < 2;
    const int along = vertical ? height : width;
    const int across = vertical ? width : height;
    int first = (along - rows) / 2 - (direction == 2 ? 1 : 0);
    const int side = (across - base) / 2;
    const int offset = pressed ? 1 : 0;
    for (int pass = disabled ? 0 : 1; pass < 2; ++pass) {
        const COLORREF color = disabled ? (pass == 0 ? GetSysColor(COLOR_BTNHIGHLIGHT) : GetSysColor(COLOR_GRAYTEXT))
                                        : GetSysColor(COLOR_BTNTEXT);
        const int shift = (disabled && pass == 0 ? 1 : 0) + offset;
        for (int i = 0; i < rows; ++i) {
            /* Row i from the tip: 2 * i + 1 pixels. */
            const int depth = direction == 0 || direction == 2 ? i : rows - 1 - i;
            const int half = depth;
            RECT line;
            if (vertical) {
                line.top = button->top + first + i + shift;
                line.bottom = line.top + 1;
                line.left = button->left + side + (rows - 1 - half) + shift;
                line.right = line.left + 2 * half + 1;
            } else {
                line.left = button->left + first + i + shift;
                line.right = line.left + 1;
                line.top = button->top + side + (rows - 1 - half) + shift;
                line.bottom = line.top + 2 * half + 1;
            }
            fill_rect(dc, &line, color);
        }
    }
}

/* The track's checkered brush: the highlight colour and the face colour on
 * alternate pixels, counted from the window's corner, the corner itself
 * the face colour. */
static void track(HDC dc, const RECT *r, int pressed) {
    for (int y = r->top; y < r->bottom; ++y) {
        for (int x = r->left; x < r->right; ++x) {
            const int odd = ((x + y) & 1) != 0;
            COLORREF c = odd ? GetSysColor(COLOR_BTNHIGHLIGHT) : GetSysColor(COLOR_BTNFACE);
            if (pressed) {
                c = odd ? GetSysColor(COLOR_BTNSHADOW) : GetSysColor(COLOR_3DDKSHADOW);
            }
            SetPixel(dc, x, y, c);
        }
    }
}

/* Which part is pressed while the pointer follows a bar. */
enum part { PART_NONE, PART_FIRST_ARROW, PART_FIRST_PAGE, PART_THUMB, PART_SECOND_PAGE, PART_SECOND_ARROW };

static void draw_bar(HDC dc, const scroll_bar *b, const bar_layout *l, int pressed_part) {
    const RECT *w = &l->whole;
    RECT first = *w;
    RECT second = *w;
    if (l->vertical) {
        first.bottom = first.top + l->arrow;
        second.top = second.bottom - l->arrow;
    } else {
        first.right = first.left + l->arrow;
        second.left = second.right - l->arrow;
    }
    const int all_off = (b->disabled & ESB_DISABLE_BOTH) == ESB_DISABLE_BOTH || b->max <= b->min;
    raised(dc, first, pressed_part == PART_FIRST_ARROW);
    draw_arrow(dc, &first, l->vertical ? 0 : 2, all_off || (b->disabled & ESB_DISABLE_LTUP) != 0,
               pressed_part == PART_FIRST_ARROW);
    raised(dc, second, pressed_part == PART_SECOND_ARROW);
    draw_arrow(dc, &second, l->vertical ? 1 : 3, all_off || (b->disabled & ESB_DISABLE_RTDN) != 0,
               pressed_part == PART_SECOND_ARROW);
    RECT t = *w;
    if (l->vertical) {
        t.top += l->arrow;
        t.bottom -= l->arrow;
    } else {
        t.left += l->arrow;
        t.right -= l->arrow;
    }
    if (t.right <= t.left || t.bottom <= t.top) {
        return;
    }
    if (l->thumb_length == 0) {
        track(dc, &t, 0);
        return;
    }
    RECT before = t;
    RECT thumb = t;
    RECT after = t;
    if (l->vertical) {
        before.bottom = t.top + l->thumb;
        thumb.top = before.bottom;
        thumb.bottom = thumb.top + l->thumb_length;
        after.top = thumb.bottom;
    } else {
        before.right = t.left + l->thumb;
        thumb.left = before.right;
        thumb.right = thumb.left + l->thumb_length;
        after.left = thumb.right;
    }
    track(dc, &before, pressed_part == PART_FIRST_PAGE);
    track(dc, &after, pressed_part == PART_SECOND_PAGE);
    raised(dc, thumb, 0);
}

/* WM_NCPAINT's bars: dc's origin is the window's corner. */
void nc_paint_scroll_bars(window *w, HDC dc, int left, int top) {
    (void)left;
    (void)top;
    for (int bar = SB_HORZ; bar <= SB_VERT; ++bar) {
        RECT r;
        if (window_bar_rect(w, bar, &r)) {
            bar_layout l;
            layout(&w->bars[bar], &r, bar == SB_VERT, &l);
            draw_bar(dc, &w->bars[bar], &l, PART_NONE);
        }
    }
    RECT v;
    RECT h;
    if (window_bar_rect(w, SB_VERT, &v) && window_bar_rect(w, SB_HORZ, &h)) {
        RECT corner = {v.left, h.top, v.right, h.bottom};
        fill_rect(dc, &corner, GetSysColor(COLOR_BTNFACE));
    }
}

static void redraw_bar(window *w, int bar) {
    if (!visible_chain(w)) {
        return;
    }
    if (bar == SB_CTL) {
        InvalidateRect(handle_of(w), NULL, FALSE);
        return;
    }
    w->frame_dirty = 1;
    nc_paint(w);
    w->frame_dirty = 0;
}

/* ---- The calls ------------------------------------------------------------------------------- */

BOOL WINAPI ShowScrollBar(HWND hWnd, int wBar, BOOL bShow) {
    window *w = checked(hWnd);
    if (w == 0) {
        return FALSE;
    }
    if (wBar == SB_CTL) {
        ShowWindow(hWnd, bShow ? SW_SHOW : SW_HIDE);
        return TRUE;
    }
    DWORD bits = wBar == SB_HORZ ? WS_HSCROLL : wBar == SB_VERT ? WS_VSCROLL : WS_HSCROLL | WS_VSCROLL;
    w->control_state |= HAS_BARS;
    const DWORD before = w->style;
    if (bShow) {
        w->style |= bits;
    } else {
        w->style &= ~bits;
    }
    if (w->style != before) {
        set_window_pos(w, NULL, 0, 0, 0, 0,
                       SWP_NOSIZE | SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    }
    return TRUE;
}

int WINAPI SetScrollInfo(HWND hwnd, int nBar, const SCROLLINFO *lpsi, BOOL redraw) {
    window *w = checked(hwnd);
    if (w == 0 || lpsi == NULL) {
        return 0;
    }
    scroll_bar *b = bar_of(w, nBar);
    if (b == 0) {
        set_last_error(ERROR_NO_SCROLLBARS);
        return 0;
    }
    int changed_params = 0;
    if ((lpsi->fMask & SIF_RANGE) != 0) {
        changed_params |= b->min != lpsi->nMin || b->max != lpsi->nMax;
        b->min = lpsi->nMin;
        b->max = lpsi->nMax > lpsi->nMin ? lpsi->nMax : lpsi->nMin;
    }
    if ((lpsi->fMask & SIF_PAGE) != 0) {
        changed_params |= b->page != (int)lpsi->nPage;
        b->page = (int)lpsi->nPage;
    }
    if (b->page > b->max - b->min + 1) {
        b->page = b->max - b->min + 1;
    }
    if (b->page < 0) {
        b->page = 0;
    }
    if ((lpsi->fMask & SIF_POS) != 0) {
        b->pos = lpsi->nPos;
    }
    clamp(b);
    if ((lpsi->fMask & (SIF_RANGE | SIF_PAGE | SIF_DISABLENOSCROLL)) != 0 && nBar != SB_CTL) {
        if (b->min >= limit(b)) {
            if ((lpsi->fMask & SIF_DISABLENOSCROLL) != 0) {
                b->disabled = ESB_DISABLE_BOTH;
            } else if (changed_params) {
                ShowScrollBar(hwnd, nBar, FALSE);
                return b->pos;
            }
        } else if (lpsi->fMask != SIF_PAGE) {
            b->disabled = 0;
            if ((w->style & (nBar == SB_HORZ ? WS_HSCROLL : WS_VSCROLL)) == 0) {
                ShowScrollBar(hwnd, nBar, TRUE);
                return b->pos;
            }
        }
    }
    if (redraw) {
        redraw_bar(w, nBar);
    }
    return b->pos;
}

BOOL WINAPI GetScrollInfo(HWND hwnd, int nBar, LPSCROLLINFO lpsi) {
    window *w = checked(hwnd);
    if (w == 0 || lpsi == NULL) {
        return FALSE;
    }
    scroll_bar *b = bar_of(w, nBar);
    if (b == 0) {
        set_last_error(ERROR_NO_SCROLLBARS);
        return FALSE;
    }
    if ((lpsi->fMask & SIF_RANGE) != 0) {
        lpsi->nMin = b->min;
        lpsi->nMax = b->max;
    }
    if ((lpsi->fMask & SIF_PAGE) != 0) {
        lpsi->nPage = (UINT)b->page;
    }
    if ((lpsi->fMask & SIF_POS) != 0) {
        lpsi->nPos = b->pos;
    }
    if ((lpsi->fMask & SIF_TRACKPOS) != 0) {
        lpsi->nTrackPos = b->track_pos;
    }
    return TRUE;
}

int WINAPI SetScrollPos(HWND hWnd, int nBar, int nPos, BOOL bRedraw) {
    window *w = checked(hWnd);
    scroll_bar *b = w != 0 ? bar_of(w, nBar) : 0;
    if (b == 0) {
        if (w != 0) {
            set_last_error(ERROR_NO_SCROLLBARS);
        }
        return 0;
    }
    const int old = b->pos;
    SCROLLINFO si;
    si.cbSize = sizeof(si);
    si.fMask = SIF_POS;
    si.nPos = nPos;
    SetScrollInfo(hWnd, nBar, &si, bRedraw);
    return old;
}

int WINAPI GetScrollPos(HWND hWnd, int nBar) {
    window *w = checked(hWnd);
    scroll_bar *b = w != 0 ? bar_of(w, nBar) : 0;
    if (b == 0) {
        if (w != 0) {
            set_last_error(ERROR_NO_SCROLLBARS);
        }
        return 0;
    }
    return b->pos;
}

BOOL WINAPI SetScrollRange(HWND hWnd, int nBar, int nMinPos, int nMaxPos, BOOL bRedraw) {
    SCROLLINFO si;
    si.cbSize = sizeof(si);
    si.fMask = SIF_RANGE;
    si.nMin = nMinPos;
    si.nMax = nMaxPos;
    window *w = checked(hWnd);
    if (w == 0 || bar_of(w, nBar) == 0) {
        if (w != 0) {
            set_last_error(ERROR_NO_SCROLLBARS);
        }
        return FALSE;
    }
    SetScrollInfo(hWnd, nBar, &si, bRedraw);
    return TRUE;
}

BOOL WINAPI GetScrollRange(HWND hWnd, int nBar, int *lpMinPos, int *lpMaxPos) {
    window *w = checked(hWnd);
    scroll_bar *b = w != 0 ? bar_of(w, nBar) : 0;
    if (b == 0 || lpMinPos == NULL || lpMaxPos == NULL) {
        if (lpMinPos != NULL) {
            *lpMinPos = 0;
        }
        if (lpMaxPos != NULL) {
            *lpMaxPos = 0;
        }
        return FALSE;
    }
    *lpMinPos = b->min;
    *lpMaxPos = b->max;
    return TRUE;
}

/* FALSE, with the error left alone, when nothing changes, as on Windows XP. */
BOOL WINAPI EnableScrollBar(HWND hWnd, UINT wSBflags, UINT wArrows) {
    window *w = checked(hWnd);
    if (w == 0) {
        return FALSE;
    }
    int any = 0;
    for (int bar = SB_HORZ; bar <= SB_VERT; ++bar) {
        const int wanted = wSBflags == SB_BOTH || (int)wSBflags == bar || (wSBflags == SB_CTL && bar == 0);
        scroll_bar *b = wanted ? bar_of(w, wSBflags == SB_CTL ? SB_CTL : bar) : 0;
        if (b != 0 && b->disabled != (wArrows & ESB_DISABLE_BOTH)) {
            b->disabled = wArrows & ESB_DISABLE_BOTH;
            redraw_bar(w, wSBflags == SB_CTL ? SB_CTL : bar);
            any = 1;
        }
        if (wSBflags == SB_CTL) {
            break;
        }
    }
    return any;
}

/* ---- Following the pointer ---------------------------------------------------------------------- */

static int part_at(const bar_layout *l, int x, int y) {
    const RECT *r = &l->whole;
    if (x < r->left || y < r->top || x >= r->right || y >= r->bottom) {
        return PART_NONE;
    }
    const int along = l->vertical ? y - r->top : x - r->left;
    const int length = l->vertical ? r->bottom - r->top : r->right - r->left;
    if (along < l->arrow) {
        return PART_FIRST_ARROW;
    }
    if (along >= length - l->arrow) {
        return PART_SECOND_ARROW;
    }
    if (l->thumb_length == 0) {
        return PART_NONE;
    }
    const int t = along - l->track;
    if (t < l->thumb) {
        return PART_FIRST_PAGE;
    }
    if (t >= l->thumb + l->thumb_length) {
        return PART_SECOND_PAGE;
    }
    return PART_THUMB;
}

static WPARAM code_of(int part) {
    switch (part) {
    case PART_FIRST_ARROW: return SB_LINEUP;
    case PART_SECOND_ARROW: return SB_LINEDOWN;
    case PART_FIRST_PAGE: return SB_PAGEUP;
    case PART_SECOND_PAGE: return SB_PAGEDOWN;
    default: return SB_ENDSCROLL;
    }
}

/* The modal loop of a pressed bar: a repeating arrow or page, or a thumb
 * dragged. bar is SB_HORZ, SB_VERT or SB_CTL; (x, y) are in the
 * coordinates draw happens in: the window's corner for its own bars, the
 * client area for a control. Scroll messages go to the window itself for
 * its own bars and to the parent for a control, as on Windows. */
void scroll_track(window *w, int bar, int x, int y) {
    const HWND hwnd = handle_of(w);
    scroll_bar *b = bar_of(w, bar);
    if (b == 0) {
        return;
    }
    RECT r;
    if (bar == SB_CTL) {
        r.left = r.top = 0;
        r.right = client_width(w);
        r.bottom = client_height(w);
    } else if (!window_bar_rect(w, bar, &r)) {
        return;
    }
    bar_layout l;
    layout(b, &r, vertical(w, bar), &l);
    const int part = part_at(&l, x, y);
    if (part == PART_NONE || (b->disabled & ESB_DISABLE_BOTH) == ESB_DISABLE_BOTH) {
        return;
    }
    window *target = bar == SB_CTL ? w->parent : w;
    const UINT message = vertical(w, bar) ? WM_VSCROLL : WM_HSCROLL;
    const LPARAM control = bar == SB_CTL ? (LPARAM)hwnd : 0;
    const int grab = vertical(w, bar) ? y - (r.top + l.track + l.thumb) : x - (r.left + l.track + l.thumb);
    window *old_capture = g_capture;
    g_capture = w;
    if (old_capture != 0 && old_capture != w) {
        send(old_capture, WM_CAPTURECHANGED, 0, (LPARAM)hwnd);
    }
    HDC dc = 0;
    if (bar == SB_CTL) {
        dc = window_dc(w, 0);
    }
    int pressed = part;
    int inside = 1;
    u32 next = ticks() + FIRST_REPEAT_MS;
    b->track_pos = b->pos;
    if (part != PART_THUMB) {
        send(target, message, code_of(part), control);
    } else {
        /* Taking the thumb tells its position at once. */
        send(target, message, MAKELPARAM(SB_THUMBTRACK, b->track_pos), control);
    }
    for (;;) {
        if (window_of(hwnd) == 0) {
            break;
        }
        b = bar_of(w, bar);
        layout(b, &r, vertical(w, bar), &l);
        if (bar == SB_CTL && dc != 0 && visible_chain(w)) {
            draw_bar(dc, b, &l, inside ? pressed : PART_NONE);
        } else if (bar != SB_CTL) {
            redraw_bar(w, bar);
        }
        present_all();
        MSG m;
        if (!next_message(&m, NULL, 0, 0, 1, 0)) {
            const u32 now = ticks();
            if (part != PART_THUMB && inside && (int)(now - next) >= 0) {
                if (part == PART_FIRST_PAGE || part == PART_SECOND_PAGE) {
                    const int at = part_at(&l, x, y);
                    if (at != part) {
                        inside = 0;
                        continue;
                    }
                }
                send(target, message, code_of(part), control);
                next = now + REPEAT_MS;
                continue;
            }
            u32 arguments[2] = {part == PART_THUMB || !inside ? SXW_INFINITE : next - now, 0};
            (void)arguments;
            next_message(&m, NULL, 0, 0, 1, 1);
        }
        if (CallMsgFilterW(&m, MSGF_SCROLLBAR)) {
            continue;
        }
        if (m.message == WM_QUIT) {
            PostQuitMessage((int)m.wParam);
            break;
        }
        if (m.message == WM_MOUSEMOVE || m.message == WM_LBUTTONUP) {
            /* The capture makes the pointer's coordinates the window's
             * client area; a window's own bars count from its corner. */
            x = GET_X_LPARAM(m.lParam);
            y = GET_Y_LPARAM(m.lParam);
            if (bar != SB_CTL) {
                x += w->client.left - w->rect.left;
                y += w->client.top - w->rect.top;
            }
            if (part == PART_THUMB) {
                const int along = (vertical(w, bar) ? y - r.top : x - r.left) - l.track - grab;
                const int room = l.track_length - l.thumb_length;
                const int span = limit(b) - b->min;
                int at = room > 0 && along > 0 ? b->min + scale(along, span, room) : b->min;
                if (at < b->min) {
                    at = b->min;
                }
                if (at > limit(b)) {
                    at = limit(b);
                }
                if (m.message == WM_MOUSEMOVE && at != b->track_pos) {
                    b->track_pos = at;
                    send(target, message, MAKELPARAM(SB_THUMBTRACK, at), control);
                }
            } else {
                inside = part_at(&l, x, y) == part;
            }
            if (m.message == WM_LBUTTONUP) {
                if (part == PART_THUMB) {
                    send(target, message, MAKELPARAM(SB_THUMBPOSITION, b->track_pos), control);
                }
                break;
            }
            continue;
        }
        if (m.message == WM_KEYDOWN && m.wParam == VK_ESCAPE) {
            break;
        }
        if ((m.message >= WM_MOUSEMOVE && m.message <= WM_MBUTTONDBLCLK) || m.message == WM_KEYDOWN ||
            m.message == WM_KEYUP || m.message == WM_CHAR) {
            continue;
        }
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    if (window_of(hwnd) != 0) {
        send(target, message, SB_ENDSCROLL, control);
        b = bar_of(w, bar);
        if (b != 0) {
            layout(b, &r, vertical(w, bar), &l);
            if (bar == SB_CTL && dc != 0 && visible_chain(w)) {
                draw_bar(dc, b, &l, PART_NONE);
            } else if (bar != SB_CTL) {
                redraw_bar(w, bar);
            }
        }
    }
    if (dc != 0) {
        DeleteObject(dc);
    }
    if (g_capture == w) {
        g_capture = 0;
        send(w, WM_CAPTURECHANGED, 0, 0);
    }
}

/* DefWindowProc's hit test of a window's own bars, in screen coordinates. */
LRESULT scroll_nc_hit(window *w, int x, int y) {
    int sx = 0;
    int sy = 0;
    if (w->parent != 0) {
        client_screen_origin(w->parent, &sx, &sy);
    }
    x -= sx + w->rect.left;
    y -= sy + w->rect.top;
    RECT r;
    if (window_bar_rect(w, SB_VERT, &r) && x >= r.left && x < r.right && y >= r.top && y < r.bottom) {
        return HTVSCROLL;
    }
    if (window_bar_rect(w, SB_HORZ, &r) && x >= r.left && x < r.right && y >= r.top && y < r.bottom) {
        return HTHSCROLL;
    }
    return HTNOWHERE;
}

/* ---- The ScrollBar control ---------------------------------------------------------------------- */

static LRESULT WINAPI scroll_bar_proc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    window *w = window_of(hwnd);
    if (w == 0) {
        return 0;
    }
    scroll_bar *b = &w->bars[0];
    switch (message) {
    case WM_CREATE:
        b->min = 0;
        b->max = 0;
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT r = {0, 0, client_width(w), client_height(w)};
        bar_layout l;
        layout(b, &r, (w->style & SBS_VERT) != 0, &l);
        draw_bar(dc, b, &l, PART_NONE);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK:
        scroll_track(w, SB_CTL, GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
        return 0;
    case WM_ENABLE:
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    case SBM_SETPOS:
        return SetScrollPos(hwnd, SB_CTL, (int)wParam, lParam != 0);
    case SBM_GETPOS:
        return b->pos;
    case SBM_SETRANGE:
    case SBM_SETRANGEREDRAW: {
        const int old = b->pos;
        SetScrollRange(hwnd, SB_CTL, (int)wParam, (int)lParam, message == SBM_SETRANGEREDRAW);
        return old;
    }
    case SBM_GETRANGE:
        if (wParam != 0) {
            *(int *)wParam = b->min;
        }
        if (lParam != 0) {
            *(int *)lParam = b->max;
        }
        return 0;
    case SBM_ENABLE_ARROWS:
        return EnableScrollBar(hwnd, SB_CTL, (UINT)wParam);
    case SBM_SETSCROLLINFO:
        return SetScrollInfo(hwnd, SB_CTL, (const SCROLLINFO *)lParam, wParam != 0);
    case SBM_GETSCROLLINFO:
        return GetScrollInfo(hwnd, SB_CTL, (LPSCROLLINFO)lParam);
    case WM_GETDLGCODE:
        return DLGC_WANTARROWS;
    case WM_KEYDOWN: {
        WPARAM code = SB_ENDSCROLL;
        const int is_vertical = (w->style & SBS_VERT) != 0;
        switch (wParam) {
        case VK_UP: code = is_vertical ? SB_LINEUP : SB_ENDSCROLL; break;
        case VK_LEFT: code = is_vertical ? SB_ENDSCROLL : SB_LINEUP; break;
        case VK_DOWN: code = is_vertical ? SB_LINEDOWN : SB_ENDSCROLL; break;
        case VK_RIGHT: code = is_vertical ? SB_ENDSCROLL : SB_LINEDOWN; break;
        case VK_PRIOR: code = SB_PAGEUP; break;
        case VK_NEXT: code = SB_PAGEDOWN; break;
        case VK_HOME: code = SB_TOP; break;
        case VK_END: code = SB_BOTTOM; break;
        default: break;
        }
        if (code != SB_ENDSCROLL && w->parent != 0) {
            send(w->parent, is_vertical ? WM_VSCROLL : WM_HSCROLL, code, (LPARAM)hwnd);
            send(w->parent, is_vertical ? WM_VSCROLL : WM_HSCROLL, SB_ENDSCROLL, (LPARAM)hwnd);
        }
        return 0;
    }
    default:
        break;
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

void register_scroll_bar(void) {
    static const WCHAR k_name[] = {'S', 'c', 'r', 'o', 'l', 'l', 'B', 'a', 'r', 0};
    register_system_class(CS_VREDRAW | CS_HREDRAW | CS_DBLCLKS | CS_PARENTDC, scroll_bar_proc, k_name,
                          ATOM_SCROLLBAR, NULL, 72);
}

/* ---- ScrollWindowEx ----------------------------------------------------------------------------- */

/* The visible part of a window's client area, in its client coordinates. */
static void client_visible(window *w, sxw_region *out) {
    HDC dc = window_dc(w, 0);
    sxw_surface *surface = SxwSurfaceOfDC(dc);
    (void)surface;
    DeleteObject(dc);
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
    if (!visible_chain(w)) {
        sxw_region_clear(out);
    }
    sxw_region_offset(out, -ox, -oy);
}

int WINAPI ScrollWindowEx(HWND hWnd, int dx, int dy, const RECT *prcScroll, const RECT *prcClip, HRGN hrgnUpdate,
                          LPRECT prcUpdate, UINT flags) {
    (void)hrgnUpdate;
    window *w = checked(hWnd);
    if (w == 0) {
        return ERROR_REGION;
    }
    sxw_rect client = {0, 0, client_width(w), client_height(w)};
    sxw_rect clip = client;
    if (prcClip != NULL) {
        sxw_rect c = {prcClip->left, prcClip->top, prcClip->right, prcClip->bottom};
        sxw_rect_intersect(&clip, &client, &c);
    }
    sxw_rect source = clip;
    if (prcScroll != NULL) {
        sxw_rect s = {prcScroll->left, prcScroll->top, prcScroll->right, prcScroll->bottom};
        sxw_rect_intersect(&source, &s, &clip);
    }
    sxw_rect moved = source;
    sxw_rect_offset(&moved, dx, dy);
    sxw_rect target;
    const int any_target = sxw_rect_intersect(&target, &moved, &clip);
    sxw_region update;
    sxw_region_clear(&update);
    if ((dx != 0 || dy != 0) && !sxw_rect_empty(&source)) {
        sxw_region_set(&update, &source);
        if (any_target) {
            sxw_region_subtract_rect(&update, &target);
        }
    }
    caret_hide_for_paint(w);
    /* The pixels move on the surface, inside the part of the window that
     * is visible; what had no visible source is left as it was. */
    if (any_target && (dx != 0 || dy != 0)) {
        sxw_region visible;
        client_visible(w, &visible);
        window *top = top_of(w);
        int ox;
        int oy;
        client_origin(w, &ox, &oy);
        const int width = target.right - target.left;
        const int height = target.bottom - target.top;
        unsigned int *copy = SxwHeapAllocate((u32)(width * height) * 4u, 0);
        if (copy != 0 && top->surface.pixels != 0) {
            for (int y = 0; y < height; ++y) {
                for (int x = 0; x < width; ++x) {
                    const int sx = target.left + x - dx;
                    const int sy = target.top + y - dy;
                    copy[y * width + x] = sxw_region_contains(&visible, sx, sy)
                                              ? top->surface.pixels[(oy + sy) * top->surface.stride + ox + sx]
                                              : 0xffffffffu;
                }
            }
            for (int y = 0; y < height; ++y) {
                for (int x = 0; x < width; ++x) {
                    const int tx = target.left + x;
                    const int ty = target.top + y;
                    const unsigned int p = copy[y * width + x];
                    if (p != 0xffffffffu && sxw_region_contains(&visible, tx, ty)) {
                        top->surface.pixels[(oy + ty) * top->surface.stride + ox + tx] = p;
                    }
                }
            }
            sxw_rect dirty = target;
            sxw_rect_offset(&dirty, ox, oy);
            sxw_region_union_rect(&top->surface.dirty, &dirty);
        }
        if (copy != 0) {
            SxwHeapFree(copy);
        }
    }
    if ((flags & SW_SCROLLCHILDREN) != 0) {
        HWND list[MAX_WINDOWS];
        int n = 0;
        for (window *c = w->child; c != 0 && n < MAX_WINDOWS; c = c->next) {
            sxw_rect cr = c->rect;
            sxw_rect hit;
            if (prcScroll == NULL || sxw_rect_intersect(&hit, &cr, &source)) {
                list[n++] = handle_of(c);
            }
        }
        for (int i = 0; i < n; ++i) {
            window *c = window_of(list[i]);
            if (c != 0) {
                set_window_pos(c, NULL, c->rect.left + dx, c->rect.top + dy, 0, 0,
                               SWP_NOZORDER | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOREDRAW);
            }
        }
    }
    if ((flags & SW_INVALIDATE) != 0) {
        for (int i = 0; i < update.count; ++i) {
            invalidate(w, &update.rects[i], (flags & SW_ERASE) != 0, (flags & SW_SCROLLCHILDREN) != 0);
        }
    }
    if (prcUpdate != NULL) {
        const sxw_rect bounds = sxw_region_bounds(&update);
        prcUpdate->left = bounds.left;
        prcUpdate->top = bounds.top;
        prcUpdate->right = bounds.right;
        prcUpdate->bottom = bounds.bottom;
    }
    return update.count == 0 ? NULLREGION : update.count == 1 ? SIMPLEREGION : COMPLEXREGION;
}

BOOL WINAPI ScrollWindow(HWND hWnd, int XAmount, int YAmount, const RECT *lpRect, const RECT *lpClipRect) {
    return ScrollWindowEx(hWnd, XAmount, YAmount, lpRect, lpClipRect, NULL, NULL,
                          SW_INVALIDATE | SW_ERASE | (lpRect == NULL ? SW_SCROLLCHILDREN : 0)) != ERROR_REGION;
}
