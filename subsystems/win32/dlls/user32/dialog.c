/*
 * dialog.c -- Dialogs (docs/userland/WIN32.md, "Dialogs and controls"):
 * dialog templates, DialogBoxParamW and its modal loop, the dialog manager's
 * window procedure and keyboard interface, the functions that work on a
 * dialog's items, and MessageBoxW, which is a dialog of its own.
 *
 * A dialog is a top-level window owned by the window that asked for it; on
 * SavanXP the host places it (SXW_WINDOW_OWNED). Its size, and its
 * controls', come from the template in dialog units of its font. The
 * messages, and their order, are Windows XP's, as fixtures/dialog recorded
 * them there.
 */
#include "user32_private.h"

#define MSGF_DIALOGBOX 0
#define MAX_TEMPLATE 4096

struct dialog_info {
    int used;
    DLGPROC proc;
    INT_PTR result;
    int ended;
    window *owner;      /* disabled while the dialog is up */
    int owner_disabled; /* by this dialog */
    window *focus;      /* the control that had the focus when it was deactivated */
    UINT default_id;    /* DM_SETDEFID's, 0 for none */
    HFONT font;
    int made_font;
};

static dialog_info g_infos[16];

static dialog_info *new_info(void) {
    for (int i = 0; i < 16; ++i) {
        if (!g_infos[i].used) {
            zero(&g_infos[i], sizeof(g_infos[i]));
            g_infos[i].used = 1;
            return &g_infos[i];
        }
    }
    return 0;
}

static void free_info(dialog_info *d) {
    if (d->made_font && d->font != NULL) {
        DeleteObject(d->font);
    }
    zero(d, sizeof(*d));
}

void dialog_free(window *w) {
    if (w->dialog != 0) {
        free_info(w->dialog);
        w->dialog = 0;
    }
}

/* ---- Items ------------------------------------------------------------------------------- */

static window *item_of(window *dlg, int id) {
    for (window *c = dlg->child; c != 0; c = c->next) {
        if ((int)(u32)c->id == id || (int)(short)c->id == id) {
            return c;
        }
    }
    return 0;
}

HWND WINAPI GetDlgItem(HWND hDlg, int nIDDlgItem) {
    window *dlg = checked(hDlg);
    if (dlg == 0) {
        return NULL;
    }
    window *c = item_of(dlg, nIDDlgItem);
    if (c == 0) {
        set_last_error(ERROR_CONTROL_ID_NOT_FOUND);
    }
    return handle_of(c);
}

BOOL WINAPI CheckDlgButton(HWND hDlg, int nIDButton, UINT uCheck) {
    window *dlg = checked(hDlg);
    window *c = dlg != 0 ? item_of(dlg, nIDButton) : 0;
    if (c == 0) {
        return FALSE;
    }
    send(c, BM_SETCHECK, uCheck, 0);
    return TRUE;
}

UINT WINAPI IsDlgButtonChecked(HWND hDlg, int nIDButton) {
    window *dlg = checked(hDlg);
    window *c = dlg != 0 ? item_of(dlg, nIDButton) : 0;
    return c != 0 ? (UINT)send(c, BM_GETCHECK, 0, 0) : 0;
}

/* Checks one button of the range and unchecks the others, by id. */
BOOL WINAPI CheckRadioButton(HWND hDlg, int nIDFirstButton, int nIDLastButton, int nIDCheckButton) {
    window *dlg = checked(hDlg);
    if (dlg == 0) {
        return FALSE;
    }
    for (int id = nIDFirstButton; id <= nIDLastButton; ++id) {
        window *c = item_of(dlg, id);
        if (c != 0) {
            send(c, BM_SETCHECK, id == nIDCheckButton, 0);
        }
    }
    return TRUE;
}

static int usable(const window *c) {
    return (c->style & (WS_VISIBLE | WS_DISABLED)) == WS_VISIBLE;
}

/* The control after (or before) from with WS_TABSTOP, in the order of the
 * template; from itself when there is no other. */
static window *next_tab_item(window *dlg, window *from, int previous) {
    window *list[128];
    int n = 0;
    int at = -1;
    for (window *c = dlg->child; c != 0 && n < 128; c = c->next) {
        if (c == from) {
            at = n;
        }
        list[n++] = c;
    }
    if (n == 0) {
        return 0;
    }
    for (int k = 1; k <= n; ++k) {
        const int i = at < 0 ? (previous ? n - k : k - 1) : ((at + (previous ? -k : k)) % n + n) % n;
        if ((list[i]->style & WS_TABSTOP) != 0 && usable(list[i])) {
            return list[i];
        }
    }
    return from;
}

/* The next control (or previous) of from's group, the controls from one
 * with WS_GROUP up to the next, wrapping. */
static window *next_group_item(window *dlg, window *from, int previous) {
    window *list[128];
    int n = 0;
    int at = -1;
    for (window *c = dlg->child; c != 0 && n < 128; c = c->next) {
        if (c == from) {
            at = n;
        }
        list[n++] = c;
    }
    if (at < 0) {
        return from;
    }
    int first = at;
    while (first > 0 && (list[first]->style & WS_GROUP) == 0) {
        --first;
    }
    int last = at + 1;
    while (last < n && (list[last]->style & WS_GROUP) == 0) {
        ++last;
    }
    const int size = last - first;
    for (int k = 1; k < size; ++k) {
        const int i = first + ((at - first + (previous ? -k : k)) % size + size) % size;
        if (usable(list[i])) {
            return list[i];
        }
    }
    return from;
}

