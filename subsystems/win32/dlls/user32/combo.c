/*
 * combo.c -- The ComboBox control (docs/userland/WIN32.md, "Edit, list box
 * and combo box"): simple, drop-down, drop-down list and owner-drawn, made
 * of an Edit control (the first two), a list box (a child of a simple one,
 * a popup of its own for the others) and a button, with the CB_ messages
 * and CBN_ notifications Windows XP answers and sends (fixtures/controls).
 *
 * The dropped list is a top-level popup window (SXW_WINDOW_POPUP on the
 * host) that never takes the keyboard: the combo box keeps the focus and
 * passes keys on, and the list holds the pointer while it is open, as on
 * Windows.
 */
#include "user32_private.h"

#define ATOM_COMBOBOX 0x8003
#define ATOM_COMBO 0x0085
#define ID_EDIT 1001
#define ID_LIST 1000
#define BUTTON_WIDTH 16
#define EDGE 2

typedef struct combo {
    window *edit;
    window *list;
    int dropped;
    int field_height; /* the selection field */
    int list_height;  /* the dropped list's height, from the height given at creation */
    int dropped_width;
    int pressed;      /* the button is held down */
    int focused;
    int extended_ui;
    int selection_at_drop;
    DWORD list_scroll; /* the WS_VSCROLL the combo box was made with, the list's */
} combo;

static combo *combo_of(window *w) {
    return (combo *)w->control;
}

static UINT kind(const window *w) {
    return (UINT)(w->style & 3u);
}

static int owner_drawn(const window *w) {
    return (w->style & (CBS_OWNERDRAWFIXED | CBS_OWNERDRAWVARIABLE)) != 0;
}

static void notify(window *w, WORD code) {
    if (w->parent != 0) {
        send(w->parent, WM_COMMAND, MAKELPARAM(w->id, code), (LPARAM)handle_of(w));
    }
}

static LRESULT to_list(combo *c, UINT message, WPARAM wParam, LPARAM lParam) {
    return c->list != 0 ? send(c->list, message, wParam, lParam) : CB_ERR;
}

/* ---- Geometry --------------------------------------------------------------------------------- */

static int font_height(window *w) {
    HDC dc = GetDC(NULL);
    if (w->font != NULL) {
        SelectObject(dc, w->font);
    }
    TEXTMETRICW tm;
    GetTextMetricsW(dc, &tm);
    ReleaseDC(NULL, dc);
    return tm.tmHeight;
}

static void button_rect(window *w, RECT *r) {
    r->right = client_width(w) - EDGE;
    r->left = r->right - BUTTON_WIDTH;
    r->top = EDGE;
    r->bottom = client_height(w) - EDGE;
}

/* The selection field: inside the edge and a pixel more, left of the
 * button, as high as CB_GETITEMHEIGHT(-1) says. */
static void field_rect(window *w, combo *c, RECT *r) {
    r->left = EDGE + 1;
    r->top = EDGE + 1;
    r->right = client_width(w) - EDGE - (kind(w) == CBS_SIMPLE ? 1 : BUTTON_WIDTH + 1);
    r->bottom = r->top + c->field_height;
}

/* The box around the field: the field, a pixel round it and the edge. */
static int box_height(const combo *c) {
    return c->field_height + 2 * EDGE + 2;
}

/* Where the children go: the field the height of the font and two pixels,
 * the combo box that high, the list below it. */
static void layout(window *w, combo *c) {
    const int full = box_height(c);
    if (kind(w) != CBS_SIMPLE && client_height(w) != full - 0) {
        set_window_pos(w, NULL, 0, 0, w->rect.right - w->rect.left, full,
                       SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOREDRAW);
    }
    RECT f;
    field_rect(w, c, &f);
    if (c->edit != 0) {
        if (kind(w) == CBS_SIMPLE) {
            set_window_pos(c->edit, NULL, f.left, f.top, f.right - f.left, c->field_height,
                           SWP_NOZORDER | SWP_NOACTIVATE);
        } else {
            set_window_pos(c->edit, NULL, f.left, f.top, f.right - f.left, f.bottom - f.top,
                           SWP_NOZORDER | SWP_NOACTIVATE);
        }
    }
    if (c->list != 0 && kind(w) == CBS_SIMPLE) {
        const int top = box_height(c);
        int height = client_height(w) - top;
        /* A whole number of items high, inside the list's sunken edge. */
        const int item = (int)to_list(c, LB_GETITEMHEIGHT, 0, 0);
        if ((w->style & CBS_NOINTEGRALHEIGHT) == 0 && item > 0 && height > 2 * EDGE + item) {
            height = (height - 2 * EDGE) / item * item + 2 * EDGE;
        }
        set_window_pos(c->list, NULL, 0, top, client_width(w), height > 0 ? height : 0,
                       SWP_NOZORDER | SWP_NOACTIVATE);
    }
}

