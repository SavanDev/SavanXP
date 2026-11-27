/*
 * msftedit.c -- SSW's RichEdit control (WIN32_WORDPAD_ROADMAP.md, WP7):
 * msftedit.dll with the RICHEDIT50W class WordPad's document is made of,
 * written here instead of running Microsoft's RichEdit 4.1 (the plan's
 * "RichEdit is written here").
 *
 * WP7a is the text: its messages, lines broken with gdi32's widths,
 * painting, the caret, selections by keyboard and pointer, scrolling, undo
 * and redo, the EN_ notifications EM_SETEVENTMASK asks for, and copy and
 * paste of text through user32's clipboard. Every answer, and the order of
 * the notifications, is what Windows XP's msftedit.dll gave
 * fixtures/richedit (docs/userland/WIN32.md, "RichEdit"). Formats are WP7b,
 * streams and RTF WP7c.
 */
#include "msftedit_private.h"

typedef struct sxw_guid {
    DWORD data1;
    WORD data2;
    WORD data3;
    BYTE data4[8];
} sxw_guid;

const sxw_guid IID_IRichEditOle = {0x00020d00, 0, 0, {0xc0, 0, 0, 0, 0, 0, 0, 0x46}};
const sxw_guid IID_IRichEditOleCallback = {0x00020d03, 0, 0, {0xc0, 0, 0, 0, 0, 0, 0, 0x46}};
const sxw_guid IID_ITextServices = {0x8d33f740, 0xcf58, 0x11ce, {0xa8, 0x9d, 0x00, 0xaa, 0x00, 0x6c, 0xad, 0xc5}};
const sxw_guid IID_ITextHost = {0xc5bdd8d0, 0xd26e, 0x11ce, {0xa8, 0x9e, 0x00, 0xaa, 0x00, 0x6c, 0xad, 0xc5}};
const sxw_guid IID_ITextHost2 = {0x13e670f5, 0x1a5a, 0x11cf, {0xab, 0xeb, 0x00, 0xaa, 0x00, 0xb6, 0x5e, 0xa1}};

/* The style bits EM_SETOPTIONS keeps in step with the options. */
#define OPTION_STYLES                                                                                                 \
    (ECO_AUTOVSCROLL | ECO_AUTOHSCROLL | ECO_NOHIDESEL | ECO_READONLY | ECO_WANTRETURN | ECO_SAVESEL |               \
     ECO_SELECTIONBAR | 0x00400000)
#define TEXT_MODE (TM_RICHTEXT | TM_MULTILEVELUNDO | TM_MULTICODEPAGE)
#define DEFAULT_LANG_OPTIONS 0x82 /* IMF_AUTOFONT | IMF_DUALFONT */
#define MK_SHIFT 0x0004
#define MK_CONTROL 0x0008
#define WHEEL_DELTA 120
#define WM_MOUSEWHEEL 0x020a
#define WM_UNDO 0x0304
#define WM_CLEAR 0x0303
/* SM_CXDRAG: how far the pointer goes before a press on the selection drags it. */
#define DRAG_DISTANCE 4
enum { DRAG_NONE, DRAG_SELECTING, DRAG_PENDING, DRAG_MOVING };

static rich *rich_of(HWND h) {
    return (rich *)GetWindowLongW(h, 0);
}

static int sel_min(const rich *r) {
    return r->anchor < r->caret ? r->anchor : r->caret;
}

static int sel_max(const rich *r) {
    return r->anchor < r->caret ? r->caret : r->anchor;
}

/* The selection's end within the text: the final paragraph mark selected
 * adds nothing to copy or replace. */
static int sel_text_max(const rich *r) {
    const int m = sel_max(r);
    return m > r->length ? r->length : m;
}

static int is_read_only(const rich *r) {
    return (r->style & ES_READONLY) != 0;
}

static int view_height(const rich *r) {
    RECT f;
    rich_format_rect(r, &f);
    return f.bottom - f.top;
}

static int max_scroll(const rich *r) {
    const int m = r->content_height - view_height(r);
    return m > 0 ? m : 0;
}

/* ---- Notifications ----------------------------------------------------------------------- */

static void command(rich *r, WORD code) {
    DWORD needed = 0;
    switch (code) {
    case EN_CHANGE: needed = ENM_CHANGE; break;
    case EN_VSCROLL:
    case EN_HSCROLL: needed = ENM_SCROLL; break;
    default: break; /* EN_UPDATE, EN_SETFOCUS, EN_KILLFOCUS and EN_MAXTEXT go out always */
    }
    if (needed != 0 && (r->event_mask & needed) == 0) {
        return;
    }
    HWND parent = GetParent(r->hwnd);
    if (parent != NULL) {
        SendMessageW(parent, WM_COMMAND, MAKEWPARAM(GetDlgCtrlID(r->hwnd), code), (LPARAM)r->hwnd);
    }
}

static WORD selection_type(const rich *r) {
    const int n = sel_text_max(r) - sel_min(r);
    const int count = sel_max(r) - sel_min(r);
    if (count <= 0) {
        return SEL_EMPTY;
    }
    return (WORD)(n > 1 || count > 1 ? SEL_TEXT | SEL_MULTICHAR : SEL_TEXT);
}

static void selchange(rich *r) {
    if ((r->event_mask & ENM_SELCHANGE) == 0) {
        return;
    }
    HWND parent = GetParent(r->hwnd);
    if (parent == NULL) {
        return;
    }
    SELCHANGE s;
    s.nmhdr.hwndFrom = r->hwnd;
    s.nmhdr.idFrom = (UINT_PTR)GetDlgCtrlID(r->hwnd);
    s.nmhdr.code = EN_SELCHANGE;
    s.chrg.cpMin = sel_min(r);
    s.chrg.cpMax = sel_max(r);
    s.seltyp = selection_type(r);
    SendMessageW(parent, WM_NOTIFY, (WPARAM)s.nmhdr.idFrom, (LPARAM)&s);
}

/* ---- Drawing helpers --------------------------------------------------------------------- */

static void invalidate_all(rich *r) {
    InvalidateRect(r->hwnd, NULL, FALSE);
}

/* The lines that hold [a, b), on screen. */
static void invalidate_range(rich *r, int a, int b) {
    if (a > b) {
        const int t = a;
        a = b;
        b = t;
    }
    RECT f;
    rich_format_rect(r, &f);
    RECT client;
    GetClientRect(r->hwnd, &client);
    const int first = rich_line_of(r, a);
    const int last = rich_line_of(r, b);
    RECT area = {client.left, f.top + r->lines[first].y - r->scroll_y, client.right,
                 f.top + r->lines[last].y + r->line_height - r->scroll_y};
    InvalidateRect(r->hwnd, &area, FALSE);
}

/* The caret is shown while the selection is empty. user32 counts hides, so
 * the control shows and hides it only when that changes. */
static void place_caret(rich *r) {
    if (!r->focused || !r->caret_made) {
        return;
    }
    if (sel_min(r) != sel_max(r)) {
        if (r->caret_shown) {
            HideCaret(r->hwnd);
            r->caret_shown = 0;
        }
        return;
    }
    POINT p;
    rich_pos_of(r, r->caret, &p);
    SetCaretPos(p.x, p.y);
    if (!r->caret_shown) {
        ShowCaret(r->hwnd);
        r->caret_shown = 1;
    }
}

/* ---- Layout, the scroll bar and scrolling ---------------------------------------------- */

