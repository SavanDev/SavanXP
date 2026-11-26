/*
 * edit.c -- The Edit control, single-line (docs/userland/WIN32.md, "Edit,
 * list box and combo box"): typing and deleting, a selection by keyboard
 * and pointer, a caret, horizontal scrolling, the clipboard, one level of
 * undo, a limit, and the EM_ messages and EN_ notifications Windows XP
 * answers and sends (fixtures/controls). A multi-line edit control is not
 * written: no program of the profile makes one outside RichEdit.
 *
 * Inside a combo box the control tells the combo box, which passes it on as
 * CBN_EDITUPDATE and CBN_EDITCHANGE.
 */
#include "user32_private.h"

#define ATOM_EDIT 0x0081
#define DEFAULT_LIMIT 30000
#define NO_LIMIT 0x7ffffffe
#define EM_GETTHUMB 0x00be
#define EM_SETRECT 0x00b3
#define EM_SETRECTNP 0x00b4
#define EM_SCROLL 0x00b5
#define EM_LINESCROLL 0x00b6
#define EM_SETTABSTOPS 0x00cb
#define EM_SETWORDBREAKPROC 0x00d0
#define EM_GETWORDBREAKPROC 0x00d1

typedef struct edit {
    WCHAR *text;
    int length;
    int capacity;
    int limit;
    int anchor; /* the selection runs from anchor to caret */
    int caret;
    int first;  /* the first character shown */
    int modified;
    WCHAR *undo;
    int undo_length;
    int undo_anchor;
    int undo_caret;
    int can_undo;
    WCHAR password;
    int left_margin;
    int right_margin;
    int focused;
    int dragging;
    int border; /* WS_BORDER, taken out of the style and drawn inside */
} edit;

static edit *edit_of(window *w) {
    return (edit *)w->control;
}

static int low(const edit *e) {
    return e->anchor < e->caret ? e->anchor : e->caret;
}

static int high(const edit *e) {
    return e->anchor < e->caret ? e->caret : e->anchor;
}

static void notify(window *w, WORD code) {
    if (w->parent != 0) {
        send(w->parent, WM_COMMAND, MAKELPARAM(w->id, code), (LPARAM)handle_of(w));
    }
}

static int reserve(edit *e, int length) {
    if (length + 1 <= e->capacity) {
        return 1;
    }
    int capacity = e->capacity == 0 ? 32 : e->capacity;
    while (capacity < length + 1) {
        capacity *= 2;
    }
    WCHAR *grown = SxwHeapAllocate((u32)capacity * sizeof(WCHAR), 0);
    if (grown == 0) {
        return 0;
    }
    for (int i = 0; i < e->length; ++i) {
        grown[i] = e->text[i];
    }
    grown[e->length] = 0;
    if (e->text != 0) {
        SxwHeapFree(e->text);
    }
    e->text = grown;
    e->capacity = capacity;
    return 1;
}

/* ---- Geometry ---------------------------------------------------------------------------- */

static HDC measuring_dc(window *w) {
    HDC dc = GetDC(NULL);
    if (w->font != NULL) {
        SelectObject(dc, w->font);
    }
    return dc;
}

static int font_height(window *w) {
    HDC dc = measuring_dc(w);
    TEXTMETRICW tm;
    GetTextMetricsW(dc, &tm);
    ReleaseDC(NULL, dc);
    return tm.tmHeight;
}

/* What is drawn for the text: the password character in its place. */
static const WCHAR *shown(window *w, edit *e, WCHAR *scratch, int capacity) {
    if (e->password == 0 || (w->style & ES_PASSWORD) == 0) {
        return e->text;
    }
    const int n = e->length < capacity - 1 ? e->length : capacity - 1;
    for (int i = 0; i < n; ++i) {
        scratch[i] = e->password;
    }
    scratch[n] = 0;
    return scratch;
}