/* ---- The field --------------------------------------------------------------------------------- */

static void field_text(window *w, combo *c, WCHAR *out, int capacity) {
    (void)w;
    out[0] = 0;
    if (c->list != 0) {
        list_box_text(c->list, list_box_selection(c->list), out, capacity);
    }
}

static void paint_field(window *w, combo *c, HDC dc) {
    RECT f;
    field_rect(w, c, &f);
    const int index = c->list != 0 ? list_box_selection(c->list) : -1;
    const int highlight = c->focused && !c->dropped;
    if (owner_drawn(w)) {
        DRAWITEMSTRUCT d;
        zero(&d, sizeof(d));
        d.CtlType = ODT_COMBOBOX;
        d.CtlID = (UINT)w->id;
        d.itemID = (UINT)index;
        d.itemAction = ODA_DRAWENTIRE;
        d.itemState = ODS_COMBOBOXEDIT | (highlight ? ODS_SELECTED | ODS_FOCUS : 0) |
                      ((w->style & WS_DISABLED) != 0 ? ODS_DISABLED : 0);
        d.hwndItem = handle_of(w);
        d.hDC = dc;
        d.rcItem = f;
        /* Asked even for no item, the field carries LB_ERR then. */
        d.itemData = (ULONG_PTR)to_list(c, LB_GETITEMDATA, (WPARAM)index, 0);
        if (w->parent != 0) {
            send(w->parent, WM_DRAWITEM, w->id, (LPARAM)&d);
        }
        return;
    }
    WCHAR text[MAX_TEXT];
    field_text(w, c, text, MAX_TEXT);
    if (highlight) {
        fill_rect(dc, &f, GetSysColor(COLOR_HIGHLIGHT));
        SetTextColor(dc, GetSysColor(COLOR_HIGHLIGHTTEXT));
    } else {
        SetTextColor(dc, GetSysColor((w->style & WS_DISABLED) != 0 ? COLOR_GRAYTEXT : COLOR_WINDOWTEXT));
    }
    /* Centred down the field, as Windows XP draws it (fixtures/controls). */
    const int room = (f.bottom - f.top) - font_height(w);
    TextOutW(dc, f.left + 1, f.top + (room > 0 ? room / 2 : 0), text, text_length(text));
    if (highlight && (top_of(w)->ui_state & UISF_HIDEFOCUS) == 0) {
        draw_focus_rect(dc, &f);
    }
}

static void paint(window *w, combo *c, HDC dc) {
    if (w->font != NULL) {
        SelectObject(dc, w->font);
    }
    SxwSetBkMode(dc, TRANSPARENT);
    const UINT message = kind(w) == CBS_DROPDOWNLIST ? WM_CTLCOLORLISTBOX : WM_CTLCOLOREDIT;
    HBRUSH brush = w->parent != 0 ? (HBRUSH)send(w->parent, message, (WPARAM)dc, (LPARAM)handle_of(w)) : NULL;
    if (brush == NULL) {
        brush = (HBRUSH)default_control_color(message, dc);
    }
    if (kind(w) == CBS_SIMPLE) {
        /* The box round the edit control, its inside the edit's colour. */
        RECT top = {0, 0, client_width(w), box_height(c)};
        DrawEdge(dc, &top, EDGE_SUNKEN, BF_RECT | BF_ADJUST);
        fill_brush(dc, &top, brush);
        return;
    }
    RECT all = {0, 0, client_width(w), client_height(w)};
    DrawEdge(dc, &all, EDGE_SUNKEN, BF_RECT | BF_ADJUST);
    fill_brush(dc, &all, brush);
    RECT b;
    button_rect(w, &b);
    if (c->pressed) {
        FrameRect(dc, &b, system_brush(COLOR_BTNSHADOW));
        RECT inner = b;
        InflateRect(&inner, -1, -1);
        fill_rect(dc, &inner, GetSysColor(COLOR_BTNFACE));
    } else {
        RECT edge = b;
        DrawEdge(dc, &edge, EDGE_RAISED, BF_RECT | BF_ADJUST | BF_MIDDLE);
    }
    draw_arrow(dc, &b, 1, (w->style & WS_DISABLED) != 0, c->pressed);
    if (c->edit == 0) {
        paint_field(w, c, dc);
    }
}