static void update_bar(rich *r) {
    if (!r->vbar_shown) {
        return;
    }
    SCROLLINFO si;
    si.cbSize = sizeof si;
    si.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
    si.nMin = 0;
    si.nMax = r->content_height;
    si.nPage = (UINT)view_height(r);
    si.nPos = r->scroll_y;
    si.nTrackPos = 0;
    SetScrollInfo(r->hwnd, SB_VERT, &si, TRUE);
}

/* Lays the text out again, showing or hiding the vertical scroll bar the
 * window asked for as the text needs it. With `update`, a bar that comes
 * or goes is painted at once (WM_SETTEXT and editing do; EM_SETOPTIONS does
 * not). */
static void relayout(rich *r, int update) {
    rich_layout(r);
    if (r->wants_vscroll && r->multiline) {
        const int needed = r->content_height > view_height(r);
        if (needed != r->vbar_shown) {
            r->vbar_shown = needed;
            r->in_bar_change = 1;
            if (needed) {
                const DWORD style = (DWORD)GetWindowLongW(r->hwnd, GWL_STYLE);
                if ((style & WS_VSCROLL) != 0) {
                    SetWindowLongW(r->hwnd, GWL_STYLE, (LONG)(style & ~(DWORD)WS_VSCROLL));
                }
                update_bar(r);
                ShowScrollBar(r->hwnd, SB_VERT, TRUE);
            } else {
                r->scroll_y = 0;
                SCROLLINFO si;
                si.cbSize = sizeof si;
                si.fMask = SIF_POS;
                si.nPos = 0;
                SetScrollInfo(r->hwnd, SB_VERT, &si, FALSE);
                ShowScrollBar(r->hwnd, SB_VERT, FALSE);
            }
            r->in_bar_change = 0;
            r->style = (DWORD)GetWindowLongW(r->hwnd, GWL_STYLE);
            rich_layout(r);
            invalidate_all(r);
            if (update) {
                UpdateWindow(r->hwnd);
            }
        }
    }
    if (r->scroll_y > max_scroll(r)) {
        r->scroll_y = max_scroll(r);
    }
    update_bar(r);
    place_caret(r);
}

/* Scrolls the view to y. EN_VSCROLL goes out when `notify`, and the view is
 * painted before the call returns, as Windows XP's control does. Returns
 * whether it moved. */
static int scroll_within(rich *r, int y, int notify, int limit) {
    if (y > limit) {
        y = limit;
    }
    if (y < 0) {
        y = 0;
    }
    if (y == r->scroll_y) {
        return 0;
    }
    r->scroll_y = y;
    update_bar(r);
    invalidate_all(r);
    place_caret(r);
    if (notify) {
        command(r, EN_VSCROLL);
    }
    UpdateWindow(r->hwnd);
    return 1;
}

static int scroll_to(rich *r, int y, int notify) {
    return scroll_within(r, y, notify, max_scroll(r));
}

static int first_visible(const rich *r) {
    int index = r->scroll_y / r->line_height;
    return index >= r->line_count ? r->line_count - 1 : index;
}

/* Brings the caret's line into view. Returns whether it scrolled. */
static int scroll_caret(rich *r) {
    if (!r->multiline) {
        return 0;
    }
    const line_info *l = &r->lines[rich_line_of(r, r->caret)];
    if (l->y < r->scroll_y) {
        return scroll_to(r, l->y, 1);
    }
    if (l->y + r->line_height > r->scroll_y + view_height(r)) {
        return scroll_to(r, l->y + r->line_height - view_height(r), 1);
    }
    return 0;
}

/* ---- The selection ----------------------------------------------------------------------- */

/* A selection a program asks for (EM_EXSETSEL, EM_SETSEL): a negative start
 * puts the caret at the end; a negative end runs to the final paragraph
 * mark, which a collapsed selection cannot sit after. */
static void normalize(const rich *r, LONG *min, LONG *max) {
    const LONG end = r->length;
    if (*min < 0) {
        *min = *max = end;
        return;
    }
    if (*max < 0 || *max > end + 1) {
        *max = end + 1;
    }
    if (*min > end + 1) {
        *min = end + 1;
    }
    if (*min == *max && *min > end) {
        *min = *max = end;
    }
}

/* Selects as a program or the user does: the old selection's highlight and
 * the new one's are each painted at once, the view follows the caret, and
 * the parent hears EN_SELCHANGE. */
static void select(rich *r, int anchor, int caret) {
    if (anchor == r->anchor && caret == r->caret) {
        return;
    }
    r->typing_open = 0;
    if (r->anchor != r->caret) {
        invalidate_range(r, r->anchor, r->caret);
        UpdateWindow(r->hwnd);
    }
    r->anchor = anchor;
    r->caret = caret;
    scroll_caret(r);
    if (anchor != caret) {
        invalidate_range(r, anchor, caret);
        UpdateWindow(r->hwnd);
    }
    place_caret(r);
    selchange(r);
}

/* Selects as the user does, by keyboard or pointer: what changes is painted
 * once, where a program's selection is painted twice. */
static void user_select_view(rich *r, int anchor, int caret, int follow) {
    if (anchor == r->anchor && caret == r->caret) {
        return;
    }
    r->typing_open = 0;
    const int painted = r->anchor != r->caret || anchor != caret;
    if (r->anchor != r->caret) {
        invalidate_range(r, r->anchor, r->caret);
    }
    r->anchor = anchor;
    r->caret = caret;
    if (follow) {
        scroll_caret(r);
    }
    if (anchor != caret) {
        invalidate_range(r, anchor, caret);
    }
    if (painted) {
        UpdateWindow(r->hwnd);
    }
    place_caret(r);
    selchange(r);
}

static void user_select(rich *r, int anchor, int caret) {
    user_select_view(r, anchor, caret, 1);
}

/* ---- Changing the text ------------------------------------------------------------------- */

/* Replaces the selection with text already in the control's form, leaving
 * the caret after it. kind < 0 records no undo and forgets what there was. */
static void replace_selection(rich *r, const WCHAR *text, int n, int kind, int force_selchange) {
    const int min = sel_min(r);
    const int max = sel_text_max(r);
    if (kind >= 0) {
        if (kind == UNDO_TYPING && n == 1) {
            rich_undo_typing(r, min, r->text + min, max - min, text[0]);
        } else {
            rich_undo_push(r, kind, min, r->text + min, max - min, text, n);
        }
    } else {
        r->typing_open = 0;
        rich_undo_clear(&r->undo);
        rich_undo_clear(&r->redo);
    }
    rich_splice(r, min, max - min, text, n);
    r->modified = 1;
    const int old_anchor = r->anchor;
    const int old_caret = r->caret;
    r->anchor = r->caret = min + n;
    /* A bar that comes or goes is painted at once, and the changed text again
     * at the next WM_PAINT, as Windows XP paints an edit (WM_SETTEXT paints
     * only once). */
    invalidate_all(r);
    relayout(r, 1);
    invalidate_all(r);
    if ((r->style & ES_AUTOVSCROLL) != 0) {
        scroll_caret(r);
    }
    place_caret(r);
    if (force_selchange || old_anchor != r->anchor || old_caret != r->caret) {
        selchange(r);
    }
    command(r, EN_CHANGE);
}