static int width_of(window *w, edit *e, int from, int to) {
    if (to <= from) {
        return 0;
    }
    static WCHAR scratch[1024];
    const WCHAR *t = shown(w, e, scratch, 1024);
    HDC dc = measuring_dc(w);
    SIZE size;
    GetTextExtentPoint32W(dc, t + from, to - from, &size);
    ReleaseDC(NULL, dc);
    return size.cx;
}

/* The rectangle text is laid out in: the client area less the margins,
 * one line high, near the top as Windows XP's single-line edit has it. */
/* Where the text goes: a pixel in from the side and at most one down, or,
 * in a combo box (ES_COMBO), at the corner itself, as Windows XP places it
 * (fixtures/controls). */
static void format_rect(window *w, edit *e, RECT *r) {
    const int b = e->border ? 1 : 0;
    const int pad = (w->style & ES_COMBO) != 0 ? 0 : 1;
    r->left = b + e->left_margin + pad;
    r->right = client_width(w) - b - e->right_margin - pad;
    const int h = font_height(w);
    r->top = (client_height(w) - 2 * b - h) / 2;
    if (r->top < 0) {
        r->top = 0;
    }
    if (r->top > pad) {
        r->top = pad;
    }
    r->top += b;
    r->bottom = r->top + h;
}

static int text_left(window *w, edit *e) {
    RECT r;
    format_rect(w, e, &r);
    if ((w->style & ES_RIGHT) != 0 || (w->style & ES_CENTER) != 0) {
        const int width = width_of(w, e, 0, e->length);
        const int room = r.right - r.left;
        if (width < room) {
            return (w->style & ES_RIGHT) != 0 ? r.right - width : r.left + (room - width) / 2;
        }
    }
    return r.left;
}

static int x_of(window *w, edit *e, int index) {
    return text_left(w, e) + width_of(w, e, e->first, index) - (index < e->first ? width_of(w, e, index, e->first) : 0);
}

static int char_at(window *w, edit *e, int x) {
    int best = e->first;
    int at = text_left(w, e);
    if (x < at) {
        /* Left of the text: the pointer scrolls back a character. */
        return e->first > 0 ? e->first - 1 : 0;
    }
    for (int i = e->first; i < e->length; ++i) {
        const int next = at + width_of(w, e, i, i + 1);
        if (x < (at + next + 1) / 2) {
            return i;
        }
        at = next;
        best = i + 1;
    }
    return best;
}

/* Scrolls so the caret is in view, a third of the width at a time. */
static void scroll_to_caret(window *w, edit *e) {
    RECT r;
    format_rect(w, e, &r);
    const int room = r.right - r.left;
    if (e->caret < e->first) {
        e->first = e->caret;
        if ((w->style & ES_AUTOHSCROLL) != 0) {
            while (e->first > 0 && width_of(w, e, e->first - 1, e->caret) < room / 3) {
                --e->first;
            }
        }
    }
    while (e->first < e->caret && width_of(w, e, e->first, e->caret) > room) {
        const int target = e->first;
        int step = target;
        while (step < e->caret && width_of(w, e, target, step) < room / 3) {
            ++step;
        }
        e->first = step > target ? step : target + 1;
    }
}

static void place_caret(window *w, edit *e) {
    if (!e->focused) {
        return;
    }
    RECT r;
    format_rect(w, e, &r);
    SetCaretPos(x_of(w, e, e->caret), r.top);
}

/* ---- Drawing ------------------------------------------------------------------------------- */

static HBRUSH colors(window *w, HDC dc) {
    const int grayed = (w->style & (ES_READONLY | WS_DISABLED)) != 0;
    const UINT message = grayed ? WM_CTLCOLORSTATIC : WM_CTLCOLOREDIT;
    window *to = w->parent != 0 ? w->parent : w;
    HBRUSH brush = (HBRUSH)send(to, message, (WPARAM)dc, (LPARAM)handle_of(w));
    if (brush == NULL) {
        brush = (HBRUSH)default_control_color(message, dc);
    }
    if ((w->style & WS_DISABLED) != 0) {
        SetTextColor(dc, GetSysColor(COLOR_GRAYTEXT));
    }
    return brush;
}