static UINT dlg_code(window *c, WPARAM key, const MSG *msg) {
    return c != 0 ? (UINT)send(c, WM_GETDLGCODE, key, (LPARAM)msg) : 0;
}

static window *default_button(window *dlg) {
    for (window *c = dlg->child; c != 0; c = c->next) {
        if ((dlg_code(c, 0, 0) & DLGC_DEFPUSHBUTTON) != 0) {
            return c;
        }
    }
    return 0;
}

/* The focus moves from old to control: a push button becomes the default
 * one while it has the focus, and otherwise the dialog's default is restored.
 * The dialog asks itself for its default (DM_GETDEFID) unless the focus goes
 * from one push button to another, or the pointer puts it on a push button;
 * the button that had the focus is redrawn. Windows XP's order. */
void dialog_check_default(window *dlg, window *old, window *control, int by_pointer) {
    if (control == 0 || dlg == 0 || dlg->dialog == 0 || old == control) {
        return;
    }
    const UINT code = dlg_code(control, 0, 0);
    const UINT old_code = old != 0 && old->used && within(old, dlg) && old != dlg ? dlg_code(old, 0, 0) : 0;
    const int push = (code & (DLGC_DEFPUSHBUTTON | DLGC_UNDEFPUSHBUTTON)) != 0;
    const int old_push = (old_code & (DLGC_DEFPUSHBUTTON | DLGC_UNDEFPUSHBUTTON)) != 0;
    window *wanted = control;
    if (by_pointer ? !push : !(push && old_push)) {
        const LRESULT id = send(dlg, DM_GETDEFID, 0, 0);
        if (!push) {
            wanted = HIWORD(id) == DC_HASDEFID ? item_of(dlg, LOWORD(id)) : 0;
        }
    }
    window *current = default_button(dlg);
    if (current != wanted) {
        if (current != 0) {
            send(current, BM_SETSTYLE, BS_PUSHBUTTON, TRUE);
        }
        if (wanted != 0 && (dlg_code(wanted, 0, 0) & DLGC_UNDEFPUSHBUTTON) != 0) {
            send(wanted, BM_SETSTYLE, BS_DEFPUSHBUTTON, TRUE);
        }
    }
    if (old_code != 0 && (old_code & DLGC_BUTTON) != 0 && !old_push && old->used) {
        InvalidateRect(handle_of(old), NULL, TRUE);
    }
}

static void set_dialog_focus(window *dlg, window *control) {
    if (control == 0) {
        return;
    }
    window *old = g_focus;
    SetFocus(handle_of(control));
    dialog_check_default(dlg, old, control, 0);
}

/* ---- The dialog manager's window procedure ------------------------------------------------- */

static void restore_focus(window *w) {
    dialog_info *d = w->dialog;
    if (d == 0 || d->ended || (w->style & WS_MINIMIZE) != 0) {
        return;
    }
    window *focus = d->focus != 0 && d->focus->used && within(d->focus, w) ? d->focus : next_tab_item(w, 0, 0);
    if (focus != 0) {
        set_dialog_focus(w, focus);
    }
}

LRESULT WINAPI DefDlgProcW(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    window *w = window_of(hwnd);
    if (w == 0) {
        return 0;
    }
    dialog_info *d = w->dialog;
    if (d != 0 && d->proc != NULL) {
        const INT_PTR result = d->proc(hwnd, message, wParam, lParam);
        if (result != 0 && window_of(hwnd) != 0) {
            switch (message) {
            case WM_CTLCOLORMSGBOX:
            case WM_CTLCOLOREDIT:
            case WM_CTLCOLORLISTBOX:
            case WM_CTLCOLORBTN:
            case WM_CTLCOLORDLG:
            case WM_CTLCOLORSCROLLBAR:
            case WM_CTLCOLORSTATIC:
            case WM_INITDIALOG:
                return result;
            default:
                return 0;
            }
        }
        if (window_of(hwnd) == 0) {
            return 0;
        }
    }
    switch (message) {
    case WM_ERASEBKGND: {
        HBRUSH brush = (HBRUSH)send(w, WM_CTLCOLORDLG, wParam, (LPARAM)hwnd);
        if (brush == NULL) {
            brush = (HBRUSH)default_control_color(WM_CTLCOLORDLG, (HDC)wParam);
        }
        RECT r = {0, 0, client_width(w), client_height(w)};
        fill_brush((HDC)wParam, &r, brush);
        return 1;
    }
    case WM_SETFONT:
        if (d != 0) {
            d->font = (HFONT)wParam;
        }
        return 0;
    case WM_GETFONT:
        return d != 0 ? (LRESULT)d->font : 0;
    case WM_ACTIVATE:
        if (LOWORD(wParam) != WA_INACTIVE) {
            restore_focus(w);
        } else if (d != 0 && g_focus != 0 && g_focus != w && within(g_focus, w)) {
            d->focus = g_focus;
        }
        return 0;
    case WM_SETFOCUS:
        restore_focus(w);
        return 0;
    case WM_CLOSE: {
        window *cancel = item_of(w, IDCANCEL);
        if (cancel == 0 || (cancel->style & WS_DISABLED) == 0) {
            PostMessageW(hwnd, WM_COMMAND, MAKELPARAM(IDCANCEL, BN_CLICKED), (LPARAM)handle_of(cancel));
        }
        return 0;
    }
    case DM_GETDEFID: {
        if (d == 0 || d->ended) {
            return 0;
        }
        if (d->default_id != 0) {
            return MAKELPARAM(d->default_id, DC_HASDEFID);
        }
        window *b = default_button(w);
        return b != 0 ? MAKELPARAM(b->id, DC_HASDEFID) : 0;
    }
    case DM_SETDEFID:
        if (d != 0) {
            d->default_id = (UINT)wParam;
        }
        return TRUE;
    case WM_NEXTDLGCTL: {
        window *to = lParam != 0 ? window_of((HWND)wParam) : next_tab_item(w, g_focus, wParam != 0);
        if (to != 0) {
            set_dialog_focus(w, to);
        }
        return 0;
    }
    case WM_CTLCOLORMSGBOX:
    case WM_CTLCOLORBTN:
    case WM_CTLCOLORDLG:
    case WM_CTLCOLORSTATIC:
        return default_control_color(message, (HDC)wParam);
    case WM_NCDESTROY:
        DefWindowProcW(hwnd, message, wParam, lParam);
        dialog_free(w);
        return 0;
    default:
        return DefWindowProcW(hwnd, message, wParam, lParam);
    }
}