static void set_text(rich *r, const WCHAR *in) {
    int n = 0;
    WCHAR *text = rich_import(in, -1, r->multiline, &n);
    if (text == 0) {
        return;
    }
    if (n == 0 && r->length == 0) {
        rich_free(text);
        return;
    }
    if (n > r->limit) {
        r->limit = n;
    }
    rich_undo_clear(&r->undo);
    rich_undo_clear(&r->redo);
    r->typing_open = 0;
    rich_splice(r, 0, r->length, text, n);
    rich_free(text);
    r->modified = 0;
    r->scroll_y = 0;
    const int moved = r->anchor != 0 || r->caret != 0;
    r->anchor = r->caret = 0;
    invalidate_all(r);
    relayout(r, 1);
    place_caret(r);
    if (moved) {
        selchange(r);
    }
    command(r, EN_CHANGE);
}

/* ---- The clipboard ----------------------------------------------------------------------- */

static void copy_selection(rich *r) {
    r->own_clipboard = 0;
    const int min = sel_min(r);
    const int max = sel_text_max(r);
    if (max <= min) {
        return;
    }
    const int n = max - min + rich_count_crs(r->text + min, max - min);
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, (SIZE_T)(n + 1) * sizeof(WCHAR));
    if (h == NULL) {
        return;
    }
    WCHAR *p = (WCHAR *)GlobalLock(h);
    rich_crlf_copy(r->text + min, max - min, p, n + 1);
    GlobalUnlock(h);
    if (!OpenClipboard(r->hwnd)) {
        GlobalFree(h);
        return;
    }
    EmptyClipboard();
    SetClipboardData(CF_UNICODETEXT, h);
    CloseClipboard();
    r->own_clipboard = GetClipboardSequenceNumber();
    r->clipboard_length = n;
}

static int can_paste(UINT format) {
    if (format == 0) {
        return IsClipboardFormatAvailable(CF_UNICODETEXT) || IsClipboardFormatAvailable(CF_TEXT);
    }
    return (format == CF_UNICODETEXT || format == CF_TEXT || format == CF_OEMTEXT) && IsClipboardFormatAvailable(format);
}

/* Pastes the clipboard's text over the selection. Its last paragraph mark is
 * dropped, as Windows XP's control drops it; what the limit leaves no room
 * for is not pasted, with EN_MAXTEXT. */
static void paste(rich *r) {
    if (is_read_only(r) || !OpenClipboard(r->hwnd)) {
        return;
    }
    HANDLE h = GetClipboardData(CF_UNICODETEXT);
    if (h == NULL) {
        CloseClipboard();
        return;
    }
    const WCHAR *data = (const WCHAR *)GlobalLock(h);
    int n = 0;
    WCHAR *text = data != 0 ? rich_import(data, -1, r->multiline, &n) : 0;
    GlobalUnlock(h);
    CloseClipboard();
    if (text == 0) {
        return;
    }
    /* Windows XP's control pastes what it copied itself as RTF, which keeps
     * the last paragraph mark; plain text loses it. Until the RTF of WP7c,
     * the clipboard's sequence number tells the two apart. */
    if (n > 0 && text[n - 1] == '\r' && GetClipboardSequenceNumber() != r->own_clipboard) {
        --n;
    }
    const int room = r->limit - (r->length - (sel_text_max(r) - sel_min(r)));
    if (n > room) {
        n = room > 0 ? room : 0;
        command(r, EN_MAXTEXT);
        if (n == 0) {
            rich_free(text);
            return;
        }
    }
    replace_selection(r, text, n, UNDO_PASTE, 1);
    rich_free(text);
}

/* ---- Text out ---------------------------------------------------------------------------- */

static int text_length_ex(const rich *r, const GETTEXTLENGTHEX *g) {
    if (g == 0) {
        return (int)0x80070057;
    }
    const DWORD flags = g->flags;
    if ((flags & GTL_NUMCHARS) != 0 && (flags & GTL_NUMBYTES) != 0) {
        return (int)0x80070057; /* E_INVALIDARG */
    }
    const int length = (flags & GTL_USECRLF) != 0 ? r->length + rich_count_crs(r->text, r->length) : r->length;
    if ((flags & GTL_NUMCHARS) != 0) {
        return length;
    }
    if ((flags & GTL_NUMBYTES) != 0) {
        if ((flags & GTL_PRECISE) == 0 || g->codepage == 1200) {
            return length * 2;
        }
        WCHAR *wide = rich_alloc((u32)(length + 1) * sizeof(WCHAR));
        if (wide == 0) {
            return length * 2;
        }
        if ((flags & GTL_USECRLF) != 0) {
            rich_crlf_copy(r->text, r->length, wide, length + 1);
        } else {
            for (int i = 0; i < length; ++i) {
                wide[i] = r->text[i];
            }
        }
        const int bytes = length == 0 ? 0 : WideCharToMultiByte(g->codepage, 0, wide, length, NULL, 0, NULL, NULL);
        rich_free(wide);
        return bytes;
    }
    if (flags == 0 || (flags & GTL_PRECISE) != 0) {
        return length;
    }
    return length * 2;
}

/* EM_GETTEXTEX: the text or the selection, in UTF-16 or a code page, into
 * cb bytes with a terminator. */
static int get_text_ex(const rich *r, const GETTEXTEX *g, void *out) {
    if (g == 0 || out == 0 || g->cb == 0) {
        return 0;
    }
    int a = 0;
    int b = r->length;
    if ((g->flags & GT_SELECTION) != 0) {
        a = sel_min(r);
        b = sel_text_max(r);
    }
    const int crlf = (g->flags & GT_USECRLF) != 0;
    if (g->codepage == 1200) {
        const int capacity = (int)(g->cb / sizeof(WCHAR));
        if (capacity == 0) {
            return 0;
        }
        WCHAR *w = out;
        if (crlf) {
            return rich_crlf_copy(r->text + a, b - a, w, capacity);
        }
        int k = 0;
        for (int i = a; i < b && k < capacity - 1; ++i) {
            w[k++] = r->text[i];
        }
        w[k] = 0;
        return k;
    }
    const int length = b - a + (crlf ? rich_count_crs(r->text + a, b - a) : 0);
    WCHAR *wide = rich_alloc((u32)(length + 1) * sizeof(WCHAR));
    if (wide == 0) {
        return 0;
    }
    if (crlf) {
        rich_crlf_copy(r->text + a, b - a, wide, length + 1);
    } else {
        for (int i = 0; i < length; ++i) {
            wide[i] = r->text[a + i];
        }
    }
    int bytes = length == 0 ? 0
                            : WideCharToMultiByte(g->codepage, 0, wide, length, out, (int)g->cb - 1, g->lpDefaultChar,
                                                  g->lpUsedDefChar);
    rich_free(wide);
    ((char *)out)[bytes] = 0;
    return bytes;
}

static int get_line(const rich *r, int index, WCHAR *out) {
    if (index < 0 || index >= r->line_count || out == 0) {
        return 0;
    }
    const int size = *(const WORD *)out;
    const line_info *l = &r->lines[index];
    int k = 0;
    for (int i = 0; i < l->length && k < size; ++i) {
        out[k++] = r->text[l->start + i];
    }
    if (l->paragraph_end && k < size) {
        out[k++] = '\r';
    }
    return k;
}

static int line_index(const rich *r, int index) {
    if (index == -1) {
        return r->lines[rich_line_of(r, sel_min(r))].start;
    }
    if (index < 0 || index >= r->line_count) {
        return -1;
    }
    return r->lines[index].start;
}

static int line_length(const rich *r, int cp) {
    if (cp >= 0 || sel_min(r) == sel_max(r)) {
        return r->lines[rich_line_of(r, cp >= 0 ? cp : sel_min(r))].length;
    }
    const line_info *first = &r->lines[rich_line_of(r, sel_min(r))];
    const int end = sel_max(r) > r->length ? r->length : sel_max(r);
    const line_info *last = &r->lines[rich_line_of(r, end)];
    const int after = last->start + last->length - end;
    return sel_min(r) - first->start + (after > 0 ? after : 0);
}