static void redraw(window *w) {
    InvalidateRect(handle_of(w), NULL, TRUE);
}

/* The edit control or the field shows the selected item. */
static void show_selection(window *w, combo *c) {
    if (c->edit != 0) {
        WCHAR text[MAX_TEXT];
        field_text(w, c, text, MAX_TEXT);
        const HWND h = handle_of(c->edit);
        c->edit->control_state |= 0x20000u; /* quiet: no CBN_EDIT* */
        SetWindowTextW(h, text);
        if (window_of(h) != 0) {
            c->edit->control_state &= ~0x20000u;
            if (c->focused) {
                send(c->edit, EM_SETSEL, 0, (LPARAM)-1);
            }
        }
    } else {
        redraw(w);
    }
}

/* ---- Dropping the list down ----------------------------------------------------------------------- */

/* The choice ended: CBN_SELENDOK or CBN_SELENDCANCEL whether the list was
 * down or not, then, when it was, the list hidden, the box drawn at once
 * and CBN_CLOSEUP, in Windows XP's order (fixtures/controls). */
static void close_up(window *w, combo *c, int ok) {
    const HWND h = handle_of(w);
    if (kind(w) == CBS_SIMPLE) {
        /* Its list never closes, and losing the focus ends nothing. */
        if (ok) {
            notify(w, CBN_SELENDOK);
        }
        return;
    }
    notify(w, ok ? CBN_SELENDOK : CBN_SELENDCANCEL);
    if (window_of(h) == 0 || !c->dropped) {
        return;
    }
    c->dropped = 0;
    c->pressed = 0;
    if (c->list != 0) {
        /* The list's focus goes off and, the box keeping the keyboard,
         * comes back on before the list hides. */
        send(c->list, LB_CARETOFF, 0, 0);
        if (c->focused && kind(w) == CBS_DROPDOWNLIST) {
            send(c->list, LB_CARETON, 0, 0);
        }
        if (g_capture == c->list) {
            ReleaseCapture();
        }
        ShowWindow(handle_of(c->list), SW_HIDE);
    }
    if (!ok && c->list != 0 && list_box_selection(c->list) != c->selection_at_drop) {
        send(c->list, LB_SETCURSEL, (WPARAM)c->selection_at_drop, 0);
        show_selection(w, c);
    }
    redraw(w);
    UpdateWindow(h);
    if (window_of(h) != 0) {
        notify(w, CBN_CLOSEUP);
    }
}

static void drop_down(window *w, combo *c) {
    if (c->dropped || c->list == 0 || kind(w) == CBS_SIMPLE) {
        return;
    }
    const HWND h = handle_of(w);
    notify(w, CBN_DROPDOWN);
    if (window_of(h) == 0) {
        return;
    }
    c->dropped = 1;
    c->selection_at_drop = list_box_selection(c->list);
    if (kind(w) == CBS_DROPDOWNLIST) {
        send(c->list, LB_CARETON, 0, 0);
    }
    int x;
    int y;
    client_screen_origin(w, &x, &y);
    const int width = c->dropped_width > w->rect.right - w->rect.left ? c->dropped_width
                                                                       : w->rect.right - w->rect.left;
    const int item = (int)to_list(c, LB_GETITEMHEIGHT, 0, 0);
    const int count = (int)to_list(c, LB_GETCOUNT, 0, 0);
    int height = c->list_height;
    if (count > 0 && item > 0 && count * item + 2 < height) {
        height = count * item + 2;
    }
    if (height < item + 2) {
        height = item + 2;
    }
    if ((w->style & CBS_NOINTEGRALHEIGHT) == 0 && item > 0) {
        height = (height - 2) / item * item + 2;
    }
    set_window_pos(c->list, HWND_TOP, x - (w->client.left - w->rect.left), y + client_height(w), width, height,
                   SWP_NOACTIVATE);
    const int selected = list_box_selection(c->list);
    if (selected >= 0) {
        send(c->list, LB_SETTOPINDEX, (WPARAM)selected, 0);
    }
    ShowWindow(handle_of(c->list), SW_SHOWNA);
    if (c->edit != 0 && c->focused) {
        send(c->edit, EM_SETSEL, 0, (LPARAM)-1);
    }
    redraw(w);
    UpdateWindow(h);
    if (window_of(h) == 0 || c->list == 0) {
        return;
    }
    SetCapture(handle_of(c->list));
}