static void paint(window *w, edit *e, HDC dc) {
    if (w->font != NULL) {
        SelectObject(dc, w->font);
    }
    HBRUSH brush = colors(w, dc);
    RECT all = {0, 0, client_width(w), client_height(w)};
    fill_brush(dc, &all, brush);
    if (e->border) {
        FrameRect(dc, &all, system_brush(COLOR_WINDOWFRAME));
    }
    RECT r;
    format_rect(w, e, &r);
    static WCHAR scratch[1024];
    const WCHAR *t = shown(w, e, scratch, 1024);
    /* A combo box's edit control hides its selection without the focus
     * though it carries ES_NOHIDESEL, as on Windows XP (fixtures/controls). */
    const int keeps = (w->style & ES_NOHIDESEL) != 0 && (w->style & ES_COMBO) == 0;
    const int show_selection = (e->focused || keeps) && low(e) != high(e);
    SxwSetBkMode(dc, TRANSPARENT);
    const COLORREF text_color = SetTextColor(dc, 0);
    SetTextColor(dc, text_color);
    const int left = text_left(w, e);
    int x = left;
    const int from = e->first;
    const int parts[4] = {from, show_selection && low(e) > from ? low(e) : from,
                          show_selection ? (high(e) > from ? high(e) : from) : from, e->length};
    for (int k = 0; k < 3; ++k) {
        const int a = parts[k];
        const int b = k == 2 ? e->length : parts[k + 1];
        if (b <= a || (k == 0 && !show_selection && 0)) {
            continue;
        }
        const int selected = show_selection && k == 1;
        SIZE size;
        HDC m = measuring_dc(w);
        GetTextExtentPoint32W(m, t + a, b - a, &size);
        ReleaseDC(NULL, m);
        if (selected) {
            RECT h = {x, r.top, x + size.cx, r.bottom};
            fill_rect(dc, &h, GetSysColor(COLOR_HIGHLIGHT));
            SetTextColor(dc, GetSysColor(COLOR_HIGHLIGHTTEXT));
        } else {
            SetTextColor(dc, text_color);
        }
        TextOutW(dc, x, r.top, t + a, b - a);
        x += size.cx;
    }
    SetTextColor(dc, text_color);
}

static void redraw(window *w) {
    InvalidateRect(handle_of(w), NULL, FALSE);
}

/* ---- Changing the text ----------------------------------------------------------------------- */

static void keep_undo(edit *e) {
    if (e->undo != 0) {
        SxwHeapFree(e->undo);
    }
    e->undo = SxwHeapAllocate((u32)(e->length + 1) * sizeof(WCHAR), 0);
    if (e->undo == 0) {
        e->can_undo = 0;
        return;
    }
    for (int i = 0; i < e->length; ++i) {
        e->undo[i] = e->text[i];
    }
    e->undo[e->length] = 0;
    e->undo_length = e->length;
    e->undo_anchor = e->anchor;
    e->undo_caret = e->caret;
    e->can_undo = 1;
}

/* The selection replaced with text, cut to the limit: REPLACE_FITS when it
 * all went in, REPLACE_CHANGED when the text changed. */