/* ---- Painting ---------------------------------------------------------------------------- */

/* Draws [a, b) of line l from x, with tabs moved to their stops. Returns
 * where it ends. */
static int draw_run(rich *r, HDC dc, const line_info *l, int a, int b, int left, int top) {
    int i = a;
    while (i < b) {
        int j = i;
        while (j < b && r->text[l->start + j] != '\t') {
            ++j;
        }
        const int x = left + rich_text_x(r, l, i);
        if (j > i) {
            ExtTextOutW(dc, x, top, 0, NULL, r->text + l->start + i, (UINT)(j - i), NULL);
        }
        if (j < b) {
            /* A tab: its space is filled with the background. */
            const int x0 = left + rich_text_x(r, l, j);
            const int x1 = left + rich_text_x(r, l, j + 1);
            RECT gap = {x0, top, x1, top + r->line_height};
            ExtTextOutW(dc, x0, top, ETO_OPAQUE, &gap, NULL, 0, NULL);
            ++j;
        }
        i = j;
    }
    return left + rich_text_x(r, l, b);
}

static void paint(rich *r) {
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(r->hwnd, &ps);
    command(r, EN_UPDATE);
    RECT client;
    GetClientRect(r->hwnd, &client);
    FillRect(dc, &client, GetSysColorBrush(COLOR_WINDOW));
    RECT f;
    rich_format_rect(r, &f);
    HGDIOBJ old = SelectObject(dc, r->font != NULL ? (HGDIOBJ)r->font : GetStockObject(SYSTEM_FONT));
    SetBkMode(dc, OPAQUE);
    const int show_selection =
        !r->hidden_selection && (r->focused || (r->options & ECO_NOHIDESEL) != 0) && sel_min(r) != sel_max(r);
    const int s0 = sel_min(r);
    const int s1 = sel_max(r);
    const int left = rich_left(r);
    for (int i = 0; i < r->line_count; ++i) {
        const line_info *l = &r->lines[i];
        const int top = f.top + l->y - r->scroll_y;
        if (top + r->line_height <= ps.rcPaint.top || top >= ps.rcPaint.bottom) {
            continue;
        }
        int a = 0;
        int b = l->length;
        if (show_selection) {
            a = s0 - l->start;
            b = s1 - l->start;
            a = a < 0 ? 0 : a > l->length ? l->length : a;
            b = b < 0 ? 0 : b > l->length ? l->length : b;
        }
        SetTextColor(dc, GetSysColor(COLOR_WINDOWTEXT));
        SetBkColor(dc, GetSysColor(COLOR_WINDOW));
        if (!show_selection || a >= b) {
            draw_run(r, dc, l, 0, l->length, left, top);
            continue;
        }
        draw_run(r, dc, l, 0, a, left, top);
        SetTextColor(dc, GetSysColor(COLOR_HIGHLIGHTTEXT));
        SetBkColor(dc, GetSysColor(COLOR_HIGHLIGHT));
        draw_run(r, dc, l, a, b, left, top);
        SetTextColor(dc, GetSysColor(COLOR_WINDOWTEXT));
        SetBkColor(dc, GetSysColor(COLOR_WINDOW));
        draw_run(r, dc, l, b, l->length, left, top);
    }
    /* A selected paragraph mark shows as a highlighted space after its line. */
    if (show_selection) {
        for (int i = 0; i < r->line_count; ++i) {
            const line_info *l = &r->lines[i];
            const int mark = l->start + l->length;
            if (!l->paragraph_end || mark < s0 || mark >= s1) {
                continue;
            }
            const int x = left + rich_text_x(r, l, l->length);
            const int top = f.top + l->y - r->scroll_y;
            RECT box = {x, top, x + r->widths[' '], top + r->line_height};
            FillRect(dc, &box, GetSysColorBrush(COLOR_HIGHLIGHT));
        }
    }
    SelectObject(dc, old);
    EndPaint(r->hwnd, &ps);
}

/* ---- Keyboard and pointer --------------------------------------------------------------- */

static int key_down(int vk) {
    return (GetKeyState(vk) & 0x8000) != 0;
}

/* Moves the caret, extending the selection with Shift. */
static void move_to(rich *r, int cp, int extend) {
    if (cp < 0) {
        cp = 0;
    }
    if (cp > r->length) {
        cp = r->length;
    }
    user_select(r, extend ? r->anchor : cp, cp);
}

static int line_x_target(rich *r, int cp) {
    POINT p;
    rich_pos_of(r, cp, &p);
    return p.x;
}

/* The position nearest x on line `index`: a wrapped line's last position is
 * before its last character, since its end is the next line's start. */
static int cp_in_line(rich *r, int index, int x) {
    RECT f;
    rich_format_rect(r, &f);
    const line_info *l = &r->lines[index];
    const int cp = rich_char_at(r, x, f.top + l->y - r->scroll_y);
    if (!l->paragraph_end && l->length > 0 && cp >= l->start + l->length) {
        return l->start + l->length - 1;
    }
    return cp;
}

/* Deletes [a, b) by key: Backspace (`backward`) or Delete. */
static void delete_range(rich *r, int a, int b, int backward) {
    if (b > r->length) {
        b = r->length;
    }
    if (a >= b) {
        return;
    }
    r->anchor = a;
    r->caret = b;
    replace_selection(r, 0, 0, backward ? UNDO_BACKSPACE : UNDO_DELETE, 1);
}