/* What the list box inside tells: an item chosen, the selection moved, or
 * the list given up. */
void combo_list_event(window *w, int event, int index) {
    combo *c = combo_of(w);
    if (c == 0) {
        return;
    }
    const HWND h = handle_of(w);
    (void)index;
    switch (event) {
    case COMBO_LIST_SELECTED:
        /* Moved in the dropped list, the choice is not made yet; anywhere
         * else it is, and said first. */
        if (!c->dropped) {
            close_up(w, c, 1);
            if (window_of(h) == 0) {
                break;
            }
        }
        notify(w, CBN_SELCHANGE);
        if (window_of(h) != 0) {
            show_selection(w, c);
        }
        break;
    case COMBO_LIST_CLICKED:
        close_up(w, c, 1);
        if (window_of(h) == 0) {
            break;
        }
        notify(w, CBN_SELCHANGE);
        if (window_of(h) != 0) {
            show_selection(w, c);
        }
        break;
    case COMBO_LIST_CHOSEN:
        close_up(w, c, 1);
        break;
    case COMBO_LIST_RELEASED:
        /* The press that dropped the list came up: the button is drawn
         * raised again, and the field with it. */
        if (c->pressed) {
            c->pressed = 0;
            redraw(w);
            UpdateWindow(h);
        }
        break;
    case COMBO_LIST_CANCEL:
        close_up(w, c, 0);
        break;
    default:
        break;
    }
}

/* What is typed finds its item in the list: chosen in a dropped list, the
 * selection gone in a closed one, and either way the list scrolled to it,
 * or to the top when nothing matches (fixtures/controls). */
static void follow_edit(combo *c) {
    if (c->list == 0 || c->edit == 0) {
        return;
    }
    WCHAR text[MAX_TEXT];
    GetWindowTextW(handle_of(c->edit), text, MAX_TEXT);
    const int found = text[0] != 0 ? (int)to_list(c, LB_FINDSTRING, (WPARAM)-1, (LPARAM)text) : LB_ERR;
    to_list(c, LB_SETCURSEL, (WPARAM)(c->dropped ? found : -1), 0);
    to_list(c, LB_SETCARETINDEX, (WPARAM)(found < 0 ? 0 : found), 0);
    to_list(c, LB_SETTOPINDEX, (WPARAM)(found < 0 ? 0 : found), 0);
}

/* ---- Keys -------------------------------------------------------------------------------------------- */

/* Up, Down and the rest move the selection, through the list when it is
 * dropped; F4 and Alt with an arrow drop it or close it. 1 when taken. */
static int key(window *w, combo *c, WPARAM vk) {
    const int alt = GetKeyState(VK_MENU) < 0;
    if (vk == VK_F4 || (alt && (vk == VK_DOWN || vk == VK_UP))) {
        if (c->dropped) {
            close_up(w, c, 1);
        } else {
            drop_down(w, c);
        }
        return 1;
    }
    if (c->dropped && (vk == VK_RETURN || vk == VK_ESCAPE)) {
        close_up(w, c, vk == VK_RETURN);
        return 1;
    }
    switch (vk) {
    case VK_UP:
    case VK_DOWN:
    case VK_PRIOR:
    case VK_NEXT:
    case VK_HOME:
    case VK_END:
        if (c->list != 0) {
            if (!c->dropped && c->edit != 0 && (vk == VK_HOME || vk == VK_END)) {
                return 0;
            }
            /* The list tells what moved (combo_list_event). */
            send(c->list, WM_KEYDOWN, vk, 0);
        }
        return 1;
    default:
        return 0;
    }
}