/* ---- Keyboard ---------------------------------------------------------------------------- */

/* The control whose mnemonic c is, from the one after the focus. */
static window *mnemonic_control(window *dlg, WCHAR c) {
    if (c >= 'a' && c <= 'z') {
        c = (WCHAR)(c - 32);
    }
    window *list[128];
    int n = 0;
    int at = -1;
    for (window *k = dlg->child; k != 0 && n < 128; k = k->next) {
        if (k == g_focus) {
            at = n;
        }
        list[n++] = k;
    }
    for (int step = 1; step <= n; ++step) {
        window *k = list[(at + step) % n];
        if ((k->style & WS_VISIBLE) == 0 || (k->style & WS_DISABLED) != 0) {
            continue;
        }
        for (int i = 0; k->text[i] != 0 && k->text[i + 1] != 0; ++i) {
            if (k->text[i] == '&') {
                WCHAR m = k->text[i + 1];
                if (m == '&') {
                    ++i;
                    continue;
                }
                if (m >= 'a' && m <= 'z') {
                    m = (WCHAR)(m - 32);
                }
                if (m == c) {
                    return k;
                }
                break;
            }
        }
    }
    return 0;
}

static int accelerator(window *dlg, WCHAR c) {
    window *control = mnemonic_control(dlg, c);
    if (control == 0) {
        return 0;
    }
    const UINT code = dlg_code(control, 0, 0);
    if ((code & DLGC_STATIC) != 0 || (control->style & BS_TYPEMASK) == BS_GROUPBOX) {
        window *next = control->next;
        while (next != 0 && !usable(next)) {
            next = next->next;
        }
        if (next != 0) {
            send(dlg, WM_NEXTDLGCTL, (WPARAM)handle_of(next), 1);
        }
    } else if ((code & DLGC_BUTTON) != 0) {
        window *old = g_focus;
        send(control, BM_CLICK, 0, 0);
        if (control->used) {
            dialog_check_default(dlg, old, control, 0);
        }
    } else {
        send(dlg, WM_NEXTDLGCTL, (WPARAM)handle_of(control), 1);
    }
    return 1;
}