static void handle_key(rich *r, int vk) {
    const int shift = key_down(VK_SHIFT);
    const int control = key_down(VK_CONTROL);
    const int line = rich_line_of(r, r->caret);
    switch (vk) {
    case VK_LEFT:
        if (!shift && sel_min(r) != sel_max(r)) {
            move_to(r, sel_min(r), 0);
        } else {
            move_to(r, control ? rich_find_word_break(r, WB_MOVEWORDLEFT, r->caret) : r->caret - 1, shift);
        }
        break;
    case VK_RIGHT:
        if (!shift && sel_min(r) != sel_max(r)) {
            move_to(r, sel_text_max(r), 0);
        } else {
            move_to(r, control ? rich_find_word_break(r, WB_MOVEWORDRIGHT, r->caret) : r->caret + 1, shift);
        }
        break;
    case VK_UP:
    case VK_DOWN: {
        const int target = vk == VK_UP ? line - 1 : line + 1;
        if (target < 0 || target >= r->line_count) {
            move_to(r, vk == VK_UP ? (control ? 0 : r->caret) : (control ? r->length : r->caret), shift);
            break;
        }
        move_to(r, cp_in_line(r, target, line_x_target(r, r->caret)), shift);
        break;
    }
    case VK_HOME:
        move_to(r, control ? 0 : r->lines[line].start, shift);
        break;
    case VK_END: {
        const line_info *l = &r->lines[line];
        int end = l->start + l->length;
        if (!l->paragraph_end && end > l->start && line + 1 < r->line_count) {
            end = r->lines[line + 1].start; /* after the trailing spaces of a wrapped line */
        } else if (l->paragraph_end && shift && !control) {
            ++end; /* Shift+End selects the paragraph mark too */
        }
        if (control) {
            end = r->length;
        }
        if (shift) {
            user_select(r, r->anchor, end);
        } else {
            move_to(r, end, 0);
        }
        break;
    }
    case VK_PRIOR:
    case VK_NEXT: {
        int page = view_height(r) - r->line_height;
        if (page < r->line_height) {
            page = r->line_height;
        }
        const int x = line_x_target(r, r->caret);
        const int before = r->scroll_y;
        int limit = r->content_height - r->line_height;
        if (limit < max_scroll(r)) {
            limit = max_scroll(r);
        }
        scroll_within(r, before + (vk == VK_PRIOR ? -page : page), 1, limit);
        int moved = (r->scroll_y - before) / r->line_height;
        if (moved == 0) {
            moved = vk == VK_PRIOR ? -r->line_count : r->line_count;
        }
        int target = line + moved;
        target = target < 0 ? 0 : target >= r->line_count ? r->line_count - 1 : target;
        move_to(r, cp_in_line(r, target, x), shift);
        break;
    }
    case VK_BACK:
        if (is_read_only(r)) {
            break;
        }
        if (sel_min(r) != sel_max(r)) {
            invalidate_range(r, r->anchor, r->caret);
            UpdateWindow(r->hwnd);
            replace_selection(r, 0, 0, UNDO_DELETE, 1);
        } else if (r->caret > 0) {
            delete_range(r, control ? rich_find_word_break(r, WB_MOVEWORDLEFT, r->caret) : r->caret - 1, r->caret, 1);
        }
        break;
    case VK_DELETE:
        if (is_read_only(r)) {
            break;
        }
        if (shift && !control) {
            SendMessageW(r->hwnd, WM_CUT, 0, 0);
        } else if (sel_min(r) != sel_max(r)) {
            invalidate_range(r, r->anchor, r->caret);
            UpdateWindow(r->hwnd);
            replace_selection(r, 0, 0, UNDO_DELETE, 1);
        } else {
            delete_range(r, r->caret, control ? rich_find_word_break(r, WB_MOVEWORDRIGHT, r->caret) : r->caret + 1,
                         0);
        }
        break;
    case VK_INSERT:
        if (control) {
            SendMessageW(r->hwnd, WM_COPY, 0, 0);
        } else if (shift) {
            SendMessageW(r->hwnd, WM_PASTE, 0, 0);
        }
        break;
    case VK_RETURN:
        if (r->multiline && !is_read_only(r)) {
            if (r->length - (sel_text_max(r) - sel_min(r)) + 1 > r->limit) {
                command(r, EN_MAXTEXT);
                break;
            }
            const WCHAR cr = '\r';
            replace_selection(r, &cr, 1, UNDO_TYPING, 0);
        }
        break;
    default:
        if (control && !shift) {
            switch (vk) {
            case 'A': user_select_view(r, 0, r->length + 1, 0); break;
            case 'C': SendMessageW(r->hwnd, WM_COPY, 0, 0); break;
            case 'V': SendMessageW(r->hwnd, WM_PASTE, 0, 0); break;
            case 'X': SendMessageW(r->hwnd, WM_CUT, 0, 0); break;
            case 'Z': SendMessageW(r->hwnd, EM_UNDO, 0, 0); break;
            case 'Y': SendMessageW(r->hwnd, EM_REDO, 0, 0); break;
            default: break;
            }
        }
        break;
    }
}

static void type_char(rich *r, WCHAR c) {
    if (c < 0x20 && c != '\t') {
        return;
    }
    if (c == 0x7f || is_read_only(r)) {
        return;
    }
    if (key_down(VK_CONTROL) && !key_down(VK_MENU)) {
        return;
    }
    if (r->length - (sel_text_max(r) - sel_min(r)) + 1 > r->limit) {
        command(r, EN_MAXTEXT);
        return;
    }
    replace_selection(r, &c, 1, UNDO_TYPING, 0);
}

/* The word around cp, as a double click selects it: the word and the spaces
 * after it. */
static void select_word(rich *r, int cp) {
    const int start = rich_find_word_break(r, WB_MOVEWORDLEFT, cp + 1 > r->length ? r->length : cp + 1);
    int end = rich_find_word_break(r, WB_MOVEWORDRIGHT, start);
    if (end > r->length) {
        end = r->length;
    }
    user_select(r, start, end);
}

/* Where a drag selecting text has its end: past the end of a line that ends
 * a paragraph, after the paragraph mark. */
static int drag_point(rich *r, int x, int y) {
    const int cp = rich_char_at(r, x, y);
    const line_info *l = &r->lines[rich_line_of(r, cp)];
    if (l->paragraph_end && cp == l->start + l->length && cp < r->length &&
        x >= rich_left(r) + rich_text_x(r, l, l->length)) {
        return cp + 1;
    }
    return cp;
}

static void mouse_down(rich *r, int x, int y, WPARAM keys) {
    if (GetFocus() != r->hwnd) {
        SetFocus(r->hwnd);
    }
    const int cp = rich_char_at(r, x, y);
    SetCapture(r->hwnd);
    r->down_x = x;
    r->down_y = y;
    if ((keys & MK_SHIFT) == 0 && sel_min(r) != sel_max(r) && cp >= sel_min(r) && cp < sel_text_max(r) &&
        (r->options & ECO_READONLY) == 0 && (r->style & ES_NOOLEDRAGDROP) == 0) {
        r->dragging = DRAG_PENDING;
        return;
    }
    user_select(r, (keys & MK_SHIFT) != 0 ? r->anchor : cp, cp);
    r->dragging = DRAG_SELECTING;
}

/* Moves the selection to `to`, as Windows XP's control moves text dropped
 * inside itself: one undo, the moved text selected where it lands. */
static void move_selection(rich *r, int to) {
    const int a = sel_min(r);
    const int b = sel_text_max(r);
    if (to >= a && to <= b) {
        return;
    }
    const int n = b - a;
    WCHAR *moved = rich_alloc((u32)(n + 1) * sizeof(WCHAR));
    if (moved == 0) {
        return;
    }
    for (int i = 0; i < n; ++i) {
        moved[i] = r->text[a + i];
    }
    r->undo_group = ++r->undo_groups;
    int start;
    if (to > b) {
        rich_undo_push(r, UNDO_DRAGDROP, to, 0, 0, moved, n);
        rich_splice(r, to, 0, moved, n);
        rich_undo_push(r, UNDO_DRAGDROP, a, moved, n, 0, 0);
        rich_splice(r, a, n, 0, 0);
        start = to - n;
    } else {
        rich_undo_push(r, UNDO_DRAGDROP, a, moved, n, 0, 0);
        rich_splice(r, a, n, 0, 0);
        rich_undo_push(r, UNDO_DRAGDROP, to, 0, 0, moved, n);
        rich_splice(r, to, 0, moved, n);
        start = to;
    }
    r->undo_group = 0;
    rich_free(moved);
    r->modified = 1;
    invalidate_all(r);
    relayout(r, 1);
    select(r, start, start + n);
    command(r, EN_CHANGE);
}

static void mouse_move(rich *r, int x, int y) {
    if (r->dragging == DRAG_PENDING) {
        const int dx = x - r->down_x;
        const int dy = y - r->down_y;
        if (dx > DRAG_DISTANCE || dx < -DRAG_DISTANCE || dy > DRAG_DISTANCE || dy < -DRAG_DISTANCE) {
            r->dragging = DRAG_MOVING;
        }
        return;
    }
    if (r->dragging != DRAG_SELECTING) {
        return;
    }
    RECT f;
    rich_format_rect(r, &f);
    if (y < f.top) {
        scroll_to(r, r->scroll_y - r->line_height, 1);
    } else if (y >= f.bottom) {
        scroll_to(r, r->scroll_y + r->line_height, 1);
    }
    user_select(r, r->anchor, drag_point(r, x, y));
}