/* ---- The window procedure ------------------------------------------------------------------------- */

static void make_children(window *w, combo *c) {
    static const WCHAR k_edit[] = {'E', 'd', 'i', 't', 0};
    static const WCHAR k_list[] = {'C', 'o', 'm', 'b', 'o', 'L', 'B', 'o', 'x', 0};
    static const WCHAR k_empty[] = {0};
    const HWND h = handle_of(w);
    DWORD list_style = LBS_COMBOBOX | LBS_NOTIFY | WS_CLIPSIBLINGS | c->list_scroll |
                       ((w->style & CBS_SORT) != 0 ? LBS_SORT : 0) |
                       ((w->style & CBS_HASSTRINGS) != 0 ? LBS_HASSTRINGS : 0) |
                       ((w->style & CBS_OWNERDRAWFIXED) != 0 ? LBS_OWNERDRAWFIXED : 0) |
                       ((w->style & CBS_OWNERDRAWVARIABLE) != 0 ? LBS_OWNERDRAWVARIABLE : 0) |
                       ((w->style & CBS_DISABLENOSCROLL) != 0 ? LBS_DISABLENOSCROLL : 0) |
                       ((w->style & CBS_NOINTEGRALHEIGHT) != 0 ? LBS_NOINTEGRALHEIGHT : 0);
    if (kind(w) == CBS_SIMPLE) {
        HWND list = CreateWindowExW(WS_EX_CLIENTEDGE, k_list, k_empty, list_style | WS_CHILD | WS_VISIBLE, 0, 0, 0,
                                    0, h, (HMENU)ID_LIST, w->instance, (LPVOID)h);
        c->list = window_of(list);
    } else {
        HWND list = CreateWindowExW(WS_EX_TOOLWINDOW, k_list, k_empty, list_style | WS_POPUP | WS_BORDER, 0, 0, 0,
                                    0, handle_of(top_of(w)), NULL, w->instance, (LPVOID)h);
        c->list = window_of(list);
    }
    if (kind(w) != CBS_DROPDOWNLIST) {
        const DWORD edit_style = WS_CHILD | WS_VISIBLE | ES_LEFT | ES_NOHIDESEL | ES_COMBO |
                                 ((w->style & CBS_AUTOHSCROLL) != 0 ? ES_AUTOHSCROLL : 0) |
                                 ((w->style & CBS_UPPERCASE) != 0 ? ES_UPPERCASE : 0) |
                                 ((w->style & CBS_LOWERCASE) != 0 ? ES_LOWERCASE : 0);
        HWND edit = CreateWindowExW(0, k_edit, k_empty, edit_style, 0, 0, 0, 0, h, (HMENU)ID_EDIT, w->instance,
                                    NULL);
        c->edit = window_of(edit);
    }
}

/* The field's height: the font's and two pixels, or what the owner says,
 * offered the same. */
static void measure(window *w, combo *c) {
    c->field_height = font_height(w) + 2;
    if (owner_drawn(w) && w->parent != 0) {
        MEASUREITEMSTRUCT mi;
        zero(&mi, sizeof(mi));
        mi.CtlType = ODT_COMBOBOX;
        mi.CtlID = (UINT)w->id;
        mi.itemID = (UINT)-1;
        mi.itemHeight = (UINT)c->field_height;
        send(w->parent, WM_MEASUREITEM, w->id, (LPARAM)&mi);
        c->field_height = (int)mi.itemHeight;
        if ((w->style & CBS_OWNERDRAWFIXED) != 0 && c->list != 0) {
            zero(&mi, sizeof(mi));
            mi.CtlType = ODT_COMBOBOX;
            mi.CtlID = (UINT)w->id;
            mi.itemID = 0;
            mi.itemHeight = (UINT)font_height(w);
            send(w->parent, WM_MEASUREITEM, w->id, (LPARAM)&mi);
            send(c->list, LB_SETITEMHEIGHT, 0, (LPARAM)mi.itemHeight);
        }
    }
}