static void changed(window *w, edit *e);
#define REPLACE_FITS 1
#define REPLACE_CHANGED 2
static int replace(window *w, edit *e, const WCHAR *text, int length, int undoable) {
    const int a = low(e);
    const int b = high(e);
    int room = e->limit - (e->length - (b - a));
    if (room < 0) {
        room = 0;
    }
    const int fits = length <= room;
    if (!fits) {
        length = room;
    }
    if (length == 0 && a == b) {
        return fits ? REPLACE_FITS : 0;
    }
    if (undoable) {
        keep_undo(e);
    }
    if (!reserve(e, e->length - (b - a) + length)) {
        return fits ? REPLACE_FITS : 0;
    }
    const int tail = e->length - b;
    if (length > b - a) {
        for (int i = tail - 1; i >= 0; --i) {
            e->text[a + length + i] = e->text[b + i];
        }
    } else {
        for (int i = 0; i < tail; ++i) {
            e->text[a + length + i] = e->text[b + i];
        }
    }
    for (int i = 0; i < length; ++i) {
        WCHAR c = text[i];
        if ((w->style & ES_UPPERCASE) != 0 && c >= 'a' && c <= 'z') {
            c = (WCHAR)(c - 32);
        } else if ((w->style & ES_LOWERCASE) != 0 && c >= 'A' && c <= 'Z') {
            c = (WCHAR)(c + 32);
        }
        e->text[a + i] = c;
    }
    e->length = e->length - (b - a) + length;
    e->text[e->length] = 0;
    e->anchor = e->caret = a + length;
    e->modified = 1;
    return (fits ? REPLACE_FITS : 0) | REPLACE_CHANGED;
}

/* What a replacement tells the parent: EN_MAXTEXT when the limit cut it,
 * before EN_UPDATE and EN_CHANGE for what went in, as Windows XP orders
 * them (fixtures/controls). */
static void after_replace(window *w, edit *e, int result) {
    const HWND h = handle_of(w);
    if ((result & REPLACE_FITS) == 0) {
        notify(w, EN_MAXTEXT);
        if (window_of(h) == 0) {
            return;
        }
    }
    if ((result & REPLACE_CHANGED) != 0) {
        changed(w, e);
    }
}

/* After a change: EN_UPDATE before it is shown, EN_CHANGE after. */
static void changed(window *w, edit *e) {
    const HWND h = handle_of(w);
    scroll_to_caret(w, e);
    notify(w, EN_UPDATE);
    if (window_of(h) == 0) {
        return;
    }
    redraw(w);
    place_caret(w, e);
    notify(w, EN_CHANGE);
}

static void set_text(window *w, edit *e, const WCHAR *text) {
    int length = text_length(text);
    if (length > e->limit && 0) {
        length = e->limit;
    }
    if (!reserve(e, length)) {
        return;
    }
    for (int i = 0; i < length; ++i) {
        e->text[i] = text[i];
    }
    e->length = length;
    e->text[length] = 0;
    e->anchor = e->caret = 0;
    e->first = 0;
    e->modified = 0;
    e->can_undo = 0;
    (void)w;
}

static void select_range(window *w, edit *e, int start, int end) {
    if (start == -1) {
        /* Nothing selected: the caret stays where the selection ended. */
        e->anchor = e->caret;
    } else {
        if (end < 0 || end > e->length) {
            end = e->length;
        }
        if (start < 0 || start > e->length) {
            start = e->length;
        }
        e->anchor = start;
        e->caret = end;
    }
    scroll_to_caret(w, e);
    redraw(w);
    place_caret(w, e);
}

/* ---- The clipboard ----------------------------------------------------------------------------- */

static void copy(window *w, edit *e) {
    if (low(e) == high(e) || (w->style & ES_PASSWORD) != 0) {
        return;
    }
    const int n = high(e) - low(e);
    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE | GMEM_DDESHARE, (SIZE_T)(n + 1) * sizeof(WCHAR));
    WCHAR *p = mem != NULL ? GlobalLock(mem) : 0;
    if (p == 0) {
        return;
    }
    for (int i = 0; i < n; ++i) {
        p[i] = e->text[low(e) + i];
    }
    p[n] = 0;
    GlobalUnlock(mem);
    if (OpenClipboard(handle_of(w))) {
        EmptyClipboard();
        SetClipboardData(CF_UNICODETEXT, mem);
        CloseClipboard();
    } else {
        GlobalFree(mem);
    }
}

