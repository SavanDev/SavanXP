/*
 * listbox.c -- The ListBox control (docs/userland/WIN32.md, "Edit, list
 * box and combo box"): items with text and a value, kept sorted or in the
 * order given, one selection, a caret item, a top item and a scroll bar,
 * owner-drawn items, the pointer and the keyboard, and the LB_ messages and
 * LBN_ notifications Windows XP answers and sends (fixtures/controls).
 * Multiple selection is not written: the profile's list boxes take one.
 *
 * A list box inside a combo box (LBS_COMBOBOX) tells the combo box instead
 * of notifying its parent, and draws its owner-drawn items through the
 * combo box's parent with the combo box's id.
 */
#include "user32_private.h"

#define ATOM_LISTBOX 0x0083
#define ATOM_COMBOLBOX 0x8003
#define SEARCH_MS 1000u

typedef struct item {
    WCHAR *text; /* 0 without LBS_HASSTRINGS in an owner-drawn list */
    ULONG_PTR data;
} item;

typedef struct list {
    item *items;
    int count;
    int capacity;
    int selected; /* -1: none */
    int caret;    /* the focus rectangle's item */
    int top;
    int item_height;
    int focused;
    int tracking; /* a button is down on it */
    window *combo; /* LBS_COMBOBOX: the combo box it belongs to */
    int caret_on;  /* a combo box's list: LB_CARETON, its focus shown */
    WCHAR search[32];
    int search_length;
    u32 search_time;
} list;

window *l_parent(window *w);

static list *list_of(window *w) {
    return (list *)w->control;
}

static int owner_drawn(const window *w) {
    return (w->style & (LBS_OWNERDRAWFIXED | LBS_OWNERDRAWVARIABLE)) != 0;
}

static int has_strings(const window *w) {
    return !owner_drawn(w) || (w->style & LBS_HASSTRINGS) != 0;
}

static void notify(window *w, WORD code) {
    if ((w->style & LBS_NOTIFY) == 0 && code != (WORD)LBN_ERRSPACE) {
        return;
    }
    if (w->parent != 0) {
        send(w->parent, WM_COMMAND, MAKELPARAM(w->id, code), (LPARAM)handle_of(w));
    }
}

/* The window owner-drawn items are drawn by, and the id they carry. */
static window *drawer(window *w, list *l, UINT *type, UINT *id) {
    if (l->combo != 0) {
        *type = ODT_COMBOBOX;
        *id = (UINT)l->combo->id;
        return l->combo->parent;
    }
    *type = ODT_LISTBOX;
    *id = (UINT)w->id;
    return w->parent;
}

int list_box_count(window *w) {
    list *l = list_of(w);
    return l != 0 ? l->count : 0;
}

int list_box_selection(window *w) {
    list *l = list_of(w);
    return l != 0 ? l->selected : -1;
}

void list_box_text(window *w, int index, WCHAR *out, int capacity) {
    list *l = list_of(w);
    out[0] = 0;
    if (l != 0 && index >= 0 && index < l->count && l->items[index].text != 0) {
        copy_text(out, l->items[index].text, capacity);
    }
}

/* ---- Geometry --------------------------------------------------------------------------------- */

static int visible_rows(window *w, list *l) {
    const int h = l->item_height > 0 ? l->item_height : 1;
    const int rows = client_height(w) / h;
    return rows > 0 ? rows : 1;
}

/* The item height of the font: its height, as Windows XP gives a list box
 * of an 8-point font 13 pixels (fixtures/controls). */
static int height_of_font(window *w) {
    HDC dc = GetDC(NULL);
    if (w->font != NULL) {
        SelectObject(dc, w->font);
    }
    TEXTMETRICW tm;
    GetTextMetricsW(dc, &tm);
    ReleaseDC(NULL, dc);
    return tm.tmHeight;
}

static void update_scroll(window *w, list *l) {
    if ((w->style & WS_VSCROLL) == 0 && (w->control_state & 0x10000u) == 0) {
        return;
    }
    SCROLLINFO si;
    si.cbSize = sizeof(si);
    si.fMask = SIF_RANGE | SIF_PAGE | SIF_POS | ((w->style & LBS_DISABLENOSCROLL) != 0 ? SIF_DISABLENOSCROLL : 0);
    si.nMin = 0;
    si.nMax = l->count > 0 ? l->count - 1 : 0;
    si.nPage = (UINT)visible_rows(w, l);
    si.nPos = l->top;
    SetScrollInfo(handle_of(w), SB_VERT, &si, TRUE);
}