/* IsDialogMessage: the dialog's keys. 0 when msg is not for it. */
static int dialog_message(window *dlg, MSG *msg) {
    window *target = window_of(msg->hwnd);
    if (target == 0 || !within(target, dlg)) {
        return 0;
    }
    const UINT code = dlg_code(target, msg->wParam, msg);
    window *focus = g_focus != 0 && within(g_focus, dlg) ? g_focus : 0;
    switch (msg->message) {
    case WM_KEYDOWN:
        switch (msg->wParam) {
        case VK_TAB:
            if ((code & DLGC_WANTTAB) == 0) {
                change_ui_state(dlg, UIS_CLEAR, UISF_HIDEFOCUS);
                window *next = next_tab_item(dlg, focus, GetKeyState(VK_SHIFT) < 0);
                if (next != 0) {
                    send(dlg, WM_NEXTDLGCTL, (WPARAM)handle_of(next), 1);
                }
                return 1;
            }
            break;
        case VK_LEFT:
        case VK_RIGHT:
        case VK_UP:
        case VK_DOWN:
            if ((code & DLGC_WANTARROWS) == 0) {
                change_ui_state(dlg, UIS_CLEAR, UISF_HIDEFOCUS);
                window *next =
                    focus != 0 ? next_group_item(dlg, focus, msg->wParam == VK_LEFT || msg->wParam == VK_UP) : 0;
                if (next != 0 && next != focus) {
                    send(dlg, WM_NEXTDLGCTL, (WPARAM)handle_of(next), 1);
                }
                return 1;
            }
            break;
        case VK_ESCAPE:
            send(dlg, WM_COMMAND, IDCANCEL, (LPARAM)handle_of(item_of(dlg, IDCANCEL)));
            return 1;
        case VK_RETURN:
            if ((code & DLGC_WANTMESSAGE) != 0) {
                break;
            }
            if (focus != 0 && (dlg_code(focus, 0, 0) & DLGC_DEFPUSHBUTTON) != 0) {
                send(dlg, WM_COMMAND, MAKELPARAM(focus->id, BN_CLICKED), (LPARAM)handle_of(focus));
            } else {
                const LRESULT id = send(dlg, DM_GETDEFID, 0, 0);
                if (HIWORD(id) == DC_HASDEFID) {
                    window *b = item_of(dlg, LOWORD(id));
                    if (b == 0 || (b->style & WS_DISABLED) == 0) {
                        send(dlg, WM_COMMAND, MAKELPARAM(LOWORD(id), BN_CLICKED), (LPARAM)handle_of(b));
                    }
                } else {
                    send(dlg, WM_COMMAND, IDOK, (LPARAM)handle_of(item_of(dlg, IDOK)));
                }
            }
            return 1;
        default:
            break;
        }
        break;
    case WM_SYSKEYDOWN:
        if (msg->wParam == VK_MENU) {
            change_ui_state(dlg, UIS_CLEAR, UISF_HIDEFOCUS | UISF_HIDEACCEL);
        }
        break;
    case WM_CHAR:
        if ((code & (DLGC_WANTCHARS | DLGC_WANTMESSAGE)) != 0) {
            break;
        }
        if (msg->wParam == '\t' || msg->wParam == '\r' || msg->wParam == 0x1b) {
            return 1;
        }
        if (accelerator(dlg, (WCHAR)msg->wParam)) {
            return 1;
        }
        break;
    case WM_SYSCHAR:
        if (accelerator(dlg, (WCHAR)msg->wParam)) {
            return 1;
        }
        break;
    default:
        break;
    }
    TranslateMessage(msg);
    DispatchMessageW(msg);
    return 1;
}

/* ---- Templates ----------------------------------------------------------------------------- */

/* A string or ordinal of a template: *ordinal set, or *text pointing at
 * the string; the position after it. */
static const unsigned char *string_or_ordinal(const unsigned char *p, const unsigned char *end, u32 *ordinal,
                                              const WCHAR **text) {
    *ordinal = 0;
    *text = 0;
    if (p + 2 > end) {
        return 0;
    }
    if (get16(p) == 0xffffu) {
        if (p + 4 > end) {
            return 0;
        }
        *ordinal = get16(p + 2);
        return p + 4;
    }
    *text = (const WCHAR *)p;
    while (p + 2 <= end && get16(p) != 0) {
        p += 2;
    }
    return p + 2 <= end ? p + 2 : 0;
}

static const WCHAR k_empty[] = {0};

/* The class name a template's ordinal stands for. */
static LPCWSTR class_of(u32 ordinal) {
    static const WCHAR k_button[] = {'B', 'u', 't', 't', 'o', 'n', 0};
    static const WCHAR k_edit[] = {'E', 'd', 'i', 't', 0};
    static const WCHAR k_static[] = {'S', 't', 'a', 't', 'i', 'c', 0};
    static const WCHAR k_listbox[] = {'L', 'i', 's', 't', 'B', 'o', 'x', 0};
    static const WCHAR k_scrollbar[] = {'S', 'c', 'r', 'o', 'l', 'l', 'B', 'a', 'r', 0};
    static const WCHAR k_combobox[] = {'C', 'o', 'm', 'b', 'o', 'B', 'o', 'x', 0};
    switch (ordinal) {
    case 0x80: return k_button;
    case 0x81: return k_edit;
    case 0x82: return k_static;
    case 0x83: return k_listbox;
    case 0x84: return k_scrollbar;
    case 0x85: return k_combobox;
    default: return 0;
    }
}

static int mul_div(int a, int b, int c) {
    const long long p = (long long)a * b;
    return (int)(p >= 0 ? (p + c / 2) / c : -((-p + c / 2) / c));
}

/* The dialog base units of a font: the average width of the alphabet,
 * rounded as Windows does, and the height. */
static void base_units(HFONT font, int *x, int *y) {
    HDC dc = GetDC(NULL);
    if (font != NULL) {
        SelectObject(dc, font);
    }
    static const WCHAR k_alphabet[] = {'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H', 'I', 'J', 'K', 'L', 'M', 'N', 'O',
                                       'P', 'Q', 'R', 'S', 'T', 'U', 'V', 'W', 'X', 'Y', 'Z', 'a', 'b', 'c', 'd',
                                       'e', 'f', 'g', 'h', 'i', 'j', 'k', 'l', 'm', 'n', 'o', 'p', 'q', 'r', 's',
                                       't', 'u', 'v', 'w', 'x', 'y', 'z'};
    SIZE size;
    GetTextExtentPoint32W(dc, k_alphabet, 52, &size);
    TEXTMETRICW tm;
    GetTextMetricsW(dc, &tm);
    ReleaseDC(NULL, dc);
    *x = (size.cx / 26 + 1) / 2;
    *y = tm.tmHeight;
}