static void mouse_up(rich *r, int x, int y) {
    const int dragging = r->dragging;
    r->dragging = 0;
    if (GetCapture() == r->hwnd) {
        ReleaseCapture();
    }
    if (dragging == DRAG_MOVING) {
        move_selection(r, rich_char_at(r, x, y));
    } else if (dragging == DRAG_PENDING) {
        const int cp = rich_char_at(r, x, y);
        user_select(r, cp, cp);
    }
}

/* ---- The window -------------------------------------------------------------------------- */

static rich *create(HWND h, const CREATESTRUCTW *cs) {
    rich *r = rich_alloc(sizeof(rich));
    if (r == 0) {
        return 0;
    }
    r->hwnd = h;
    r->style = cs->style;
    r->multiline = (cs->style & ES_MULTILINE) != 0;
    r->wants_vscroll = (cs->style & WS_VSCROLL) != 0;
    r->limit = DEFAULT_LIMIT;
    r->undo_limit = DEFAULT_UNDO_LIMIT;
    r->lang_options = DEFAULT_LANG_OPTIONS;
    r->options = cs->style & OPTION_STYLES;
    r->insets.left = DEFAULT_INSET;
    r->insets.right = DEFAULT_INSET;
    r->no_wrap = !r->multiline;
    rich_reserve(r, 0);
    /* The scroll bars come when the text needs them. */
    r->style &= ~(DWORD)(WS_VSCROLL | WS_HSCROLL);
    SetWindowLongW(h, GWL_STYLE, (LONG)r->style);
    SetWindowLongW(h, 0, (LONG)r);
    rich_measure_font(r);
    return r;
}

static void destroy(rich *r) {
    SetWindowLongW(r->hwnd, 0, 0);
    rich_undo_clear(&r->undo);
    rich_undo_clear(&r->redo);
    rich_free(r->undo.records);
    rich_free(r->redo.records);
    rich_free(r->lines);
    rich_free(r->text);
    if (r->font != NULL) {
        DeleteObject(r->font);
    }
    rich_free(r);
}

static void set_font(rich *r, HFONT font, int redraw) {
    LOGFONTW lf;
    HFONT copy = NULL;
    if (font != NULL && GetObjectW(font, sizeof lf, &lf) != 0) {
        copy = CreateFontIndirectW(&lf);
    }
    if (r->font != NULL) {
        DeleteObject(r->font);
    }
    r->font = copy;
    rich_measure_font(r);
    invalidate_all(r);
    relayout(r, 0);
    (void)redraw;
}

/* EM_SETOPTIONS, and EM_SETREADONLY as `style_only`: the first writes the
 * window's style back from what it was made with, scroll bar included,
 * without recalculating the frame; the second changes only its own bit. */
static LRESULT set_options(rich *r, WPARAM op, LPARAM value, int style_only) {
    const DWORD before = r->options;
    switch (op) {
    case ECOOP_SET: r->options = (DWORD)value; break;
    case ECOOP_OR: r->options |= (DWORD)value; break;
    case ECOOP_AND: r->options &= (DWORD)value; break;
    case ECOOP_XOR: r->options ^= (DWORD)value; break;
    default: return 0;
    }
    r->style = (DWORD)GetWindowLongW(r->hwnd, GWL_STYLE);
    r->style = (r->style & ~(DWORD)OPTION_STYLES) | (r->options & OPTION_STYLES);
    if (r->wants_vscroll && !style_only) {
        r->style |= WS_VSCROLL;
    }
    SetWindowLongW(r->hwnd, GWL_STYLE, (LONG)r->style);
    if (((before ^ r->options) & ECO_SELECTIONBAR) != 0) {
        relayout(r, 0);
        invalidate_all(r);
    }
    return (LRESULT)r->options;
}

static void set_rect(rich *r, const RECT *rc, int redraw) {
    if (rc == NULL) {
        r->insets.left = DEFAULT_INSET;
        r->insets.top = 0;
        r->insets.right = DEFAULT_INSET;
        r->insets.bottom = 0;
    } else {
        RECT client;
        GetClientRect(r->hwnd, &client);
        r->insets.left = rc->left - client.left;
        r->insets.top = rc->top - client.top;
        r->insets.right = client.right - rc->right;
        r->insets.bottom = client.bottom - rc->bottom;
        LONG *edges[4] = {&r->insets.left, &r->insets.top, &r->insets.right, &r->insets.bottom};
        for (int i = 0; i < 4; ++i) {
            if (*edges[i] < 0) {
                *edges[i] = 0;
            }
        }
    }
    if (redraw) {
        invalidate_all(r);
        relayout(r, 1);
    }
}

static LRESULT scroll_lines(rich *r, int code) {
    const int before = first_visible(r);
    const int page = view_height(r) - r->line_height;
    int y = r->scroll_y;
    switch (code) {
    case SB_LINEUP: y = (before - 1) * r->line_height; break;
    case SB_LINEDOWN: y = (before + 1) * r->line_height; break;
    case SB_PAGEUP: y -= page > r->line_height ? page : r->line_height; break;
    case SB_PAGEDOWN: y += page > r->line_height ? page : r->line_height; break;
    default: return 0;
    }
    scroll_to(r, y, 1);
    return MAKELONG((WORD)(SHORT)(first_visible(r) - before), TRUE);
}

static LRESULT vscroll(rich *r, int code, int pos) {
    switch (code) {
    case SB_TOP: scroll_to(r, 0, 1); break;
    case SB_BOTTOM: scroll_to(r, max_scroll(r), 1); break;
    case SB_THUMBPOSITION:
    case SB_THUMBTRACK: scroll_to(r, pos, 0); break;
    case SB_LINEUP:
    case SB_LINEDOWN:
    case SB_PAGEUP:
    case SB_PAGEDOWN: scroll_lines(r, code); break;
    default: break;
    }
    return 0;
}