static void set_top(window *w, list *l, int top) {
    const int rows = visible_rows(w, l);
    if (top > l->count - rows) {
        top = l->count - rows;
    }
    if (top < 0) {
        top = 0;
    }
    if (top != l->top) {
        l->top = top;
        InvalidateRect(handle_of(w), NULL, TRUE);
    }
    update_scroll(w, l);
}

static void show_item(window *w, list *l, int index) {
    if (index < 0) {
        return;
    }
    const int rows = visible_rows(w, l);
    if (index < l->top) {
        set_top(w, l, index);
    } else if (index >= l->top + rows) {
        set_top(w, l, index - rows + 1);
    }
}

static int item_at(window *w, list *l, int y) {
    if (y < 0) {
        return l->top > 0 ? l->top - 1 : 0;
    }
    int index = l->top + y / (l->item_height > 0 ? l->item_height : 1);
    if (y >= client_height(w)) {
        index = l->top + visible_rows(w, l);
    }
    if (index >= l->count) {
        index = l->count - 1;
    }
    return index;
}

/* A list box without LBS_NOINTEGRALHEIGHT is made a whole number of items
 * high, as Windows does. */
static void integral_height(window *w, list *l) {
    if ((w->style & LBS_NOINTEGRALHEIGHT) != 0 || l->item_height <= 0 || l->combo != 0) {
        return;
    }
    const int extra = client_height(w) % l->item_height;
    if (extra != 0 && client_height(w) > l->item_height) {
        set_window_pos(w, NULL, 0, 0, w->rect.right - w->rect.left, w->rect.bottom - w->rect.top - extra,
                       SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
}

/* ---- Drawing ---------------------------------------------------------------------------------- */

static void draw_item(window *w, list *l, HDC dc, int index, UINT action) {
    RECT r = {0, (index - l->top) * l->item_height, client_width(w), (index - l->top + 1) * l->item_height};
    const int selected = index == l->selected;
    const int focus = l->focused && index == l->caret && (top_of(w)->ui_state & UISF_HIDEFOCUS) == 0;
    if (owner_drawn(w)) {
        UINT type;
        UINT id;
        window *to = drawer(w, l, &type, &id);
        DRAWITEMSTRUCT d;
        zero(&d, sizeof(d));
        d.CtlType = type;
        d.CtlID = id;
        d.itemID = (UINT)index;
        d.itemAction = action;
        d.itemState = (selected ? ODS_SELECTED : 0) | (focus ? ODS_FOCUS : 0) |
                      ((w->style & WS_DISABLED) != 0 ? ODS_DISABLED : 0);
        d.hwndItem = handle_of(w);
        d.hDC = dc;
        d.rcItem = r;
        d.itemData = index >= 0 && index < l->count ? l->items[index].data : (ULONG_PTR)LB_ERR;
        if (to != 0) {
            send(to, WM_DRAWITEM, id, (LPARAM)&d);
        }
        return;
    }
    if (index < 0 || index >= l->count) {
        return;
    }
    if (selected) {
        fill_rect(dc, &r, GetSysColor(COLOR_HIGHLIGHT));
    }
    const COLORREF text = SetTextColor(dc, 0);
    SetTextColor(dc, selected ? GetSysColor(COLOR_HIGHLIGHTTEXT)
                     : (w->style & WS_DISABLED) != 0 ? GetSysColor(COLOR_GRAYTEXT)
                                                    : text);
    if (l->items[index].text != 0) {
        TextOutW(dc, r.left + 2, r.top, l->items[index].text, text_length(l->items[index].text));
    }
    SetTextColor(dc, text);
    if (focus) {
        draw_focus_rect(dc, &r);
    }
}

static HBRUSH colors(window *w, HDC dc) {
    window *to = l_parent(w);
    HBRUSH brush = to != 0 ? (HBRUSH)send(to, WM_CTLCOLORLISTBOX, (WPARAM)dc, (LPARAM)handle_of(w)) : NULL;
    if (brush == NULL) {
        brush = (HBRUSH)default_control_color(WM_CTLCOLORLISTBOX, dc);
    }
    return brush;
}

/* ODA_FOCUS for the caret item, the focus on or off. */
static void focus_item(window *w, list *l, HDC dc, int on) {
    if (!owner_drawn(w)) {
        return;
    }
    UINT type;
    UINT id;
    window *to = drawer(w, l, &type, &id);
    DRAWITEMSTRUCT d;
    zero(&d, sizeof(d));
    d.CtlType = type;
    d.CtlID = id;
    d.itemID = (UINT)l->caret;
    d.itemAction = ODA_FOCUS;
    d.itemState = (on ? ODS_FOCUS : 0) | (l->caret >= 0 && l->caret == l->selected ? ODS_SELECTED : 0);
    d.hwndItem = handle_of(w);
    d.hDC = dc;
    d.rcItem.left = 0;
    d.rcItem.top = (l->caret - l->top) * l->item_height;
    d.rcItem.right = client_width(w);
    d.rcItem.bottom = d.rcItem.top + l->item_height;
    d.itemData = l->caret >= 0 && l->caret < l->count ? l->items[l->caret].data : (ULONG_PTR)LB_ERR;
    if (to != 0) {
        send(to, WM_DRAWITEM, id, (LPARAM)&d);
    }
}

static void paint(window *w, list *l, HDC dc) {
    if (w->font != NULL) {
        SelectObject(dc, w->font);
    }
    SxwSetBkMode(dc, TRANSPARENT);
    HBRUSH brush = colors(w, dc);
    RECT all = {0, 0, client_width(w), client_height(w)};
    fill_brush(dc, &all, brush);
    /* A combo box's owner-drawn list with its caret on takes the focus off
     * its caret item, draws the items and puts the focus back, as Windows
     * XP does (fixtures/controls). */
    const int combo_focus = l->combo != 0 && l->caret_on && owner_drawn(w);
    if (combo_focus) {
        focus_item(w, l, dc, 0);
    }
    for (int i = l->top; i < l->count && (i - l->top) * l->item_height < client_height(w); ++i) {
        draw_item(w, l, dc, i, ODA_DRAWENTIRE);
    }
    if (combo_focus) {
        focus_item(w, l, dc, 1);
    } else if (l->count == 0 && l->focused && owner_drawn(w)) {
        draw_item(w, l, dc, -1, ODA_FOCUS);
    }
}

static void redraw_item(window *w, list *l, int index) {
    if (index < l->top || index >= l->top + visible_rows(w, l) + 1 || !visible_chain(w)) {
        return;
    }
    RECT r = {0, (index - l->top) * l->item_height, client_width(w), (index - l->top + 1) * l->item_height};
    InvalidateRect(handle_of(w), &r, TRUE);
}

/* ---- Items -------------------------------------------------------------------------------------- */

static int reserve(list *l, int count) {
    if (count <= l->capacity) {
        return 1;
    }
    int capacity = l->capacity == 0 ? 16 : l->capacity * 2;
    while (capacity < count) {
        capacity *= 2;
    }
    item *grown = SxwHeapAllocate((u32)capacity * sizeof(item), 0);
    if (grown == 0) {
        return 0;
    }
    for (int i = 0; i < l->count; ++i) {
        grown[i] = l->items[i];
    }
    if (l->items != 0) {
        SxwHeapFree(l->items);
    }
    l->items = grown;
    l->capacity = capacity;
    return 1;
}

static WCHAR *copy_of(const WCHAR *text) {
    const int n = text_length(text);
    WCHAR *copy = SxwHeapAllocate((u32)(n + 1) * sizeof(WCHAR), 0);
    if (copy != 0) {
        for (int i = 0; i <= n; ++i) {
            copy[i] = text != NULL ? text[i] : 0;
        }
    }
    return copy;
}

/* The order of a sorted list: CompareStringW's, which puts a word before
 * the same word capitalized. */
static int compare(window *w, list *l, const item *a, LPARAM b_value, int b_index) {
    if (owner_drawn(w) && !has_strings(w)) {
        UINT type;
        UINT id;
        window *to = drawer(w, l, &type, &id);
        COMPAREITEMSTRUCT c;
        zero(&c, sizeof(c));
        c.CtlType = type;
        c.CtlID = id;
        c.hwndItem = handle_of(w);
        c.itemID1 = (UINT)-1;
        c.itemData1 = (ULONG_PTR)b_value;
        c.itemID2 = (UINT)b_index;
        c.itemData2 = a->data;
        return to != 0 ? -(int)send(to, WM_COMPAREITEM, id, (LPARAM)&c) : 0;
    }
    return CompareStringW(0x0400, NORM_IGNORECASE, a->text, -1, (const WCHAR *)b_value, -1) - 2;
}

static int insert(window *w, list *l, int at, LPARAM value, int sorted) {
    if (!reserve(l, l->count + 1)) {
        notify(w, LBN_ERRSPACE);
        return LB_ERRSPACE;
    }
    if (sorted) {
        at = l->count;
        for (int i = 0; i < l->count; ++i) {
            /* Before the first item not smaller: a new item goes ahead of
             * one that compares equal, as on Windows XP. */
            if (compare(w, l, &l->items[i], value, i) >= 0) {
                at = i;
                break;
            }
        }
    }
    if (at < 0 || at > l->count) {
        at = l->count;
    }
    item it;
    it.text = has_strings(w) ? copy_of((const WCHAR *)value) : 0;
    it.data = has_strings(w) ? 0 : (ULONG_PTR)value;
    if (has_strings(w) && it.text == 0) {
        return LB_ERRSPACE;
    }
    for (int i = l->count; i > at; --i) {
        l->items[i] = l->items[i - 1];
    }
    l->items[at] = it;
    ++l->count;
    if (l->selected >= at) {
        ++l->selected;
    }
    if (l->caret >= at && l->count > 1) {
        ++l->caret;
    }
    if (owner_drawn(w) && (w->style & LBS_OWNERDRAWVARIABLE) != 0) {
        MEASUREITEMSTRUCT mi;
        UINT type;
        UINT id;
        window *to = drawer(w, l, &type, &id);
        zero(&mi, sizeof(mi));
        mi.CtlType = type;
        mi.CtlID = id;
        mi.itemID = (UINT)at;
        mi.itemHeight = (UINT)l->item_height;
        mi.itemData = it.data;
        if (to != 0) {
            send(to, WM_MEASUREITEM, id, (LPARAM)&mi);
        }
    }
    InvalidateRect(handle_of(w), NULL, TRUE);
    update_scroll(w, l);
    return at;
}

/* WM_DELETEITEM for every item that goes, owner-drawn or not, with data or
 * without, as Windows XP sends it (fixtures/controls). */
static void delete_item_message(window *w, list *l, int index) {
    UINT type;
    UINT id;
    window *to = drawer(w, l, &type, &id);
    DELETEITEMSTRUCT d;
    d.CtlType = type;
    d.CtlID = id;
    d.itemID = (UINT)index;
    d.hwndItem = handle_of(w);
    d.itemData = l->items[index].data;
    if (to != 0) {
        send(to, WM_DELETEITEM, id, (LPARAM)&d);
    }
}

static int remove_item(window *w, list *l, int index) {
    if (index < 0 || index >= l->count) {
        return LB_ERR;
    }
    delete_item_message(w, l, index);
    if (l->items[index].text != 0) {
        SxwHeapFree(l->items[index].text);
    }
    for (int i = index; i + 1 < l->count; ++i) {
        l->items[i] = l->items[i + 1];
    }
    --l->count;
    if (l->selected == index) {
        l->selected = -1;
    } else if (l->selected > index) {
        --l->selected;
    }
    if (l->caret > index || l->caret >= l->count) {
        l->caret = l->caret > 0 ? l->caret - 1 : 0;
    }
    if (l->top > 0 && l->top + visible_rows(w, l) > l->count) {
        l->top = l->count - visible_rows(w, l) > 0 ? l->count - visible_rows(w, l) : 0;
    }
    InvalidateRect(handle_of(w), NULL, TRUE);
    update_scroll(w, l);
    return l->count;
}

/* Every item gone, from the last: emptied or destroyed, Windows XP sends
 * WM_DELETEITEM for an owner-drawn list's items alone (fixtures/controls). */
static void reset(window *w, list *l) {
    for (int i = l->count - 1; i >= 0; --i) {
        if (owner_drawn(w)) {
            delete_item_message(w, l, i);
        }
        if (l->items[i].text != 0) {
            SxwHeapFree(l->items[i].text);
        }
    }
    l->count = 0;
    l->selected = -1;
    l->caret = l->combo != 0 ? -1 : 0;
    l->top = 0;
    InvalidateRect(handle_of(w), NULL, TRUE);
    update_scroll(w, l);
}

/* The first item after start (wrapping) whose text begins with prefix, or
 * equals it; LB_ERR for none. */
static int find(window *w, list *l, int start, const WCHAR *prefix, int exact) {
    if (l->count == 0) {
        return LB_ERR;
    }
    if (start < -1 || start >= l->count) {
        start = -1;
    }
    const int n = text_length(prefix);
    for (int k = 1; k <= l->count; ++k) {
        const int i = (start + k) % l->count;
        if (!has_strings(w)) {
            if (l->items[i].data == (ULONG_PTR)prefix) {
                return i;
            }
            continue;
        }
        const WCHAR *t = l->items[i].text;
        const int length = exact ? -1 : n;
        if (exact ? CompareStringW(0x0400, NORM_IGNORECASE, t, -1, prefix, -1) == 2
                  : text_length(t) >= n && CompareStringW(0x0400, NORM_IGNORECASE, t, length, prefix, n) == 2) {
            return i;
        }
    }
    return LB_ERR;
}

/* The selection moves to index (-1 for none), redrawn and in view. */
static void select_item(window *w, list *l, int index, int scroll) {
    const int old = l->selected;
    const int old_caret = l->caret;
    l->selected = index;
    if (index >= 0) {
        l->caret = index;
    }
    if (scroll) {
        show_item(w, l, index);
    }
    if (old != index) {
        redraw_item(w, l, old);
        redraw_item(w, l, index);
    }
    if (old_caret != l->caret) {
        redraw_item(w, l, old_caret);
    }
}

/* ---- Input -------------------------------------------------------------------------------------- */

static void user_select(window *w, list *l, int index) {
    if (index < 0 || index >= l->count) {
        return;
    }
    const int changed = index != l->selected;
    select_item(w, l, index, 1);
    if (!changed) {
        return;
    }
    if (l->combo != 0) {
        combo_list_event(l->combo, COMBO_LIST_SELECTED, index);
    } else {
        notify(w, LBN_SELCHANGE);
    }
}

static void key_down(window *w, list *l, WPARAM vk) {
    if (l->count == 0) {
        return;
    }
    const int rows = visible_rows(w, l);
    int to = l->caret;
    switch (vk) {
    case VK_UP: to = l->caret - 1; break;
    case VK_DOWN: to = l->selected < 0 ? l->caret : l->caret + 1; break;
    case VK_PRIOR: to = l->caret - (rows > 1 ? rows - 1 : 1); break;
    case VK_NEXT: to = l->caret + (rows > 1 ? rows - 1 : 1); break;
    case VK_HOME: to = 0; break;
    case VK_END: to = l->count - 1; break;
    case VK_RETURN:
        if (l->combo != 0) {
            combo_list_event(l->combo, COMBO_LIST_CHOSEN, l->selected);
        }
        return;
    case VK_ESCAPE:
        if (l->combo != 0) {
            combo_list_event(l->combo, COMBO_LIST_CANCEL, l->selected);
        }
        return;
    default:
        return;
    }
    if (to < 0) {
        to = 0;
    }
    if (to >= l->count) {
        to = l->count - 1;
    }
    user_select(w, l, to);
}

/* Typing selects the next item that starts with what was typed within a
 * second, as Windows does. */
static void character(window *w, list *l, WCHAR c) {
    if (c < 0x20 || l->count == 0 || !has_strings(w)) {
        return;
    }
    const u32 now = ticks();
    if (now - l->search_time > SEARCH_MS || l->search_length >= 31) {
        l->search_length = 0;
    }
    l->search_time = now;
    l->search[l->search_length++] = c;
    l->search[l->search_length] = 0;
    const int start = l->search_length == 1 ? l->caret : l->caret - 1;
    const int found = find(w, l, start, l->search, 0);
    if (found != LB_ERR) {
        user_select(w, l, found);
    }
}

static void vscroll(window *w, list *l, WPARAM code) {
    const int rows = visible_rows(w, l);
    switch (LOWORD(code)) {
    case SB_LINEUP: set_top(w, l, l->top - 1); break;
    case SB_LINEDOWN: set_top(w, l, l->top + 1); break;
    case SB_PAGEUP: set_top(w, l, l->top - (rows > 1 ? rows - 1 : 1)); break;
    case SB_PAGEDOWN: set_top(w, l, l->top + (rows > 1 ? rows - 1 : 1)); break;
    case SB_THUMBTRACK:
    case SB_THUMBPOSITION: set_top(w, l, HIWORD(code)); break;
    case SB_TOP: set_top(w, l, 0); break;
    case SB_BOTTOM: set_top(w, l, l->count); break;
    default: break;
    }
}

/* ---- The window procedure ---------------------------------------------------------------------- */

static LRESULT WINAPI list_proc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    window *w = window_of(hwnd);
    if (w == 0) {
        return 0;
    }
    list *l = list_of(w);
    if (l == 0 && message != WM_NCCREATE) {
        return DefWindowProcW(hwnd, message, wParam, lParam);
    }
    switch (message) {
    case WM_NCCREATE: {
        l = SxwHeapAllocate(sizeof(list), 1);
        if (l == 0) {
            return FALSE;
        }
        w->control = l;
        l->selected = -1;
        const CREATESTRUCTW *cs = (const CREATESTRUCTW *)lParam;
        if ((w->style & LBS_COMBOBOX) != 0 && cs != NULL && cs->lpCreateParams != NULL) {
            l->combo = window_of((HWND)cs->lpCreateParams);
            /* No caret item until one is chosen (fixtures/controls). */
            l->caret = -1;
        }
        l->item_height = height_of_font(w);
        return DefWindowProcW(hwnd, message, wParam, lParam);
    }
    case WM_CREATE:
        if (owner_drawn(w) && (w->style & LBS_OWNERDRAWFIXED) != 0) {
            MEASUREITEMSTRUCT mi;
            UINT type;
            UINT id;
            window *to = drawer(w, l, &type, &id);
            zero(&mi, sizeof(mi));
            mi.CtlType = type;
            mi.CtlID = id;
            mi.itemHeight = (UINT)l->item_height;
            if (to != 0 && l->combo == 0) {
                send(to, WM_MEASUREITEM, id, (LPARAM)&mi);
                l->item_height = (int)mi.itemHeight;
            }
        }
        update_scroll(w, l);
        return 0;
    case WM_NCDESTROY:
        reset(w, l);
        if (l->items != 0) {
            SxwHeapFree(l->items);
        }
        SxwHeapFree(l);
        w->control = 0;
        return DefWindowProcW(hwnd, message, wParam, lParam);
    case WM_GETDLGCODE:
        return DLGC_WANTARROWS | DLGC_WANTCHARS;
    case WM_SETFONT:
        w->font = (HFONT)wParam;
        if (!owner_drawn(w)) {
            l->item_height = height_of_font(w);
        }
        integral_height(w, l);
        update_scroll(w, l);
        if (lParam != 0) {
            InvalidateRect(hwnd, NULL, TRUE);
        }
        return 0;
    case WM_GETFONT:
        return (LRESULT)w->font;
    case WM_SIZE:
        set_top(w, l, l->top);
        return 0;
    case WM_ERASEBKGND: {
        HBRUSH brush = colors(w, (HDC)wParam);
        RECT all = {0, 0, client_width(w), client_height(w)};
        fill_brush((HDC)wParam, &all, brush);
        return 1;
    }
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        paint(w, l, dc);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_ENABLE:
        InvalidateRect(hwnd, NULL, TRUE);
        return 0;
    case WM_SETFOCUS:
        l->focused = 1;
        redraw_item(w, l, l->caret);
        notify(w, LBN_SETFOCUS);
        return 0;
    case WM_KILLFOCUS:
        l->focused = 0;
        redraw_item(w, l, l->caret);
        notify(w, LBN_KILLFOCUS);
        return 0;
    case WM_LBUTTONDOWN:
        if (l->combo != 0 && (w->style & WS_POPUP) != 0) {
            /* A press outside the dropped list gives it up; one on the
             * combo box's button closes it there. */
            const int x = GET_X_LPARAM(lParam);
            const int y = GET_Y_LPARAM(lParam);
            if (x < 0 || y < 0 || x >= w->rect.right - w->rect.left || y >= w->rect.bottom - w->rect.top) {
                combo_list_event(l->combo, COMBO_LIST_CANCEL, l->selected);
                return 0;
            }
        }
        if (l->combo == 0 && g_focus != w) {
            SetFocus(hwnd);
        } else if (l->combo != 0 && (w->style & WS_POPUP) == 0 && g_focus != l->combo &&
                   (g_focus == 0 || g_focus->parent != l->combo)) {
            /* A simple combo box takes the focus before the choice. */
            SetFocus(handle_of(l->combo));
        }
        if (window_of(hwnd) == 0) {
            return 0;
        }
        l->tracking = 1;
        SetCapture(hwnd);
        if (l->combo != 0 && (w->style & WS_POPUP) != 0) {
            /* The dropped list chooses when the button comes up. */
            const int at = item_at(w, l, GET_Y_LPARAM(lParam));
            if (at >= 0 && at < l->count) {
                select_item(w, l, at, 1);
            }
            return 0;
        }
        user_select(w, l, item_at(w, l, GET_Y_LPARAM(lParam)));
        return 0;
    case WM_MOUSEMOVE:
        if (l->tracking && g_capture == w) {
            const int y = GET_Y_LPARAM(lParam);
            if (y < 0) {
                set_top(w, l, l->top - 1);
            } else if (y >= client_height(w)) {
                set_top(w, l, l->top + 1);
            }
            user_select(w, l, item_at(w, l, y));
        } else if (l->combo != 0) {
            /* The dropped list follows the pointer without a button. */
            const int x = GET_X_LPARAM(lParam);
            const int y = GET_Y_LPARAM(lParam);
            if (x >= 0 && y >= 0 && x < client_width(w) && y < client_height(w)) {
                const int at = item_at(w, l, y);
                if (at != l->selected) {
                    select_item(w, l, at, 0);
                }
            }
        }
        return 0;
    case WM_LBUTTONUP:
        if (!l->tracking && l->combo != 0 && (w->style & WS_POPUP) != 0) {
            combo_list_event(l->combo, COMBO_LIST_RELEASED, l->selected);
            return 0;
        }
        if (l->tracking) {
            l->tracking = 0;
            if (g_capture == w) {
                ReleaseCapture();
            }
            if (l->combo != 0) {
                const int x = GET_X_LPARAM(lParam);
                const int y = GET_Y_LPARAM(lParam);
                if (x >= 0 && y >= 0 && x < client_width(w) && y < client_height(w)) {
                    combo_list_event(l->combo, COMBO_LIST_CLICKED, l->selected);
                }
            }
        }
        return 0;
    case WM_CAPTURECHANGED:
        l->tracking = 0;
        return 0;
    case WM_LBUTTONDBLCLK:
        if (l->combo != 0) {
            combo_list_event(l->combo, COMBO_LIST_CLICKED, l->selected);
            return 0;
        }
        notify(w, LBN_DBLCLK);
        return 0;
    case WM_KEYDOWN:
        key_down(w, l, wParam);
        return 0;
    case WM_CHAR:
        character(w, l, (WCHAR)wParam);
        return 0;
    case WM_VSCROLL:
        vscroll(w, l, wParam);
        return 0;
    case WM_CLOSE:
        /* A dropped list closed by a press outside the program. */
        if (l->combo != 0) {
            combo_list_event(l->combo, COMBO_LIST_CANCEL, l->selected);
            return 0;
        }
        break;
    case WM_MOUSEWHEEL:
        set_top(w, l, l->top - (short)HIWORD(wParam) / 120 * 3);
        return 0;
    case LB_CARETON:
    case LB_CARETOFF: {
        const int on = message == LB_CARETON;
        if (l->caret_on != on) {
            l->caret_on = on;
            if (visible_chain(w)) {
                HDC dc = GetDC(hwnd);
                focus_item(w, l, dc, on);
                ReleaseDC(hwnd, dc);
            }
        }
        return 0;
    }
    case LB_ADDSTRING:
        return insert(w, l, -1, lParam, (w->style & LBS_SORT) != 0 && (!owner_drawn(w) || has_strings(w) ||
                                                                       (w->style & LBS_SORT) != 0));
    case LB_INSERTSTRING:
        if ((int)wParam < -1 || (int)wParam > l->count) {
            return LB_ERR;
        }
        return insert(w, l, (int)wParam, lParam, 0);
    case LB_DELETESTRING:
        return remove_item(w, l, (int)wParam);
    case LB_RESETCONTENT:
        reset(w, l);
        return 0;
    case LB_GETCOUNT:
        return l->count;
    case LB_GETTEXT: {
        const int i = (int)wParam;
        if (i < 0 || i >= l->count) {
            return LB_ERR;
        }
        if (!has_strings(w)) {
            *(ULONG_PTR *)lParam = l->items[i].data;
            return sizeof(ULONG_PTR);
        }
        WCHAR *out = (WCHAR *)lParam;
        const WCHAR *t = l->items[i].text;
        int n = 0;
        while (t[n] != 0) {
            out[n] = t[n];
            ++n;
        }
        out[n] = 0;
        return n;
    }
    case LB_GETTEXTLEN: {
        const int i = (int)wParam;
        if (i < 0 || i >= l->count) {
            return LB_ERR;
        }
        return has_strings(w) ? text_length(l->items[i].text) : (LRESULT)sizeof(ULONG_PTR);
    }
    case LB_GETITEMDATA: {
        const int i = (int)wParam;
        return i >= 0 && i < l->count ? (LRESULT)l->items[i].data : LB_ERR;
    }
    case LB_SETITEMDATA: {
        const int i = (int)wParam;
        if (i < 0 || i >= l->count) {
            return LB_ERR;
        }
        l->items[i].data = (ULONG_PTR)lParam;
        return TRUE;
    }
    case LB_GETCURSEL:
        return l->selected;
    case LB_SETCURSEL: {
        const int i = (int)wParam;
        if (i < -1 || i >= l->count) {
            /* An index past the end leaves the selection as it was. */
            return LB_ERR;
        }
        select_item(w, l, i, 1);
        return i == -1 ? LB_ERR : i;
    }
    case LB_GETSEL: {
        const int i = (int)wParam;
        return i >= 0 && i < l->count ? i == l->selected : LB_ERR;
    }
    case LB_SETSEL:
        select_item(w, l, wParam ? (int)lParam : -1, 1);
        return 0;
    case LB_GETSELCOUNT:
    case LB_GETSELITEMS:
        return LB_ERR;
    case LB_FINDSTRING:
        return find(w, l, (int)wParam, (const WCHAR *)lParam, 0);
    case LB_FINDSTRINGEXACT:
        return find(w, l, (int)wParam, (const WCHAR *)lParam, 1);
    case LB_SELECTSTRING: {
        const int found = find(w, l, (int)wParam, (const WCHAR *)lParam, 0);
        if (found != LB_ERR) {
            select_item(w, l, found, 1);
        }
        return found;
    }
    case LB_GETTOPINDEX:
        return l->top;
    case LB_SETTOPINDEX:
        if ((int)wParam < 0 || (int)wParam >= l->count) {
            return LB_ERR;
        }
        set_top(w, l, (int)wParam);
        return 0;
    case LB_GETCARETINDEX:
        return l->caret;
    case LB_SETCARETINDEX:
        if ((int)wParam < 0 || (int)wParam >= l->count) {
            return LB_ERR;
        }
        l->caret = (int)wParam;
        show_item(w, l, l->caret);
        InvalidateRect(hwnd, NULL, TRUE);
        return 0;
    case LB_GETITEMHEIGHT:
        return l->item_height;
    case LB_SETITEMHEIGHT:
        if (LOWORD(lParam) == 0 || LOWORD(lParam) > 255) {
            return LB_ERR;
        }
        l->item_height = LOWORD(lParam);
        update_scroll(w, l);
        InvalidateRect(hwnd, NULL, TRUE);
        return 0;
    case LB_GETITEMRECT: {
        RECT *r = (RECT *)lParam;
        const int i = (int)wParam;
        if (r == NULL || i < 0 || (i >= l->count && !(i == 0 && l->count == 0))) {
            return LB_ERR;
        }
        r->left = 0;
        r->right = client_width(w);
        r->top = (i - l->top) * l->item_height;
        r->bottom = r->top + l->item_height;
        return 1;
    }
    case LB_ITEMFROMPOINT: {
        const int y = GET_Y_LPARAM(lParam);
        const int x = GET_X_LPARAM(lParam);
        const int i = l->count == 0 ? 0 : item_at(w, l, y);
        const int outside = x < 0 || y < 0 || x >= client_width(w) || y >= client_height(w) ||
                            l->top + y / (l->item_height > 0 ? l->item_height : 1) >= l->count;
        return MAKELPARAM(i, outside);
    }
    case LB_INITSTORAGE:
        return reserve(l, l->count + (int)wParam) ? l->capacity : LB_ERRSPACE;
    default:
        break;
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

window *l_parent(window *w) {
    list *l = list_of(w);
    if (l != 0 && l->combo != 0) {
        return l->combo->parent;
    }
    return w->parent;
}

void register_list_box(void) {
    static const WCHAR k_name[] = {'L', 'i', 's', 't', 'B', 'o', 'x', 0};
    static const WCHAR k_combo[] = {'C', 'o', 'm', 'b', 'o', 'L', 'B', 'o', 'x', 0};
    register_system_class(CS_DBLCLKS | CS_PARENTDC, list_proc, k_name, ATOM_LISTBOX, NULL, 4);
    register_system_class(CS_DBLCLKS | CS_SAVEBITS, list_proc, k_combo, ATOM_COMBOLBOX, NULL, 4);
}