/* Creates a dialog from a template, its controls and its focus, as
 * DialogBox does before its loop. NULL when it cannot. */
static HWND create_dialog(HINSTANCE instance, const unsigned char *data, u32 size, window *owner, DLGPROC proc,
                          LPARAM param, dialog_info **out) {
    const unsigned char *end = data + size;
    if (size < 18 || get16(data + 2) == 0xffffu) {
        set_last_error(ERROR_INVALID_PARAMETER);
        return NULL;
    }
    const DWORD style = get32(data);
    DWORD ex_style = get32(data + 4);
    const int count = (int)get16(data + 8);
    const int tx = (short)get16(data + 10);
    const int ty = (short)get16(data + 12);
    const int tcx = (short)get16(data + 14);
    const int tcy = (short)get16(data + 16);
    const unsigned char *p = data + 18;
    u32 menu_id;
    u32 class_id;
    u32 none;
    const WCHAR *menu_name;
    const WCHAR *class_name;
    const WCHAR *title;
    p = string_or_ordinal(p, end, &menu_id, &menu_name);
    p = p != 0 ? string_or_ordinal(p, end, &class_id, &class_name) : 0;
    p = p != 0 ? string_or_ordinal(p, end, &none, &title) : 0;
    if (p == 0) {
        set_last_error(ERROR_INVALID_PARAMETER);
        return NULL;
    }
    HFONT font = NULL;
    if ((style & DS_SETFONT) != 0) {
        if (p + 2 > end) {
            return NULL;
        }
        const int points = (int)get16(p);
        const WCHAR *face;
        p = string_or_ordinal(p + 2, end, &none, &face);
        if (p == 0) {
            return NULL;
        }
        font = CreateFontW(-mul_div(points, 96, 72), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, 0, 0,
                           face != 0 ? face : k_empty);
    }
    int bx;
    int by;
    base_units(font != NULL ? font : (HFONT)GetStockObject(SYSTEM_FONT), &bx, &by);

    dialog_info *d = new_info();
    if (d == 0) {
        if (font != NULL) {
            DeleteObject(font);
        }
        set_last_error(ERROR_NOT_ENOUGH_MEMORY);
        return NULL;
    }
    d->font = font;
    d->made_font = font != NULL;
    if (owner != 0) {
        owner = top_of(owner);
        d->owner = owner;
        if ((owner->style & WS_DISABLED) == 0) {
            EnableWindow(handle_of(owner), FALSE);
            d->owner_disabled = 1;
        }
    }

    if ((style & DS_MODALFRAME) != 0) {
        ex_style |= WS_EX_DLGMODALFRAME;
    }
    if ((style & DS_CONTEXTHELP) != 0) {
        ex_style |= WS_EX_CONTEXTHELP;
    }
    RECT r = {0, 0, mul_div(tcx, bx, 4), mul_div(tcy, by, 8)};
    AdjustWindowRect(&r, style & ~(DWORD)WS_VISIBLE, FALSE);
    int x = mul_div(tx, bx, 4);
    int y = mul_div(ty, by, 8);
    if (owner != 0) {
        x += owner->client.left;
        y += owner->client.top;
    }
    static const WCHAR k_dialog_class[] = {'#', '3', '2', '7', '7', '0', 0};
    LPCWSTR cls = class_name != 0 && class_name[0] != 0 ? class_name : class_id != 0 ? (LPCWSTR)(u32)class_id
                                                                                         : k_dialog_class;
    HWND hwnd = CreateWindowExW(ex_style, cls, title != 0 ? title : k_empty, style & ~(DWORD)WS_VISIBLE, x, y,
                                r.right - r.left, r.bottom - r.top, handle_of(owner), NULL, instance, NULL);
    window *w = window_of(hwnd);
    if (w == 0) {
        if (d->owner_disabled) {
            EnableWindow(handle_of(owner), TRUE);
        }
        free_info(d);
        return NULL;
    }
    w->dialog = d;
    d->proc = proc;
    if (font != NULL) {
        send(w, WM_SETFONT, (WPARAM)font, 0);
    }

    for (int i = 0; i < count && window_of(hwnd) != 0; ++i) {
        p = (const unsigned char *)(((u32)p + 3u) & ~3u);
        if (p + 18 > end) {
            break;
        }
        const DWORD item_style = get32(p);
        const DWORD item_ex = get32(p + 4);
        const int ix = (short)get16(p + 8);
        const int iy = (short)get16(p + 10);
        const int icx = (short)get16(p + 12);
        const int icy = (short)get16(p + 14);
        const UINT id = get16(p + 16);
        u32 item_class_id;
        const WCHAR *item_class;
        u32 text_id;
        const WCHAR *text;
        p = string_or_ordinal(p + 18, end, &item_class_id, &item_class);
        p = p != 0 ? string_or_ordinal(p, end, &text_id, &text) : 0;
        if (p == 0 || p + 2 > end) {
            break;
        }
        p += 2 + get16(p);
        LPCWSTR name = item_class != 0 ? item_class : class_of(item_class_id);
        if (name == 0) {
            continue;
        }
        if (d->default_id == 0 && item_class_id == 0x80 && (item_style & BS_TYPEMASK) == BS_DEFPUSHBUTTON) {
            d->default_id = id;
        }
        HWND control = CreateWindowExW(item_ex | WS_EX_NOPARENTNOTIFY, name, text != 0 ? text : k_empty,
                                       item_style | WS_CHILD, mul_div(ix, bx, 4), mul_div(iy, by, 8),
                                       mul_div(icx, bx, 4), mul_div(icy, by, 8), hwnd, (HMENU)(u32)id, instance,
                                       NULL);
        if (control != NULL && font != NULL) {
            send_handle(control, WM_SETFONT, (WPARAM)font, 0);
        }
    }
    if (window_of(hwnd) == 0) {
        return NULL;
    }
    window *first = next_tab_item(w, 0, 0);
    if (send(w, WM_INITDIALOG, (WPARAM)handle_of(first), param) != 0 && window_of(hwnd) != 0) {
        first = next_tab_item(w, 0, 0);
        if (first != 0) {
            SetFocus(handle_of(first));
        }
    }
    if (window_of(hwnd) == 0) {
        return NULL;
    }
    send(w, WM_CHANGEUISTATE, MAKELPARAM(UIS_INITIALIZE, 0), 0);
    if ((style & WS_VISIBLE) != 0) {
        ShowWindow(hwnd, SW_SHOWNORMAL);
    }
    *out = d;
    return hwnd;
}