LRESULT WINAPI RichEditWndProc(HWND h, UINT msg, WPARAM w, LPARAM l) {
    if (msg == WM_NCCREATE) {
        if (create(h, (const CREATESTRUCTW *)l) == 0) {
            return FALSE;
        }
        return DefWindowProcW(h, msg, w, l);
    }
    rich *r = rich_of(h);
    if (r == 0) {
        return DefWindowProcW(h, msg, w, l);
    }
    switch (msg) {
    case WM_CREATE: {
        const CREATESTRUCTW *cs = (const CREATESTRUCTW *)l;
        relayout(r, 0);
        if (cs->lpszName != NULL && cs->lpszName[0] != 0) {
            set_text(r, cs->lpszName);
        }
        return 0;
    }
    case WM_DESTROY:
        if (r->own_clipboard != 0 && GetClipboardSequenceNumber() == r->own_clipboard) {
            HWND parent = GetParent(h);
            if (parent != NULL) {
                ENSAVECLIPBOARD e;
                e.nmhdr.hwndFrom = h;
                e.nmhdr.idFrom = (UINT_PTR)GetDlgCtrlID(h);
                e.nmhdr.code = EN_SAVECLIPBOARD;
                e.cObjectCount = 0;
                e.cch = r->clipboard_length;
                SendMessageW(parent, WM_NOTIFY, (WPARAM)e.nmhdr.idFrom, (LPARAM)&e);
            }
        }
        return DefWindowProcW(h, msg, w, l);
    case WM_NCDESTROY:
        destroy(r);
        return DefWindowProcW(h, msg, w, l);
    case WM_SIZE:
        if (!r->in_bar_change) {
            relayout(r, 0);
            invalidate_all(r);
        }
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
        paint(r);
        return 0;
    case WM_SETFOCUS:
        r->focused = 1;
        CreateCaret(h, NULL, 1, r->line_height);
        r->caret_made = 1;
        r->caret_shown = 0; /* a new caret starts hidden */
        place_caret(r);
        if ((r->options & ECO_NOHIDESEL) == 0 && sel_min(r) != sel_max(r)) {
            invalidate_range(r, r->anchor, r->caret);
        }
        command(r, EN_SETFOCUS);
        return 0;
    case WM_KILLFOCUS:
        r->focused = 0;
        if (r->caret_made) {
            DestroyCaret();
            r->caret_made = 0;
        }
        if ((r->options & ECO_NOHIDESEL) == 0 && sel_min(r) != sel_max(r)) {
            invalidate_range(r, r->anchor, r->caret);
        }
        command(r, EN_KILLFOCUS);
        return 0;
    case WM_GETDLGCODE:
        return r->multiline ? DLGC_WANTCHARS | DLGC_WANTALLKEYS | DLGC_WANTTAB | DLGC_WANTARROWS
                            : DLGC_WANTCHARS | DLGC_HASSETSEL | DLGC_WANTTAB | DLGC_WANTARROWS;
    case WM_SETCURSOR:
        if (LOWORD(l) == HTCLIENT) {
            SetCursor(LoadCursorW(NULL, IDC_IBEAM));
            return TRUE;
        }
        return DefWindowProcW(h, msg, w, l);
    case WM_SETFONT:
        set_font(r, (HFONT)w, l != 0);
        return 0;
    case WM_GETFONT:
        return 0;

    case WM_SETTEXT:
        set_text(r, (LPCWSTR)l);
        return 1;
    case WM_GETTEXTLENGTH:
        return r->length + rich_count_crs(r->text, r->length);
    case WM_GETTEXT:
        if (l == 0 || w == 0) {
            return 0;
        }
        return rich_crlf_copy(r->text, r->length, (WCHAR *)l, (int)w);
    case EM_GETTEXTLENGTHEX:
        return text_length_ex(r, (const GETTEXTLENGTHEX *)w);
    case EM_GETTEXTEX:
        return get_text_ex(r, (const GETTEXTEX *)w, (void *)l);
    case EM_SETTEXTEX: {
        const SETTEXTEX *s = (const SETTEXTEX *)w;
        if (s != 0 && (s->flags & ST_SELECTION) != 0) {
            int n = 0;
            WCHAR *text = rich_import((LPCWSTR)l, -1, r->multiline, &n);
            if (text != 0) {
                replace_selection(r, text, n, (s->flags & ST_KEEPUNDO) != 0 ? UNDO_UNKNOWN : -1, 0);
                rich_free(text);
            }
            return n;
        }
        set_text(r, l != 0 ? (LPCWSTR)l : (LPCWSTR)L"");
        return 1;
    }
    case EM_GETTEXTRANGE: {
        TEXTRANGEW *t = (TEXTRANGEW *)l;
        if (t == 0 || t->lpstrText == 0) {
            return 0;
        }
        LONG a = t->chrg.cpMin;
        LONG b = t->chrg.cpMax;
        if (b < 0 || b > r->length) {
            b = r->length;
        }
        if (a < 0 || a >= b) {
            return 0;
        }
        for (LONG i = a; i < b; ++i) {
            t->lpstrText[i - a] = r->text[i];
        }
        t->lpstrText[b - a] = 0;
        return b - a;
    }
    case EM_GETSELTEXT: {
        WCHAR *out = (WCHAR *)l;
        if (out == 0) {
            return 0;
        }
        const int a = sel_min(r);
        const int b = sel_text_max(r);
        for (int i = a; i < b; ++i) {
            out[i - a] = r->text[i];
        }
        out[b > a ? b - a : 0] = 0;
        return b > a ? b - a : 0;
    }
    case EM_REPLACESEL: {
        int n = 0;
        WCHAR *text = rich_import((LPCWSTR)l, -1, r->multiline, &n);
        if (text == 0) {
            return 0;
        }
        replace_selection(r, text, n, w != 0 ? UNDO_UNKNOWN : -1, 0);
        rich_free(text);
        return n;
    }

    case EM_GETSEL:
        if (w != 0) {
            *(DWORD *)w = (DWORD)sel_min(r);
        }
        if (l != 0) {
            *(DWORD *)l = (DWORD)sel_max(r);
        }
        return MAKELONG(sel_min(r) > 0xffff ? 0xffff : sel_min(r), sel_max(r) > 0xffff ? 0xffff : sel_max(r));
    case EM_EXGETSEL: {
        CHARRANGE *c = (CHARRANGE *)l;
        if (c != 0) {
            c->cpMin = sel_min(r);
            c->cpMax = sel_max(r);
        }
        return 0;
    }
    case EM_SETSEL:
    case EM_EXSETSEL: {
        LONG a;
        LONG b;
        if (msg == EM_SETSEL) {
            a = (LONG)w;
            b = (LONG)l;
        } else {
            const CHARRANGE *c = (const CHARRANGE *)l;
            if (c == 0) {
                return sel_max(r);
            }
            a = c->cpMin;
            b = c->cpMax;
        }
        /* A range given backwards keeps its anchor at the higher end. */
        const int backwards = a >= 0 && b >= 0 && a > b;
        LONG lo = backwards ? b : a;
        LONG hi = backwards ? a : b;
        normalize(r, &lo, &hi);
        if (backwards && lo != hi) {
            select(r, (int)hi, (int)lo);
        } else {
            select(r, (int)lo, (int)hi);
        }
        return sel_max(r);
    }
    case EM_SELECTIONTYPE:
        return selection_type(r);
    case EM_HIDESELECTION:
        r->hidden_selection = w != 0;
        if (sel_min(r) != sel_max(r)) {
            invalidate_range(r, r->anchor, r->caret);
            UpdateWindow(h);
        }
        return 0;

    case EM_GETLINECOUNT:
        return r->line_count;
    case EM_LINEINDEX:
        return line_index(r, (int)w);
    case EM_LINELENGTH:
        return line_length(r, (int)w);
    case EM_GETLINE:
        return get_line(r, (int)w, (WCHAR *)l);
    case EM_LINEFROMCHAR:
        return rich_line_of(r, (int)w < 0 ? sel_min(r) : (int)w);
    case EM_EXLINEFROMCHAR:
        return rich_line_of(r, (int)l < 0 ? sel_min(r) : (int)l);
    case EM_GETFIRSTVISIBLELINE:
        return first_visible(r);
    case EM_POSFROMCHAR: {
        POINTL *p = (POINTL *)w;
        if (p != 0) {
            POINT q;
            rich_pos_of(r, (int)l, &q);
            p->x = q.x;
            p->y = q.y;
        }
        return 0;
    }
    case EM_CHARFROMPOS: {
        const POINTL *p = (const POINTL *)l;
        return p != 0 ? rich_char_at(r, p->x, p->y) : 0;
    }
    case EM_FINDWORDBREAK:
        return rich_find_word_break(r, (int)w, (int)l);

    case EM_GETRECT:
        if (l != 0) {
            rich_format_rect(r, (RECT *)l);
        }
        return 0;
    case EM_SETRECT:
    case EM_SETRECTNP:
        set_rect(r, (const RECT *)l, msg == EM_SETRECT);
        return 0;
    case EM_SETTARGETDEVICE:
        if (r->multiline) {
            r->no_wrap = l == 1;
            relayout(r, 0);
            invalidate_all(r);
        }
        return 1;

    case EM_LINESCROLL: {
        if (!r->multiline) {
            return 0;
        }
        const int target = first_visible(r) + (int)l;
        scroll_to(r, (target < 0 ? 0 : target) * r->line_height, 1);
        return 1;
    }
    case EM_SCROLL:
        return r->multiline ? scroll_lines(r, (int)w) : 0;
    case EM_SCROLLCARET:
        scroll_caret(r);
        return 0;
    case EM_GETSCROLLPOS: {
        POINT *p = (POINT *)l;
        if (p != 0) {
            p->x = r->scroll_x;
            p->y = r->scroll_y;
        }
        return 1;
    }
    case EM_SETSCROLLPOS: {
        const POINT *p = (const POINT *)l;
        if (p != 0) {
            scroll_to(r, p->y, 1);
        }
        return 1;
    }
    case WM_VSCROLL:
        return vscroll(r, LOWORD(w), (SHORT)HIWORD(w));
    case WM_MOUSEWHEEL: {
        const int delta = (SHORT)HIWORD(w);
        scroll_to(r, r->scroll_y - delta / WHEEL_DELTA * 3 * r->line_height, 1);
        return 0;
    }

    case EM_GETLIMITTEXT:
        return r->limit;
    case EM_LIMITTEXT:
        r->limit = w == 0 ? ZERO_LIMIT : (int)w;
        return 0;
    case EM_EXLIMITTEXT:
        r->limit = l == 0 ? ZERO_LIMIT : (int)l;
        return 0;
    case EM_GETMODIFY:
        return r->modified ? -1 : 0;
    case EM_SETMODIFY:
        r->modified = w != 0;
        return 0;
    case EM_CANUNDO:
        return r->undo.count > 0;
    case EM_CANREDO:
        return r->redo.count > 0;
    case WM_UNDO:
    case EM_UNDO:
    case EM_REDO: {
        const int old_anchor = r->anchor;
        const int old_caret = r->caret;
        if (!rich_undo(r, msg == EM_REDO)) {
            return 0;
        }
        r->modified = 1;
        invalidate_all(r);
        relayout(r, 1);
        scroll_caret(r);
        place_caret(r);
        if (old_anchor != r->anchor || old_caret != r->caret) {
            selchange(r);
        }
        command(r, EN_CHANGE);
        return 1;
    }
    case EM_GETUNDONAME:
        return r->undo.count > 0 ? UNDO_NAME(r->undo.records[r->undo.count - 1].kind) : 0;
    case EM_GETREDONAME:
        return r->redo.count > 0 ? UNDO_NAME(r->redo.records[r->redo.count - 1].kind) : 0;
    case EM_EMPTYUNDOBUFFER:
        rich_undo_clear(&r->undo);
        rich_undo_clear(&r->redo);
        r->typing_open = 0;
        return 0;
    case EM_SETUNDOLIMIT:
        r->undo_limit = (int)w;
        if (r->undo_limit <= 0) {
            rich_undo_clear(&r->undo);
            rich_undo_clear(&r->redo);
        }
        return r->undo_limit;
    case EM_STOPGROUPTYPING:
        r->typing_open = 0;
        return 0;

    case WM_COPY:
        copy_selection(r);
        return 0;
    case WM_CUT:
        if (is_read_only(r) || sel_text_max(r) <= sel_min(r)) {
            return 0;
        }
        copy_selection(r);
        invalidate_range(r, r->anchor, r->caret);
        UpdateWindow(h);
        replace_selection(r, 0, 0, UNDO_CUT, 0);
        return 0;
    case WM_CLEAR:
        if (!is_read_only(r) && sel_text_max(r) > sel_min(r)) {
            replace_selection(r, 0, 0, UNDO_DELETE, 0);
        }
        return 0;
    case WM_PASTE:
        paste(r);
        return 0;
    case EM_CANPASTE:
        return can_paste((UINT)w);

    case EM_GETEVENTMASK:
        return (LRESULT)r->event_mask;
    case EM_SETEVENTMASK: {
        const DWORD before = r->event_mask;
        r->event_mask = (DWORD)l;
        return (LRESULT)before;
    }
    case EM_GETOPTIONS:
        return (LRESULT)r->options;
    case EM_SETOPTIONS:
        return set_options(r, w, l, 0);
    case EM_SETREADONLY:
        set_options(r, w != 0 ? ECOOP_OR : ECOOP_AND, w != 0 ? ECO_READONLY : ~(LPARAM)ECO_READONLY, 1);
        return 1;
    case EM_GETTEXTMODE:
        return TEXT_MODE;
    case EM_SETTEXTMODE:
        return 0;
    case EM_GETEDITSTYLE:
        return (LRESULT)r->edit_style;
    case EM_SETEDITSTYLE:
        r->edit_style = (r->edit_style & ~(DWORD)l) | ((DWORD)w & (DWORD)l);
        return (LRESULT)r->edit_style;
    case EM_GETLANGOPTIONS:
        return (LRESULT)r->lang_options;
    case EM_SETLANGOPTIONS:
        r->lang_options = (DWORD)l;
        return 1;
    case EM_GETWORDWRAPMODE:
    case EM_GETAUTOURLDETECT:
        return 0;
    case EM_SETWORDWRAPMODE:
    case EM_AUTOURLDETECT:
        return 0;
    case EM_GETZOOM:
        if (w != 0) {
            *(DWORD *)w = 0;
        }
        if (l != 0) {
            *(DWORD *)l = 0;
        }
        return 1;
    case EM_SETZOOM:
        return 1;
    case EM_SETOLECALLBACK:
        r->ole_callback = (void *)l;
        return 1;
    case EM_GETOLEINTERFACE:
        if (l != 0) {
            *(void **)l = 0;
        }
        return 0;

    case WM_KEYDOWN:
        handle_key(r, (int)w);
        return 0;
    case WM_CHAR:
        type_char(r, (WCHAR)w);
        return 0;
    case WM_LBUTTONDOWN:
        mouse_down(r, (SHORT)LOWORD(l), (SHORT)HIWORD(l), w);
        return 0;
    case WM_LBUTTONDBLCLK:
        select_word(r, rich_char_at(r, (SHORT)LOWORD(l), (SHORT)HIWORD(l)));
        return 0;
    case WM_MOUSEMOVE:
        mouse_move(r, (SHORT)LOWORD(l), (SHORT)HIWORD(l));
        return 0;
    case WM_LBUTTONUP:
        mouse_up(r, (SHORT)LOWORD(l), (SHORT)HIWORD(l));
        return 0;
    case WM_CAPTURECHANGED:
        r->dragging = 0;
        return 0;
    default:
        return DefWindowProcW(h, msg, w, l);
    }
}

/* ---- The DLL ----------------------------------------------------------------------------- */

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved) {
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        WNDCLASSEXW wc;
        wc.cbSize = sizeof wc;
        wc.style = CS_GLOBALCLASS | CS_PARENTDC | CS_DBLCLKS;
        wc.lpfnWndProc = RichEditWndProc;
        wc.cbClsExtra = 0;
        wc.cbWndExtra = sizeof(void *);
        wc.hInstance = instance;
        wc.hIcon = NULL;
        wc.hCursor = NULL;
        wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
        wc.lpszMenuName = NULL;
        wc.lpszClassName = MSFTEDIT_CLASS;
        wc.hIconSm = NULL;
        if (RegisterClassExW(&wc) == 0) {
            return FALSE;
        }
    }
    return TRUE;
}