static LRESULT WINAPI combo_proc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    window *w = window_of(hwnd);
    if (w == 0) {
        return 0;
    }
    combo *c = combo_of(w);
    if (c == 0 && message != WM_NCCREATE) {
        return DefWindowProcW(hwnd, message, wParam, lParam);
    }
    switch (message) {
    case WM_NCCREATE:
        c = SxwHeapAllocate(sizeof(combo), 1);
        if (c == 0) {
            return FALSE;
        }
        w->control = c;
        if (kind(w) == 0) {
            w->style |= CBS_SIMPLE;
        }
        /* The scroll bar is the list's, not the combo box's own; a combo
         * box without owner drawing holds strings (fixtures/controls). */
        c->list_scroll = w->style & WS_VSCROLL;
        w->style &= ~(WS_VSCROLL | WS_HSCROLL);
        if (!owner_drawn(w)) {
            w->style |= CBS_HASSTRINGS;
        }
        return DefWindowProcW(hwnd, message, wParam, lParam);
    case WM_CREATE: {
        const CREATESTRUCTW *cs = (const CREATESTRUCTW *)lParam;
        c->list_height = cs != NULL ? cs->cy : 100;
        make_children(w, c);
        measure(w, c);
        if (c->list != 0) {
            const int rest = c->list_height - box_height(c);
            c->list_height = rest > 0 ? rest : 0;
        }
        layout(w, c);
        return 0;
    }
    case WM_NCDESTROY:
        if (c->list != 0 && kind(w) != CBS_SIMPLE && c->list->used) {
            DestroyWindow(handle_of(c->list));
        }
        SxwHeapFree(c);
        w->control = 0;
        return DefWindowProcW(hwnd, message, wParam, lParam);
    case WM_SETFONT:
        w->font = (HFONT)wParam;
        if (c->edit != 0) {
            send(c->edit, WM_SETFONT, wParam, lParam);
        }
        if (c->list != 0) {
            send(c->list, WM_SETFONT, wParam, lParam);
        }
        if (!owner_drawn(w)) {
            c->field_height = font_height(w) + 2;
        }
        layout(w, c);
        if (lParam != 0) {
            redraw(w);
        }
        return 0;
    case WM_GETFONT:
        return (LRESULT)w->font;
    case WM_SIZE:
        if (c->edit != 0 || c->list != 0) {
            layout(w, c);
        }
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        paint(w, c, dc);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_ENABLE:
        if (c->edit != 0) {
            EnableWindow(handle_of(c->edit), (BOOL)wParam);
        }
        redraw(w);
        return 0;
    case WM_GETDLGCODE: {
        LRESULT code = DLGC_WANTARROWS | DLGC_WANTCHARS;
        if (c->dropped) {
            code |= DLGC_WANTMESSAGE;
        }
        return code;
    }
    case WM_SETFOCUS:
        if (c->edit != 0) {
            SetFocus(handle_of(c->edit));
            return 0;
        }
        if (!c->focused) {
            c->focused = 1;
            if (kind(w) == CBS_DROPDOWNLIST && c->list != 0) {
                send(c->list, LB_CARETON, 0, 0);
            }
            /* The field shows the focus before the parent hears of it. */
            redraw(w);
            UpdateWindow(hwnd);
            if (window_of(hwnd) != 0) {
                notify(w, CBN_SETFOCUS);
            }
        }
        return 0;
    case WM_KILLFOCUS:
        if (c->edit == 0 && c->focused) {
            close_up(w, c, 0);
            if (window_of(hwnd) == 0) {
                return 0;
            }
            if (c->list != 0) {
                send(c->list, LB_CARETOFF, 0, 0);
            }
            c->focused = 0;
            redraw(w);
            notify(w, CBN_KILLFOCUS);
        }
        return 0;
    case WM_COMMAND:
        /* From the edit control inside. */
        if (c->edit != 0 && (HWND)lParam == handle_of(c->edit)) {
            switch (HIWORD(wParam)) {
            case EN_SETFOCUS:
                if (!c->focused) {
                    c->focused = 1;
                    send(c->edit, EM_SETSEL, 0, (LPARAM)-1);
                    notify(w, CBN_SETFOCUS);
                }
                break;
            case EN_KILLFOCUS:
                if (c->focused) {
                    close_up(w, c, 0);
                    if (window_of(hwnd) == 0) {
                        break;
                    }
                    c->focused = 0;
                    notify(w, CBN_KILLFOCUS);
                }
                break;
            case EN_CHANGE:
                if ((c->edit->control_state & 0x20000u) == 0) {
                    follow_edit(c);
                    notify(w, CBN_EDITCHANGE);
                }
                break;
            case EN_UPDATE:
                if ((c->edit->control_state & 0x20000u) == 0) {
                    notify(w, CBN_EDITUPDATE);
                }
                break;
            default:
                break;
            }
        }
        return 0;
    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK: {
        RECT b;
        button_rect(w, &b);
        const int x = GET_X_LPARAM(lParam);
        const int y = GET_Y_LPARAM(lParam);
        const int on_button = x >= b.left && x < b.right && y >= b.top && y < b.bottom;
        if (g_focus == 0 || (g_focus != w && g_focus != c->edit)) {
            SetFocus(hwnd);
        }
        if (kind(w) == CBS_SIMPLE) {
            return 0;
        }
        if (c->dropped) {
            close_up(w, c, 1);
        } else if (on_button || kind(w) == CBS_DROPDOWNLIST) {
            c->pressed = on_button || kind(w) == CBS_DROPDOWNLIST;
            drop_down(w, c);
        }
        return 0;
    }
    case WM_KEYDOWN:
        if (key(w, c, wParam)) {
            return 0;
        }
        break;
    case WM_SYSKEYDOWN:
        if (key(w, c, wParam)) {
            return 0;
        }
        break;
    case WM_CHAR:
        if (c->edit == 0 && c->list != 0 && wParam >= 0x20) {
            send(c->list, WM_CHAR, wParam, lParam);
        }
        return 0;
    case WM_SETTEXT:
        /* The edit control takes it without a CBN_EDIT notification; a
         * drop-down list keeps its selection (fixtures/controls). */
        if (c->edit != 0) {
            c->edit->control_state |= 0x20000u;
            const LRESULT result = send(c->edit, WM_SETTEXT, wParam, lParam);
            if (window_of(handle_of(c->edit)) != 0) {
                c->edit->control_state &= ~0x20000u;
            }
            return result;
        }
        return CB_ERR;
    case WM_GETTEXT:
        if (c->edit != 0) {
            return send(c->edit, WM_GETTEXT, wParam, lParam);
        }
        if (wParam == 0 || lParam == 0) {
            return 0;
        }
        {
            WCHAR text[MAX_TEXT];
            field_text(w, c, text, MAX_TEXT);
            return copy_text((WCHAR *)lParam, text, (int)wParam);
        }
    case WM_GETTEXTLENGTH:
        if (c->edit != 0) {
            return send(c->edit, WM_GETTEXTLENGTH, 0, 0);
        }
        {
            WCHAR text[MAX_TEXT];
            field_text(w, c, text, MAX_TEXT);
            return text_length(text);
        }
    case WM_CUT:
    case WM_COPY:
    case WM_PASTE:
    case WM_CLEAR:
        return c->edit != 0 ? send(c->edit, message, wParam, lParam) : 0;
    case CB_ADDSTRING:
        return to_list(c, LB_ADDSTRING, wParam, lParam);
    case CB_INSERTSTRING:
        return to_list(c, LB_INSERTSTRING, wParam, lParam);
    case CB_DELETESTRING:
        return to_list(c, LB_DELETESTRING, wParam, lParam);
    case CB_RESETCONTENT:
        to_list(c, LB_RESETCONTENT, 0, 0);
        if (c->edit != 0) {
            c->edit->control_state |= 0x20000u;
            SetWindowTextW(handle_of(c->edit), NULL);
            c->edit->control_state &= ~0x20000u;
        }
        redraw(w);
        return CB_OKAY;
    case CB_GETCOUNT:
        return to_list(c, LB_GETCOUNT, 0, 0);
    case CB_GETCURSEL:
        return to_list(c, LB_GETCURSEL, 0, 0);
    case CB_SETCURSEL: {
        /* An index past the end changes nothing (fixtures/controls). */
        const LRESULT result = to_list(c, LB_SETCURSEL, wParam, 0);
        if (result != LB_ERR || (int)wParam == -1) {
            if (result >= 0) {
                to_list(c, LB_SETTOPINDEX, (WPARAM)result, 0);
            }
            show_selection(w, c);
        }
        return result;
    }
    case CB_GETLBTEXT:
        return to_list(c, LB_GETTEXT, wParam, lParam);
    case CB_GETLBTEXTLEN:
        return to_list(c, LB_GETTEXTLEN, wParam, 0);
    case CB_GETITEMDATA:
        return to_list(c, LB_GETITEMDATA, wParam, 0);
    case CB_SETITEMDATA:
        return to_list(c, LB_SETITEMDATA, wParam, lParam);
    case CB_FINDSTRING:
        return to_list(c, LB_FINDSTRING, wParam, lParam);
    case CB_FINDSTRINGEXACT:
        return to_list(c, LB_FINDSTRINGEXACT, wParam, lParam);
    case CB_SELECTSTRING: {
        const LRESULT found = to_list(c, LB_FINDSTRING, wParam, lParam);
        if (found != LB_ERR) {
            to_list(c, LB_SETCURSEL, (WPARAM)found, 0);
            show_selection(w, c);
        }
        return found;
    }
    case CB_SHOWDROPDOWN:
        if (wParam) {
            drop_down(w, c);
        } else {
            close_up(w, c, 0);
        }
        return TRUE;
    case CB_GETDROPPEDSTATE:
        /* A simple combo box's list is always down (fixtures/controls). */
        return kind(w) == CBS_SIMPLE ? TRUE : c->dropped;
    case CB_GETEDITSEL:
        if (c->edit == 0) {
            return CB_ERR;
        }
        return send(c->edit, EM_GETSEL, wParam, lParam);
    case CB_SETEDITSEL:
        if (c->edit == 0) {
            return CB_ERR;
        }
        send(c->edit, EM_SETSEL, (WPARAM)(short)LOWORD(lParam), (LPARAM)(short)HIWORD(lParam));
        return TRUE;
    case CB_LIMITTEXT:
        if (c->edit == 0) {
            return CB_ERR;
        }
        send(c->edit, EM_LIMITTEXT, wParam, 0);
        return TRUE;
    case CB_GETITEMHEIGHT:
        if ((int)wParam == -1) {
            return c->field_height;
        }
        return to_list(c, LB_GETITEMHEIGHT, wParam, 0);
    case CB_SETITEMHEIGHT:
        if ((int)wParam == -1) {
            c->field_height = (int)lParam;
            layout(w, c);
            redraw(w);
            return CB_OKAY;
        }
        return to_list(c, LB_SETITEMHEIGHT, wParam, lParam);
    case CB_GETDROPPEDCONTROLRECT: {
        RECT *r = (RECT *)lParam;
        if (r == NULL) {
            return CB_ERR;
        }
        /* The box and the list's height, which was the height given at
         * creation less the box then (fixtures/controls). */
        GetWindowRect(hwnd, r);
        r->bottom = r->top + box_height(c) + c->list_height;
        return CB_OKAY;
    }
    case CB_SETEXTENDEDUI:
        c->extended_ui = wParam != 0;
        return CB_OKAY;
    case CB_GETEXTENDEDUI:
        return c->extended_ui;
    case CB_GETTOPINDEX:
        return to_list(c, LB_GETTOPINDEX, 0, 0);
    case CB_SETTOPINDEX:
        return to_list(c, LB_SETTOPINDEX, wParam, 0) == LB_ERR ? CB_ERR : 0;
    case CB_GETDROPPEDWIDTH:
        return c->dropped_width > w->rect.right - w->rect.left ? c->dropped_width : w->rect.right - w->rect.left;
    case CB_SETDROPPEDWIDTH:
        c->dropped_width = (int)wParam;
        return c->dropped_width;
    case CB_INITSTORAGE:
        return to_list(c, LB_INITSTORAGE, wParam, lParam);
    default:
        break;
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

void register_combo_box(void) {
    static const WCHAR k_name[] = {'C', 'o', 'm', 'b', 'o', 'B', 'o', 'x', 0};
    register_system_class(CS_VREDRAW | CS_HREDRAW | CS_DBLCLKS | CS_PARENTDC, combo_proc, k_name, ATOM_COMBO, NULL,
                          4);
}