/* The dialog's modal loop: it is shown when the queue first runs dry, and
 * runs until EndDialog. */
static INT_PTR modal_loop(HWND hwnd, dialog_info *d) {
    window *w = window_of(hwnd);
    int idle_sent = 0;
    while (w != 0 && w->used && !d->ended) {
        MSG msg;
        if (!next_message(&msg, NULL, 0, 0, 1, 0)) {
            if ((w->style & WS_VISIBLE) == 0) {
                ShowWindow(hwnd, SW_SHOWNORMAL);
                continue;
            }
            if (!idle_sent && d->owner != 0) {
                idle_sent = 1;
                send(d->owner, WM_ENTERIDLE, MSGF_DIALOGBOX, (LPARAM)hwnd);
                continue;
            }
            next_message(&msg, NULL, 0, 0, 1, 1);
        }
        idle_sent = 0;
        if (msg.message == WM_QUIT) {
            PostQuitMessage((int)msg.wParam);
            break;
        }
        if (window_of(hwnd) == 0) {
            break;
        }
        if (!dialog_message(w, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    const INT_PTR result = d->ended ? d->result : -1;
    if (window_of(hwnd) != 0) {
        if (!d->ended && d->owner_disabled && d->owner != 0 && d->owner->used) {
            EnableWindow(handle_of(d->owner), TRUE);
        }
        DestroyWindow(hwnd);
    }
    return result;
}

static INT_PTR dialog_box(HINSTANCE instance, const unsigned char *data, u32 size, HWND owner, DLGPROC proc,
                          LPARAM param) {
    window *o = owner != NULL ? window_of(owner) : 0;
    dialog_info *d = 0;
    HWND hwnd = create_dialog(instance, data, size, o, proc, param, &d);
    if (hwnd == NULL) {
        return -1;
    }
    return modal_loop(hwnd, d);
}

INT_PTR WINAPI DialogBoxParamW(HINSTANCE hInstance, LPCWSTR lpTemplateName, HWND hWndParent, DLGPROC lpDialogFunc,
                               LPARAM dwInitParam) {
    u32 size = 0;
    const unsigned char *data = find_resource(module_of(hInstance), RT_DIALOG, lpTemplateName, &size);
    if (data == 0) {
        set_last_error(ERROR_RESOURCE_NAME_NOT_FOUND);
        return -1;
    }
    return dialog_box(hInstance, data, size, hWndParent, lpDialogFunc, dwInitParam);
}

/* For shell32's About box: a template built in memory. */
__declspec(dllexport) INT_PTR WINAPI SxwDialogBoxIndirectParamW(HINSTANCE hInstance, const void *lpTemplate,
                                                                u32 size, HWND hWndParent, DLGPROC lpDialogFunc,
                                                                LPARAM dwInitParam) {
    return dialog_box(hInstance, lpTemplate, size, hWndParent, lpDialogFunc, dwInitParam);
}

/* The owner is enabled again, the focus goes to the dialog itself and the
 * dialog is hidden without WM_SHOWWINDOW, its owner activated: what
 * Windows XP does before DialogBox returns. */
BOOL WINAPI EndDialog(HWND hDlg, INT_PTR nResult) {
    window *w = checked(hDlg);
    dialog_info *d = w != 0 ? w->dialog : 0;
    if (d == 0) {
        return FALSE;
    }
    d->result = nResult;
    d->ended = 1;
    if (d->owner_disabled && d->owner != 0 && d->owner->used) {
        EnableWindow(handle_of(d->owner), TRUE);
        d->owner_disabled = 0;
    }
    if (g_focus != 0 && g_focus != w && within(g_focus, w)) {
        SetFocus(hDlg);
    }
    hide_window(w);
    PostMessageW(hDlg, WM_NULL, 0, 0);
    return TRUE;
}

/* ---- MessageBoxW ------------------------------------------------------------------------- */

typedef struct box_builder {
    unsigned char data[MAX_TEMPLATE];
    u32 size;
    int ok;
} box_builder;

static void put16(box_builder *b, u32 v) {
    if (b->size + 2 > MAX_TEMPLATE) {
        b->ok = 0;
        return;
    }
    b->data[b->size++] = (unsigned char)v;
    b->data[b->size++] = (unsigned char)(v >> 8);
}

static void put32(box_builder *b, u32 v) {
    put16(b, v & 0xffffu);
    put16(b, v >> 16);
}

static void put_text(box_builder *b, const WCHAR *text) {
    for (int i = 0; text != 0 && text[i] != 0; ++i) {
        put16(b, text[i]);
    }
    put16(b, 0);
}

static void align4(box_builder *b) {
    while ((b->size & 3u) != 0) {
        put16(b, 0);
    }
}

static void put_item(box_builder *b, DWORD style, int x, int y, int cx, int cy, UINT id, u32 class_ordinal,
                     const WCHAR *text) {
    align4(b);
    put32(b, style | WS_CHILD | WS_VISIBLE);
    put32(b, 0);
    put16(b, (u32)x);
    put16(b, (u32)y);
    put16(b, (u32)cx);
    put16(b, (u32)cy);
    put16(b, id);
    put16(b, 0xffff);
    put16(b, class_ordinal);
    put_text(b, text);
    put16(b, 0);
}

typedef struct box_state {
    UINT type;
    int has_cancel;
} box_state;

static box_state g_box;

static void draw_icon(HDC dc, int left, int top, UINT icon) {
    /* A 32x32 sign of its own: a yellow triangle for an exclamation, a red
     * disc for an error, a blue disc for information or a question. */
    const COLORREF fill = icon == MB_ICONEXCLAMATION ? RGB(255, 255, 0)
                          : icon == MB_ICONHAND      ? RGB(255, 0, 0)
                                                     : RGB(0, 0, 255);
    const COLORREF mark = icon == MB_ICONEXCLAMATION ? RGB(0, 0, 0) : RGB(255, 255, 255);
    for (int y = 0; y < 32; ++y) {
        for (int x = 0; x < 32; ++x) {
            int inside;
            if (icon == MB_ICONEXCLAMATION) {
                const int half = (y * 15) / 31;
                inside = y >= 1 && x >= 15 - half && x <= 16 + half;
            } else {
                const int dx = 2 * x - 31;
                const int dy = 2 * y - 31;
                inside = dx * dx + dy * dy <= 31 * 31;
            }
            if (inside) {
                SetPixel(dc, left + x, top + y, fill);
            }
        }
    }
    const int first = icon == MB_ICONEXCLAMATION ? 9 : 7;
    for (int y = first; y < 26; ++y) {
        if (y == 21 || y == 22) {
            continue;
        }
        for (int x = 15; x < 17; ++x) {
            SetPixel(dc, left + x, top + y, mark);
        }
    }
}

static INT_PTR WINAPI box_proc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    (void)lParam;
    switch (message) {
    case WM_INITDIALOG: {
        const int which = (int)((g_box.type & MB_DEFMASK) >> 8);
        static const int k_order[6][3] = {{IDOK}, {IDOK, IDCANCEL}, {IDABORT, IDRETRY, IDIGNORE},
                                          {IDYES, IDNO, IDCANCEL}, {IDYES, IDNO}, {IDRETRY, IDCANCEL}};
        const int type = (int)(g_box.type & MB_TYPEMASK) < 6 ? (int)(g_box.type & MB_TYPEMASK) : 0;
        int id = k_order[type][which < 3 ? which : 0];
        if (id == 0) {
            id = k_order[type][0];
        }
        HWND button = GetDlgItem(hwnd, id);
        send_handle(hwnd, DM_SETDEFID, (WPARAM)id, 0);
        SetFocus(button);
        return FALSE;
    }
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        const UINT icon = g_box.type & MB_ICONMASK;
        if (icon != 0) {
            draw_icon(dc, 11, 11, icon);
        }
        EndPaint(hwnd, &ps);
        return TRUE;
    }
    case WM_COMMAND: {
        const int id = LOWORD(wParam);
        if (id == IDCANCEL && !g_box.has_cancel) {
            if ((g_box.type & MB_TYPEMASK) == MB_OK) {
                EndDialog(hwnd, IDOK);
            }
            return TRUE;
        }
        if (id >= IDOK && id <= IDNO) {
            EndDialog(hwnd, id);
        }
        return TRUE;
    }
    default:
        return FALSE;
    }
}

int WINAPI MessageBoxW(HWND hWnd, LPCWSTR lpText, LPCWSTR lpCaption, UINT uType) {
    static const WCHAR k_ok[] = {'O', 'K', 0};
    static const WCHAR k_cancel[] = {'C', 'a', 'n', 'c', 'e', 'l', 0};
    static const WCHAR k_abort[] = {'&', 'A', 'b', 'o', 'r', 't', 0};
    static const WCHAR k_retry[] = {'&', 'R', 'e', 't', 'r', 'y', 0};
    static const WCHAR k_ignore[] = {'&', 'I', 'g', 'n', 'o', 'r', 'e', 0};
    static const WCHAR k_yes[] = {'&', 'Y', 'e', 's', 0};
    static const WCHAR k_no[] = {'&', 'N', 'o', 0};
    static const WCHAR k_error[] = {'E', 'r', 'r', 'o', 'r', 0};
    static const WCHAR k_face[] = {'M', 'S', ' ', 'S', 'h', 'e', 'l', 'l', ' ', 'D', 'l', 'g', 0};
    const WCHAR *labels[3] = {0, 0, 0};
    int ids[3] = {0, 0, 0};
    int count = 0;
    switch (uType & MB_TYPEMASK) {
    case MB_OKCANCEL:
        labels[0] = k_ok, ids[0] = IDOK, labels[1] = k_cancel, ids[1] = IDCANCEL, count = 2;
        break;
    case MB_ABORTRETRYIGNORE:
        labels[0] = k_abort, ids[0] = IDABORT, labels[1] = k_retry, ids[1] = IDRETRY;
        labels[2] = k_ignore, ids[2] = IDIGNORE, count = 3;
        break;
    case MB_YESNOCANCEL:
        labels[0] = k_yes, ids[0] = IDYES, labels[1] = k_no, ids[1] = IDNO;
        labels[2] = k_cancel, ids[2] = IDCANCEL, count = 3;
        break;
    case MB_YESNO:
        labels[0] = k_yes, ids[0] = IDYES, labels[1] = k_no, ids[1] = IDNO, count = 2;
        break;
    case MB_RETRYCANCEL:
        labels[0] = k_retry, ids[0] = IDRETRY, labels[1] = k_cancel, ids[1] = IDCANCEL, count = 2;
        break;
    default:
        labels[0] = k_ok, ids[0] = IDOK, count = 1;
        break;
    }
    g_box.type = uType;
    g_box.has_cancel = 0;
    for (int i = 0; i < count; ++i) {
        g_box.has_cancel |= ids[i] == IDCANCEL;
    }

    /* The text's size in dialog units (6x13 pixels per 4x8 units). */
    HFONT font = CreateFontW(-11, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, 0, 0, k_face);
    HDC dc = GetDC(NULL);
    SelectObject(dc, font);
    RECT text = {0, 0, 400, 0};
    DrawTextW(dc, lpText != NULL ? lpText : k_error + 5, -1, &text, DT_CALCRECT | DT_WORDBREAK | DT_EXPANDTABS);
    ReleaseDC(NULL, dc);
    DeleteObject(font);
    const int icon = (uType & MB_ICONMASK) != 0;
    const int text_x = icon ? 32 : 8;
    const int text_cx = (text.right * 4 + 5) / 6 + 2;
    const int text_cy = (text.bottom * 8 + 12) / 13;
    const int body_cy = icon && text_cy < 22 ? 22 : text_cy;
    const int buttons_cx = count * 50 + (count - 1) * 4;
    int cx = text_x + text_cx + 8;
    cx = cx < buttons_cx + 16 ? buttons_cx + 16 : cx;
    const int cy = 8 + body_cy + 8 + 14 + 6;

    static box_builder b;
    b.size = 0;
    b.ok = 1;
    put32(&b, DS_MODALFRAME | DS_SETFONT | DS_CENTER | WS_POPUP | WS_CAPTION | WS_SYSMENU);
    put32(&b, 0);
    put16(&b, (u32)(count + 1));
    put16(&b, 0);
    put16(&b, 0);
    put16(&b, (u32)cx);
    put16(&b, (u32)cy);
    put16(&b, 0);
    put16(&b, 0);
    put_text(&b, lpCaption != NULL ? lpCaption : k_error);
    put16(&b, 8);
    put_text(&b, k_face);
    put_item(&b, SS_LEFT | SS_NOPREFIX, text_x, 8 + (body_cy - text_cy) / 2, text_cx, text_cy, 0xffff, 0x82,
             lpText != NULL ? lpText : k_error + 5);
    int x = (cx - buttons_cx) / 2;
    for (int i = 0; i < count; ++i) {
        put_item(&b, BS_PUSHBUTTON | WS_TABSTOP | (i == 0 ? WS_GROUP : 0), x, 8 + body_cy + 8, 50, 14,
                 (UINT)ids[i], 0x80, labels[i]);
        x += 54;
    }
    if (!b.ok) {
        return 0;
    }
    const INT_PTR result = dialog_box(NULL, b.data, b.size, hWnd, box_proc, 0);
    return result > 0 ? (int)result : 0;
}

/* Help is out of the profile's scope: there is no help engine. */
BOOL WINAPI WinHelpW(HWND hWndMain, LPCWSTR lpszHelp, UINT uCommand, ULONG_PTR dwData) {
    (void)hWndMain;
    (void)lpszHelp;
    (void)uCommand;
    (void)dwData;
    set_last_error(ERROR_FILE_NOT_FOUND);
    return FALSE;
}