static void paste(window *w, edit *e) {
    if ((w->style & ES_READONLY) != 0 || !OpenClipboard(handle_of(w))) {
        return;
    }
    HANDLE mem = GetClipboardData(CF_UNICODETEXT);
    const WCHAR *p = mem != NULL ? GlobalLock(mem) : 0;
    if (p != 0) {
        /* One line: the text stops at its first line break. */
        int n = 0;
        while (p[n] != 0 && p[n] != '\r' && p[n] != '\n') {
            ++n;
        }
        const int result = replace(w, e, p, n, 1);
        GlobalUnlock(mem);
        CloseClipboard();
        after_replace(w, e, result);
        return;
    }
    CloseClipboard();
}

/* ---- Keys ------------------------------------------------------------------------------------- */

static int word_char(WCHAR c) {
    return c != ' ' && c != '\t';
}

static int word_left(const edit *e, int at) {
    while (at > 0 && !word_char(e->text[at - 1])) {
        --at;
    }
    while (at > 0 && word_char(e->text[at - 1])) {
        --at;
    }
    return at;
}

static int word_right(const edit *e, int at) {
    while (at < e->length && word_char(e->text[at])) {
        ++at;
    }
    while (at < e->length && !word_char(e->text[at])) {
        ++at;
    }
    return at;
}

static void move_caret(window *w, edit *e, int to, int extend) {
    e->caret = to;
    if (!extend) {
        e->anchor = to;
    }
    scroll_to_caret(w, e);
    redraw(w);
    place_caret(w, e);
}

static void delete_range(window *w, edit *e, int a, int b) {
    if (a == b) {
        return;
    }
    e->anchor = a;
    e->caret = b;
    replace(w, e, 0, 0, 1);
    changed(w, e);
}

static void key_down(window *w, edit *e, WPARAM vk) {
    const int shift = GetKeyState(VK_SHIFT) < 0;
    const int ctrl = GetKeyState(VK_CONTROL) < 0;
    switch (vk) {
    case VK_LEFT:
        if (!shift && low(e) != high(e)) {
            move_caret(w, e, low(e), 0);
        } else {
            move_caret(w, e, ctrl ? word_left(e, e->caret) : (e->caret > 0 ? e->caret - 1 : 0), shift);
        }
        break;
    case VK_RIGHT:
        if (!shift && low(e) != high(e)) {
            move_caret(w, e, high(e), 0);
        } else {
            move_caret(w, e, ctrl ? word_right(e, e->caret) : (e->caret < e->length ? e->caret + 1 : e->length),
                       shift);
        }
        break;
    case VK_HOME:
        move_caret(w, e, 0, shift);
        break;
    case VK_END:
        move_caret(w, e, e->length, shift);
        break;
    case VK_DELETE:
        if ((w->style & ES_READONLY) != 0) {
            break;
        }
        if (shift) {
            copy(w, e);
            delete_range(w, e, low(e), high(e));
        } else if (low(e) != high(e)) {
            delete_range(w, e, low(e), high(e));
        } else if (e->caret < e->length) {
            delete_range(w, e, e->caret, ctrl ? word_right(e, e->caret) : e->caret + 1);
        }
        break;
    case VK_INSERT:
        if (ctrl) {
            copy(w, e);
        } else if (shift) {
            paste(w, e);
        }
        break;
    default:
        break;
    }
}

static void character(window *w, edit *e, WCHAR c) {
    switch (c) {
    case 0x03: /* Ctrl+C */
        copy(w, e);
        return;
    case 0x16: /* Ctrl+V */
        paste(w, e);
        return;
    case 0x18: /* Ctrl+X */
        if ((w->style & ES_READONLY) == 0) {
            copy(w, e);
            delete_range(w, e, low(e), high(e));
        }
        return;
    case 0x1a: /* Ctrl+Z */
        send(w, WM_UNDO, 0, 0);
        return;
    case 0x08: /* Backspace */
        if ((w->style & ES_READONLY) != 0) {
            return;
        }
        if (low(e) != high(e)) {
            delete_range(w, e, low(e), high(e));
        } else if (e->caret > 0) {
            delete_range(w, e, e->caret - 1, e->caret);
        }
        return;
    default:
        break;
    }
    if (c < 0x20 || (w->style & ES_READONLY) != 0) {
        return;
    }
    if ((w->style & ES_NUMBER) != 0 && (c < '0' || c > '9')) {
        MessageBeep(0);
        return;
    }
    after_replace(w, e, replace(w, e, &c, 1, 1));
}

/* ---- The window procedure ------------------------------------------------------------------- */

static void undo(window *w, edit *e) {
    if (!e->can_undo || e->undo == 0) {
        return;
    }
    WCHAR *old = e->undo;
    const int old_length = e->undo_length;
    const int old_anchor = e->undo_anchor;
    const int old_caret = e->undo_caret;
    e->undo = 0;
    keep_undo(e);
    if (reserve(e, old_length)) {
        for (int i = 0; i < old_length; ++i) {
            e->text[i] = old[i];
        }
        e->length = old_length;
        e->text[old_length] = 0;
        e->anchor = old_anchor;
        e->caret = old_caret;
    }
    SxwHeapFree(old);
    e->modified = 1;
    changed(w, e);
}

static LRESULT WINAPI edit_proc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    window *w = window_of(hwnd);
    if (w == 0) {
        return 0;
    }
    edit *e = edit_of(w);
    if (e == 0 && message != WM_NCCREATE) {
        return DefWindowProcW(hwnd, message, wParam, lParam);
    }
    switch (message) {
    case WM_NCCREATE: {
        e = SxwHeapAllocate(sizeof(edit), 1);
        if (e == 0) {
            return FALSE;
        }
        w->control = e;
        e->limit = DEFAULT_LIMIT;
        if ((w->style & ES_PASSWORD) != 0) {
            e->password = '*';
        }
        const CREATESTRUCTW *cs = (const CREATESTRUCTW *)lParam;
        reserve(e, 0);
        if (cs != NULL && cs->lpszName != NULL && (u32)cs->lpszName >= 0x10000u) {
            set_text(w, e, cs->lpszName);
        }
        /* Windows XP takes WS_BORDER out of an edit control's style and
         * draws the line itself, inside its client area
         * (fixtures/controls). */
        if ((w->style & WS_BORDER) != 0 && (w->ex_style & WS_EX_CLIENTEDGE) == 0) {
            e->border = 1;
            w->style &= ~WS_BORDER;
        }
        return DefWindowProcW(hwnd, message, wParam, lParam);
    }
    case WM_CREATE:
        /* Made with text, the control tells its parent as Windows XP does
         * (fixtures/controls). */
        if (e->length > 0) {
            notify(w, EN_UPDATE);
            if (window_of(hwnd) != 0) {
                notify(w, EN_CHANGE);
            }
        }
        return 0;
    case WM_NCDESTROY:
        if (e->text != 0) {
            SxwHeapFree(e->text);
        }
        if (e->undo != 0) {
            SxwHeapFree(e->undo);
        }
        SxwHeapFree(e);
        w->control = 0;
        return DefWindowProcW(hwnd, message, wParam, lParam);
    case WM_GETDLGCODE:
        return DLGC_WANTCHARS | DLGC_HASSETSEL | DLGC_WANTARROWS;
    case WM_SETFONT:
        w->font = (HFONT)wParam;
        if (lParam != 0) {
            redraw(w);
        }
        place_caret(w, e);
        return 0;
    case WM_GETFONT:
        return (LRESULT)w->font;
    case WM_SETTEXT:
        set_text(w, e, (const WCHAR *)lParam);
        redraw(w);
        place_caret(w, e);
        notify(w, EN_UPDATE);
        if (window_of(hwnd) != 0) {
            notify(w, EN_CHANGE);
        }
        return TRUE;
    case WM_GETTEXT: {
        if (wParam == 0 || lParam == 0) {
            return 0;
        }
        WCHAR *out = (WCHAR *)lParam;
        int n = 0;
        while (n < e->length && n + 1 < (int)wParam) {
            out[n] = e->text[n];
            ++n;
        }
        out[n] = 0;
        return n;
    }
    case WM_GETTEXTLENGTH:
        return e->length;
    case WM_ERASEBKGND: {
        HBRUSH brush = colors(w, (HDC)wParam);
        RECT all = {0, 0, client_width(w), client_height(w)};
        fill_brush((HDC)wParam, &all, brush);
        return 1;
    }
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        paint(w, e, dc);
        EndPaint(hwnd, &ps);
        place_caret(w, e);
        return 0;
    }
    case WM_ENABLE:
        redraw(w);
        return 0;
    case WM_SETFOCUS:
        e->focused = 1;
        CreateCaret(hwnd, NULL, 1, font_height(w));
        place_caret(w, e);
        ShowCaret(hwnd);
        redraw(w);
        notify(w, EN_SETFOCUS);
        return 0;
    case WM_KILLFOCUS:
        e->focused = 0;
        e->dragging = 0;
        DestroyCaret();
        redraw(w);
        notify(w, EN_KILLFOCUS);
        return 0;
    case WM_LBUTTONDOWN: {
        if (g_focus != w) {
            SetFocus(hwnd);
        }
        const int at = char_at(w, e, GET_X_LPARAM(lParam));
        move_caret(w, e, at, (wParam & MK_SHIFT) != 0);
        e->dragging = 1;
        SetCapture(hwnd);
        return 0;
    }
    case WM_MOUSEMOVE:
        if (e->dragging && g_capture == w) {
            const int at = char_at(w, e, GET_X_LPARAM(lParam));
            if (at != e->caret) {
                move_caret(w, e, at, 1);
            }
        }
        return 0;
    case WM_LBUTTONUP:
        if (e->dragging) {
            e->dragging = 0;
            if (g_capture == w) {
                ReleaseCapture();
            }
        }
        return 0;
    case WM_CAPTURECHANGED:
        e->dragging = 0;
        return 0;
    case WM_LBUTTONDBLCLK: {
        const int at = char_at(w, e, GET_X_LPARAM(lParam));
        int a = at;
        int b = at;
        while (a > 0 && word_char(e->text[a - 1])) {
            --a;
        }
        while (b < e->length && word_char(e->text[b])) {
            ++b;
        }
        e->anchor = a;
        e->caret = b;
        scroll_to_caret(w, e);
        redraw(w);
        place_caret(w, e);
        return 0;
    }
    case WM_KEYDOWN:
        key_down(w, e, wParam);
        return 0;
    case WM_CHAR:
        character(w, e, (WCHAR)wParam);
        return 0;
    case WM_COPY:
        copy(w, e);
        return 0;
    case WM_CUT:
        if ((w->style & ES_READONLY) == 0) {
            copy(w, e);
            delete_range(w, e, low(e), high(e));
        }
        return 0;
    case WM_PASTE:
        paste(w, e);
        return 0;
    case WM_CLEAR:
        if ((w->style & ES_READONLY) == 0) {
            delete_range(w, e, low(e), high(e));
        }
        return 0;
    case WM_UNDO:
    case EM_UNDO:
        if (!e->can_undo) {
            return FALSE;
        }
        undo(w, e);
        return TRUE;
    case EM_CANUNDO:
        return e->can_undo;
    case EM_EMPTYUNDOBUFFER:
        e->can_undo = 0;
        return 0;
    case EM_GETSEL:
        if (wParam != 0) {
            *(DWORD *)wParam = (DWORD)low(e);
        }
        if (lParam != 0) {
            *(DWORD *)lParam = (DWORD)high(e);
        }
        return MAKELPARAM(low(e), high(e));
    case EM_SETSEL:
        select_range(w, e, (int)wParam, (int)lParam);
        return 0;
    case EM_REPLACESEL: {
        const WCHAR *text = (const WCHAR *)lParam;
        after_replace(w, e, replace(w, e, text, text_length(text), wParam != 0));
        return 0;
    }
    case EM_LIMITTEXT:
        e->limit = wParam == 0 ? NO_LIMIT : (int)wParam;
        return 0;
    case EM_GETLIMITTEXT:
        return e->limit;
    case EM_GETMODIFY:
        return e->modified;
    case EM_SETMODIFY:
        e->modified = wParam != 0;
        return 0;
    case EM_GETLINECOUNT:
        return 1;
    case EM_LINEINDEX:
        return (int)wParam <= 0 ? 0 : -1;
    case EM_LINELENGTH:
        return e->length - (high(e) - low(e)) * ((int)wParam == -1);
    case EM_LINEFROMCHAR:
        return 0;
    case EM_GETFIRSTVISIBLELINE:
        return e->first;
    case EM_GETLINE: {
        WCHAR *out = (WCHAR *)lParam;
        if (out == NULL) {
            return 0;
        }
        const int capacity = (int)out[0];
        int n = 0;
        while (n < e->length && n < capacity) {
            out[n] = e->text[n];
            ++n;
        }
        return n;
    }
    case EM_SETREADONLY:
        if (wParam) {
            w->style |= ES_READONLY;
        } else {
            w->style &= ~(DWORD)ES_READONLY;
        }
        redraw(w);
        return TRUE;
    case EM_SETPASSWORDCHAR:
        e->password = (WCHAR)wParam;
        if (wParam != 0) {
            w->style |= ES_PASSWORD;
        } else {
            w->style &= ~(DWORD)ES_PASSWORD;
        }
        redraw(w);
        return 0;
    case EM_GETPASSWORDCHAR:
        return (w->style & ES_PASSWORD) != 0 ? e->password : 0;
    case EM_SETMARGINS:
        if ((wParam & EC_LEFTMARGIN) != 0) {
            e->left_margin = LOWORD(lParam) == EC_USEFONTINFO ? 0 : LOWORD(lParam);
        }
        if ((wParam & EC_RIGHTMARGIN) != 0) {
            e->right_margin = HIWORD(lParam) == EC_USEFONTINFO ? 0 : HIWORD(lParam);
        }
        redraw(w);
        return 0;
    case EM_GETMARGINS:
        return MAKELPARAM(e->left_margin, e->right_margin);
    case EM_SCROLLCARET:
        scroll_to_caret(w, e);
        redraw(w);
        place_caret(w, e);
        return TRUE;
    case EM_POSFROMCHAR: {
        RECT r;
        format_rect(w, e, &r);
        const int at = (int)wParam > e->length ? e->length : (int)wParam;
        return MAKELPARAM(x_of(w, e, at), r.top);
    }
    case EM_CHARFROMPOS:
        return MAKELPARAM(char_at(w, e, GET_X_LPARAM(lParam)), 0);
    case EM_GETRECT: {
        RECT *r = (RECT *)lParam;
        if (r != NULL) {
            format_rect(w, e, r);
        }
        return 0;
    }
    case EM_SETRECT:
    case EM_SETRECTNP:
    case EM_SETTABSTOPS:
    case EM_SETWORDBREAKPROC:
    case EM_LINESCROLL:
    case EM_SCROLL:
        return 0;
    default:
        break;
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

void register_edit(void) {
    static const WCHAR k_name[] = {'E', 'd', 'i', 't', 0};
    register_system_class(CS_DBLCLKS | CS_PARENTDC, edit_proc, k_name, ATOM_EDIT, NULL, 6);
    wclass *c = class_of_atom(ATOM_EDIT);
    if (c != 0) {
        c->cursor = LoadCursorW(NULL, IDC_IBEAM);
    }
}
