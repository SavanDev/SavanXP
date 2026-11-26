/*
 * controls.exe -- The WP5 fixture of the standard controls WordPad's
 * dialogs and format bar use (WIN32_WORDPAD_ROADMAP.md, WP5): single-line
 * Edit controls, list boxes (plain and sorted, with a scroll bar), the four
 * combo boxes (simple, drop-down, drop-down list and owner-drawn), scroll
 * bar controls, a window with scroll bars of its own, and a popup menu from
 * TrackPopupMenu, driven by an input script (input.txt next to this file).
 *
 * First it asks every control what Windows XP answers without input: the
 * Edit, LB_ and CB_ messages, the styles the controls give themselves, the
 * windows a combo box is made of. Then the script types, clicks, drags and
 * drops lists down, and the main window writes the notifications it gets
 * (WM_COMMAND, WM_VSCROLL and WM_HSCROLL, the owner-drawn messages) and, as
 * the window closes, where every control ended up. On F2 it writes
 * the pixels of the controls' frames, arrows and scroll bars as runs of
 * colour; no pixel of text is read, and nothing it writes depends on the
 * width of a glyph.
 *
 * The client area is 400x300 and starts with a 4x4 magenta marker.
 */
#include <windows.h>

#include "../kernel/print.h"

DECLSPEC_IMPORT HMODULE WINAPI GetModuleHandleW(LPCWSTR name);

#define CLIENT_WIDTH 400
#define CLIENT_HEIGHT 300

#define ID_EDIT1 201
#define ID_EDIT2 202
#define ID_LIST1 203
#define ID_LIST2 204
#define ID_SIMPLE 205
#define ID_DROPDOWN 206
#define ID_DROPLIST 207
#define ID_OWNER 208
#define ID_VSCROLL 209
#define ID_HSCROLL 210
#define ID_PANE 211

static HINSTANCE g_instance;
static HWND g_main;
static HWND g_edit1;
static HWND g_edit2;
static HWND g_list1;
static HWND g_list2;
static HWND g_simple;
static HWND g_dropdown;
static HWND g_droplist;
static HWND g_owner;
static HWND g_vscroll;
static HWND g_hscroll;
static HWND g_pane;
static HFONT g_font;
static int g_quiet; /* notifications are not written while the program itself asks */

/* ---- Names ------------------------------------------------------------------------------ */

static void say_window(HWND h) {
    static const struct {
        HWND *h;
        const char *name;
    } k_names[] = {
        {&g_main, "main"},       {&g_edit1, "edit1"},       {&g_edit2, "edit2"},   {&g_list1, "list1"},
        {&g_list2, "list2"},     {&g_simple, "simple"},     {&g_dropdown, "dropdown"}, {&g_droplist, "droplist"},
        {&g_owner, "owner"},     {&g_vscroll, "vscroll"},   {&g_hscroll, "hscroll"}, {&g_pane, "pane"},
    };
    if (h == NULL) {
        say("none");
        return;
    }
    for (unsigned i = 0; i < sizeof(k_names) / sizeof(k_names[0]); ++i) {
        if (*k_names[i].h == h) {
            say(k_names[i].name);
            return;
        }
    }
    HWND parent = GetParent(h);
    for (unsigned i = 0; i < sizeof(k_names) / sizeof(k_names[0]); ++i) {
        if (parent != NULL && *k_names[i].h == parent) {
            say(k_names[i].name);
            say("'s ");
            WCHAR cls[32];
            GetClassNameW(h, cls, 32);
            say_w(cls);
            return;
        }
    }
    say("other");
}

static const char *code_name(int id, int code) {
    if (id == ID_EDIT1 || id == ID_EDIT2) {
        switch (code) {
        case EN_SETFOCUS: return "EN_SETFOCUS";
        case EN_KILLFOCUS: return "EN_KILLFOCUS";
        case EN_CHANGE: return "EN_CHANGE";
        case EN_UPDATE: return "EN_UPDATE";
        case EN_MAXTEXT: return "EN_MAXTEXT";
        case EN_HSCROLL: return "EN_HSCROLL";
        default: return 0;
        }
    }
    if (id == ID_LIST1 || id == ID_LIST2) {
        switch (code) {
        case LBN_SELCHANGE: return "LBN_SELCHANGE";
        case LBN_DBLCLK: return "LBN_DBLCLK";
        case LBN_SELCANCEL: return "LBN_SELCANCEL";
        case LBN_SETFOCUS: return "LBN_SETFOCUS";
        case LBN_KILLFOCUS: return "LBN_KILLFOCUS";
        default: return 0;
        }
    }
    switch (code) {
    case CBN_SELCHANGE: return "CBN_SELCHANGE";
    case CBN_DBLCLK: return "CBN_DBLCLK";
    case CBN_SETFOCUS: return "CBN_SETFOCUS";
    case CBN_KILLFOCUS: return "CBN_KILLFOCUS";
    case CBN_EDITCHANGE: return "CBN_EDITCHANGE";
    case CBN_EDITUPDATE: return "CBN_EDITUPDATE";
    case CBN_DROPDOWN: return "CBN_DROPDOWN";
    case CBN_CLOSEUP: return "CBN_CLOSEUP";
    case CBN_SELENDOK: return "CBN_SELENDOK";
    case CBN_SELENDCANCEL: return "CBN_SELENDCANCEL";
    default: return 0;
    }
}

static const char *scroll_code(int code) {
    static const char *const k_codes[] = {"SB_LINEUP", "SB_LINEDOWN", "SB_PAGEUP", "SB_PAGEDOWN",
                                          "SB_THUMBPOSITION", "SB_THUMBTRACK", "SB_TOP", "SB_BOTTOM",
                                          "SB_ENDSCROLL"};
    return code >= 0 && code <= 8 ? k_codes[code] : "?";
}

static void say_text(HWND h) {
    WCHAR text[128];
    GetWindowTextW(h, text, 128);
    say_w(text);
}

/* ---- Pixels ------------------------------------------------------------------------------- */

typedef struct region {
    const char *name;
    RECT r;      /* in the main window's client coordinates */
    RECT hole;   /* left out: text may be there */
} region;

static void say_color(COLORREF c) {
    char digits[7];
    const unsigned v = (unsigned)(GetRValue(c) << 16 | GetGValue(c) << 8 | GetBValue(c));
    for (int i = 0; i < 6; ++i) {
        const unsigned nibble = (v >> (20 - 4 * i)) & 0xfu;
        digits[i] = (char)(nibble < 10u ? '0' + nibble : 'a' + nibble - 10u);
    }
    digits[6] = 0;
    say(digits);
}

/* A region's pixels row by row as runs "RRGGBB*count", the hole as "text",
 * a row like the one above as "=". */
static void say_region(HDC dc, const region *g) {
    static COLORREF row[CLIENT_WIDTH];
    static COLORREF above[CLIENT_WIDTH];
    const int width = g->r.right - g->r.left;
    line("pixels ");
    say(g->name);
    say(" ");
    say_int(g->r.left);
    say(",");
    say_int(g->r.top);
    say(" ");
    say_int(width);
    say("x");
    say_int(g->r.bottom - g->r.top);
    for (int y = g->r.top; y < g->r.bottom; ++y) {
        int same = y != g->r.top;
        for (int x = g->r.left; x < g->r.right; ++x) {
            const int hole = x >= g->hole.left && x < g->hole.right && y >= g->hole.top && y < g->hole.bottom;
            row[x - g->r.left] = hole ? 0xff000000u : GetPixel(dc, x, y);
            same = same && row[x - g->r.left] == above[x - g->r.left];
        }
        say("\r\n ");
        if (same) {
            say("=");
            continue;
        }
        for (int x = 0; x < width;) {
            int run = 1;
            while (x + run < width && row[x + run] == row[x]) {
                ++run;
            }
            say(" ");
            if (row[x] == 0xff000000u) {
                say("text");
            } else {
                say_color(row[x]);
            }
            say("*");
            say_int(run);
            x += run;
        }
        for (int x = 0; x < width; ++x) {
            above[x] = row[x];
        }
    }
}

static RECT window_rect(HWND h) {
    RECT r;
    GetWindowRect(h, &r);
    MapWindowPoints(NULL, g_main, (POINT *)&r, 2);
    return r;
}

static RECT inset(RECT r, int left, int top, int right, int bottom) {
    r.left += left;
    r.top += top;
    r.right -= right;
    r.bottom -= bottom;
    return r;
}

static void write_pixels(void) {
    const RECT none = {0, 0, 0, 0};
    region g[12];
    int n = 0;
    /* An edit control's text sits inside its frame; a list box's items in
     * its client area left of the bar; a combo box's selection field left
     * of its button. */
    g[n].name = "edit1";
    g[n].r = window_rect(g_edit1);
    g[n].hole = inset(g[n].r, 2, 2, 2, 2);
    ++n;
    g[n].name = "edit2";
    g[n].r = window_rect(g_edit2);
    g[n].hole = inset(g[n].r, 3, 3, 3, 3);
    ++n;
    g[n].name = "list1";
    g[n].r = window_rect(g_list1);
    g[n].hole = inset(g[n].r, 1, 1, 17, 1);
    ++n;
    g[n].name = "list2";
    g[n].r = window_rect(g_list2);
    g[n].hole = inset(g[n].r, 2, 2, 2, 2);
    ++n;
    g[n].name = "simple";
    g[n].r = window_rect(g_simple);
    g[n].hole = inset(g[n].r, 2, 2, 2, 2);
    ++n;
    g[n].name = "dropdown";
    g[n].r = window_rect(g_dropdown);
    g[n].hole = inset(g[n].r, 2, 2, 19, 2);
    ++n;
    g[n].name = "droplist";
    g[n].r = window_rect(g_droplist);
    g[n].hole = inset(g[n].r, 2, 2, 19, 2);
    ++n;
    g[n].name = "owner";
    g[n].r = window_rect(g_owner);
    g[n].hole = inset(g[n].r, 2, 2, 19, 2);
    ++n;
    g[n].name = "vscroll";
    g[n].r = window_rect(g_vscroll);
    g[n].hole = none;
    ++n;
    g[n].name = "hscroll";
    g[n].r = window_rect(g_hscroll);
    g[n].hole = none;
    ++n;
    g[n].name = "pane";
    g[n].r = window_rect(g_pane);
    g[n].hole = none;
    ++n;
    HDC dc = GetDC(g_main);
    for (int i = 0; i < n; ++i) {
        say_region(dc, &g[i]);
    }
    ReleaseDC(g_main, dc);
}

/* ---- The state at the end ------------------------------------------------------------------- */

static void write_state(void) {
    DWORD start = 0;
    DWORD end = 0;
    line("end edit1 ");
    say_text(g_edit1);
    SendMessageW(g_edit1, EM_GETSEL, (WPARAM)&start, (LPARAM)&end);
    say(" sel ");
    say_int((long)start);
    say(",");
    say_int((long)end);
    line("end edit2 ");
    say_text(g_edit2);
    static HWND *const k_lists[] = {&g_list1, &g_list2};
    for (int i = 0; i < 2; ++i) {
        line("end ");
        say_window(*k_lists[i]);
        say(" cur ");
        say_int(SendMessageW(*k_lists[i], LB_GETCURSEL, 0, 0));
        say(" top ");
        say_int(SendMessageW(*k_lists[i], LB_GETTOPINDEX, 0, 0));
        say(" caret ");
        say_int(SendMessageW(*k_lists[i], LB_GETCARETINDEX, 0, 0));
    }
    static HWND *const k_combos[] = {&g_simple, &g_dropdown, &g_droplist, &g_owner};
    for (int i = 0; i < 4; ++i) {
        line("end ");
        say_window(*k_combos[i]);
        say(" ");
        say_text(*k_combos[i]);
        say(" cur ");
        say_int(SendMessageW(*k_combos[i], CB_GETCURSEL, 0, 0));
        say(" dropped ");
        say_int(SendMessageW(*k_combos[i], CB_GETDROPPEDSTATE, 0, 0));
    }
    line("end vscroll ");
    say_int(GetScrollPos(g_vscroll, SB_CTL));
    say(" hscroll ");
    say_int(GetScrollPos(g_hscroll, SB_CTL));
    say(" pane ");
    say_int(GetScrollPos(g_pane, SB_VERT));
    say(",");
    say_int(GetScrollPos(g_pane, SB_HORZ));
    line("end focus ");
    say_window(GetFocus());
}

/* ---- Window procedures ------------------------------------------------------------------------ */

static void follow_scroll(HWND bar, int which, HWND target, WPARAM wParam) {
    SCROLLINFO si;
    si.cbSize = sizeof(si);
    si.fMask = SIF_ALL;
    GetScrollInfo(target, which, &si);
    int pos = si.nPos;
    switch (LOWORD(wParam)) {
    case SB_LINEUP: pos -= 1; break;
    case SB_LINEDOWN: pos += 1; break;
    case SB_PAGEUP: pos -= (int)(si.nPage != 0 ? si.nPage : 5); break;
    case SB_PAGEDOWN: pos += (int)(si.nPage != 0 ? si.nPage : 5); break;
    case SB_THUMBTRACK: pos = si.nTrackPos; break;
    case SB_THUMBPOSITION: pos = HIWORD(wParam); break;
    case SB_TOP: pos = si.nMin; break;
    case SB_BOTTOM: pos = si.nMax; break;
    default: return;
    }
    (void)bar;
    SetScrollPos(target, which, pos, TRUE);
}

static LRESULT CALLBACK pane_proc(HWND hwnd, UINT m, WPARAM wParam, LPARAM lParam) {
    switch (m) {
    case WM_VSCROLL:
    case WM_HSCROLL:
        line("pane ");
        say(m == WM_VSCROLL ? "WM_VSCROLL " : "WM_HSCROLL ");
        say(scroll_code(LOWORD(wParam)));
        if (LOWORD(wParam) == SB_THUMBTRACK || LOWORD(wParam) == SB_THUMBPOSITION) {
            say(" ");
            say_int(HIWORD(wParam));
        }
        say(" from ");
        say_window((HWND)lParam);
        follow_scroll(NULL, m == WM_VSCROLL ? SB_VERT : SB_HORZ, hwnd, wParam);
        say(" now ");
        say_int(GetScrollPos(hwnd, m == WM_VSCROLL ? SB_VERT : SB_HORZ));
        return 0;
    case WM_CONTEXTMENU: {
        HMENU menu = CreatePopupMenu();
        AppendMenuW(menu, MF_STRING, 501, L"&Cut");
        AppendMenuW(menu, MF_STRING | MF_GRAYED, 502, L"C&opy");
        AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
        AppendMenuW(menu, MF_STRING | MF_CHECKED, 503, L"&Paste");
        POINT p = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        line("pane WM_CONTEXTMENU from ");
        say_window((HWND)wParam);
        const BOOL chosen = TrackPopupMenu(menu, TPM_LEFTALIGN | TPM_RIGHTBUTTON | TPM_RETURNCMD, p.x, p.y, 0, hwnd,
                                           NULL);
        line("pane chose ");
        say_int(chosen);
        DestroyMenu(menu);
        return 0;
    }
    case WM_INITMENUPOPUP:
        line("pane WM_INITMENUPOPUP");
        return 0;
    case WM_ENTERMENULOOP:
    case WM_EXITMENULOOP:
        line(m == WM_ENTERMENULOOP ? "pane WM_ENTERMENULOOP " : "pane WM_EXITMENULOOP ");
        say_int((long)wParam);
        return 0;
    case WM_MENUSELECT:
        line("pane WM_MENUSELECT ");
        say_int(LOWORD(wParam));
        say(" ");
        say_hex(HIWORD(wParam));
        return 0;
    case WM_ERASEBKGND: {
        RECT r;
        GetClientRect(hwnd, &r);
        HBRUSH b = CreateSolidBrush(RGB(255, 255, 255));
        FillRect((HDC)wParam, &r, b);
        DeleteObject(b);
        return 1;
    }
    default:
        return DefWindowProcW(hwnd, m, wParam, lParam);
    }
}

static LRESULT CALLBACK main_proc(HWND hwnd, UINT m, WPARAM wParam, LPARAM lParam) {
    switch (m) {
    case WM_COMMAND: {
        const char *name = code_name(LOWORD(wParam), HIWORD(wParam));
        if (!g_quiet || name == 0) {
            line("main WM_COMMAND ");
            say_window((HWND)lParam);
            say(" ");
            if (name != 0) {
                say(name);
            } else {
                say_int(HIWORD(wParam));
            }
        }
        return 0;
    }
    case WM_VSCROLL:
    case WM_HSCROLL: {
        HWND bar = (HWND)lParam;
        line("main ");
        say(m == WM_VSCROLL ? "WM_VSCROLL " : "WM_HSCROLL ");
        say(scroll_code(LOWORD(wParam)));
        if (LOWORD(wParam) == SB_THUMBTRACK || LOWORD(wParam) == SB_THUMBPOSITION) {
            say(" ");
            say_int(HIWORD(wParam));
        }
        say(" from ");
        say_window(bar);
        if (bar != NULL) {
            follow_scroll(bar, SB_CTL, bar, wParam);
            say(" now ");
            say_int(GetScrollPos(bar, SB_CTL));
        }
        return 0;
    }
    case WM_MEASUREITEM: {
        MEASUREITEMSTRUCT *mi = (MEASUREITEMSTRUCT *)lParam;
        line("main WM_MEASUREITEM type ");
        say_int((long)mi->CtlType);
        say(" id ");
        say_int((long)mi->CtlID);
        /* The list's measure for a fixed height carries no item worth
         * reading: Windows XP leaves itemID as it finds it. */
        say(" item ");
        if ((int)mi->itemID == -1) {
            say_int(-1);
        } else {
            say("of the list");
        }
        say(" height ");
        say_int((long)mi->itemHeight);
        mi->itemHeight = 16;
        return TRUE;
    }
    case WM_DRAWITEM: {
        DRAWITEMSTRUCT *d = (DRAWITEMSTRUCT *)lParam;
        line("main WM_DRAWITEM type ");
        say_int((long)d->CtlType);
        say(" id ");
        say_int((long)d->CtlID);
        say(" item ");
        say_int((long)(int)d->itemID);
        say(" action ");
        say_hex(d->itemAction);
        say(" state ");
        say_hex(d->itemState);
        say(" ");
        say_int(d->rcItem.left);
        say(",");
        say_int(d->rcItem.top);
        say(" ");
        say_int(d->rcItem.right - d->rcItem.left);
        say("x");
        say_int(d->rcItem.bottom - d->rcItem.top);
        say(" data ");
        say_int((long)d->itemData);
        HBRUSH b = CreateSolidBrush((d->itemState & ODS_SELECTED) != 0 ? RGB(0, 0, 128) : RGB(255, 255, 255));
        FillRect(d->hDC, &d->rcItem, b);
        DeleteObject(b);
        return TRUE;
    }
    case WM_DELETEITEM: {
        DELETEITEMSTRUCT *di = (DELETEITEMSTRUCT *)lParam;
        line("main WM_DELETEITEM type ");
        say_int((long)di->CtlType);
        say(" id ");
        say_int((long)di->CtlID);
        say(" item ");
        say_int((long)di->itemID);
        say(" data ");
        say_int((long)di->itemData);
        return TRUE;
    }
    case WM_CLOSE:
        write_state();
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT marker = {0, 0, 4, 4};
        HBRUSH magenta = CreateSolidBrush(RGB(255, 0, 255));
        FillRect(dc, &marker, magenta);
        DeleteObject(magenta);
        EndPaint(hwnd, &ps);
        return 0;
    }
    default:
        return DefWindowProcW(hwnd, m, wParam, lParam);
    }
}

/* ---- Making the controls ---------------------------------------------------------------------- */

static HWND make(const WCHAR *cls, const WCHAR *text, DWORD style, DWORD ex_style, int x, int y, int cx, int cy,
                 int id) {
    HWND h = CreateWindowExW(ex_style, cls, text, style | WS_CHILD | WS_VISIBLE, x, y, cx, cy, g_main,
                             (HMENU)(ULONG_PTR)id, g_instance, NULL);
    SendMessageW(h, WM_SETFONT, (WPARAM)g_font, FALSE);
    return h;
}

static void say_styles(HWND h) {
    line("styles ");
    say_window(h);
    say(" ");
    say_hex((DWORD)GetWindowLongW(h, GWL_STYLE));
    say(" ");
    say_hex((DWORD)GetWindowLongW(h, GWL_EXSTYLE));
    for (HWND c = GetWindow(h, GW_CHILD); c != NULL; c = GetWindow(c, GW_HWNDNEXT)) {
        WCHAR cls[32];
        GetClassNameW(c, cls, 32);
        say(" | child ");
        say_w(cls);
        say(" id ");
        say_int(GetDlgCtrlID(c));
        say(" ");
        say_hex((DWORD)GetWindowLongW(c, GWL_STYLE));
        say(" ");
        say_hex((DWORD)GetWindowLongW(c, GWL_EXSTYLE));
    }
}

static void create_controls(void) {
    g_font = CreateFontW(-11, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, 0, 0, L"MS Shell Dlg");
    g_edit1 = make(L"Edit", L"Hello", WS_BORDER | ES_AUTOHSCROLL | WS_TABSTOP, 0, 10, 10, 120, 20, ID_EDIT1);
    g_edit2 = make(L"Edit", L"", ES_AUTOHSCROLL | ES_NUMBER | WS_TABSTOP, WS_EX_CLIENTEDGE, 10, 40, 120, 21,
                   ID_EDIT2);
    g_list1 = make(L"ListBox", NULL, LBS_NOTIFY | WS_VSCROLL | WS_BORDER | LBS_NOINTEGRALHEIGHT | WS_TABSTOP, 0, 140,
                   10, 100, 80, ID_LIST1);
    g_list2 = make(L"ListBox", NULL, LBS_SORT | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT | WS_TABSTOP, WS_EX_CLIENTEDGE,
                   140, 100, 100, 60, ID_LIST2);
    g_simple = make(L"ComboBox", NULL, CBS_SIMPLE | WS_TABSTOP, 0, 250, 10, 130, 82, ID_SIMPLE);
    g_dropdown = make(L"ComboBox", NULL, CBS_DROPDOWN | CBS_AUTOHSCROLL | WS_VSCROLL | WS_TABSTOP, 0, 250, 100,
                      130, 100, ID_DROPDOWN);
    g_droplist = make(L"ComboBox", NULL, CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, 0, 250, 130, 130, 100,
                      ID_DROPLIST);
    g_owner = make(L"ComboBox", NULL, CBS_DROPDOWNLIST | CBS_OWNERDRAWFIXED | CBS_HASSTRINGS | WS_VSCROLL, 0, 250,
                   160, 130, 100, ID_OWNER);
    g_vscroll = make(L"ScrollBar", NULL, SBS_VERT, 0, 10, 80, 16, 100, ID_VSCROLL);
    g_hscroll = make(L"ScrollBar", NULL, SBS_HORZ, 0, 40, 180, 90, 16, ID_HSCROLL);
    WNDCLASSW wc;
    for (unsigned i = 0; i < sizeof(wc); ++i) {
        ((char *)&wc)[i] = 0;
    }
    wc.lpfnWndProc = pane_proc;
    wc.hInstance = g_instance;
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.lpszClassName = L"Pane";
    RegisterClassW(&wc);
    g_pane = CreateWindowExW(WS_EX_CLIENTEDGE, L"Pane", L"", WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_HSCROLL, 10,
                             210, 120, 70, g_main, (HMENU)ID_PANE, g_instance, NULL);
}

/* ---- What the controls answer ----------------------------------------------------------------- */

static void ask_edit(void) {
    DWORD start = 0;
    DWORD end = 0;
    line("edit1 text ");
    say_text(g_edit1);
    say(" length ");
    say_int(GetWindowTextLengthW(g_edit1));
    SendMessageW(g_edit1, EM_GETSEL, (WPARAM)&start, (LPARAM)&end);
    say(" sel ");
    say_int((long)start);
    say(",");
    say_int((long)end);
    say(" packed ");
    say_hex((DWORD)SendMessageW(g_edit1, EM_GETSEL, 0, 0));
    line("set sel 1,3");
    SendMessageW(g_edit1, EM_SETSEL, 1, 3);
    SendMessageW(g_edit1, EM_GETSEL, (WPARAM)&start, (LPARAM)&end);
    say(" ");
    say_int((long)start);
    say(",");
    say_int((long)end);
    line("set sel 4,2");
    SendMessageW(g_edit1, EM_SETSEL, 4, 2);
    SendMessageW(g_edit1, EM_GETSEL, (WPARAM)&start, (LPARAM)&end);
    say(" ");
    say_int((long)start);
    say(",");
    say_int((long)end);
    line("set sel 0,-1");
    SendMessageW(g_edit1, EM_SETSEL, 0, (LPARAM)-1);
    SendMessageW(g_edit1, EM_GETSEL, (WPARAM)&start, (LPARAM)&end);
    say(" ");
    say_int((long)start);
    say(",");
    say_int((long)end);
    line("modified ");
    say_int(SendMessageW(g_edit1, EM_GETMODIFY, 0, 0));
    SendMessageW(g_edit1, EM_SETSEL, 1, 3);
    line("replace sel");
    SendMessageW(g_edit1, EM_REPLACESEL, TRUE, (LPARAM)L"XY");
    say(" ");
    say_text(g_edit1);
    SendMessageW(g_edit1, EM_GETSEL, (WPARAM)&start, (LPARAM)&end);
    say(" sel ");
    say_int((long)start);
    say(",");
    say_int((long)end);
    say(" modified ");
    say_int(SendMessageW(g_edit1, EM_GETMODIFY, 0, 0));
    say(" can undo ");
    say_int(SendMessageW(g_edit1, EM_CANUNDO, 0, 0));
    line("undo ");
    say_int(SendMessageW(g_edit1, EM_UNDO, 0, 0));
    say(" ");
    say_text(g_edit1);
    line("undo again ");
    say_int(SendMessageW(g_edit1, EM_UNDO, 0, 0));
    say(" ");
    say_text(g_edit1);
    SendMessageW(g_edit1, EM_EMPTYUNDOBUFFER, 0, 0);
    say(" can undo ");
    say_int(SendMessageW(g_edit1, EM_CANUNDO, 0, 0));
    line("limit ");
    say_int(SendMessageW(g_edit1, EM_GETLIMITTEXT, 0, 0));
    SendMessageW(g_edit1, EM_LIMITTEXT, 8, 0);
    say(" now ");
    say_int(SendMessageW(g_edit1, EM_GETLIMITTEXT, 0, 0));
    SendMessageW(g_edit1, EM_SETSEL, 5, 5);
    line("replace past the limit");
    SendMessageW(g_edit1, EM_REPLACESEL, FALSE, (LPARAM)L"123456");
    say(" ");
    say_text(g_edit1);
    line("set text past the limit");
    SetWindowTextW(g_edit1, L"0123456789");
    say(" ");
    say_text(g_edit1);
    SendMessageW(g_edit1, EM_LIMITTEXT, 0, 0);
    line("limit 0 is ");
    say_int(SendMessageW(g_edit1, EM_GETLIMITTEXT, 0, 0));
    line("lines ");
    say_int(SendMessageW(g_edit1, EM_GETLINECOUNT, 0, 0));
    say(" length of line at 3 ");
    say_int(SendMessageW(g_edit1, EM_LINELENGTH, 3, 0));
    say(" index of line 0 ");
    say_int(SendMessageW(g_edit1, EM_LINEINDEX, 0, 0));
    say(" line of char 4 ");
    say_int(SendMessageW(g_edit1, EM_LINEFROMCHAR, 4, 0));
    say(" first visible ");
    say_int(SendMessageW(g_edit1, EM_GETFIRSTVISIBLELINE, 0, 0));
    WCHAR got[16];
    got[0] = 16;
    line("get line ");
    say_int(SendMessageW(g_edit1, EM_GETLINE, 0, (LPARAM)got));
    line("read only");
    SendMessageW(g_edit1, EM_SETREADONLY, TRUE, 0);
    say(" style ");
    say_hex((DWORD)GetWindowLongW(g_edit1, GWL_STYLE));
    SendMessageW(g_edit1, EM_SETREADONLY, FALSE, 0);
    line("password char ");
    say_int(SendMessageW(g_edit1, EM_GETPASSWORDCHAR, 0, 0));
    SetWindowTextW(g_edit1, L"Hello");
    SendMessageW(g_edit1, EM_SETMODIFY, FALSE, 0);
    line("edit2 dialog code ");
    say_hex((DWORD)SendMessageW(g_edit2, WM_GETDLGCODE, 0, 0));
}

static void say_list(HWND h, UINT get_count, UINT get_text) {
    const int n = (int)SendMessageW(h, get_count, 0, 0);
    say(" [");
    for (int i = 0; i < n; ++i) {
        WCHAR text[64];
        SendMessageW(h, get_text, (WPARAM)i, (LPARAM)text);
        say(i != 0 ? "|" : "");
        say_w(text);
    }
    say("]");
}

static void ask_list(void) {
    static const WCHAR *const k_items[] = {L"Item 1", L"Item 2", L"Item 3", L"Item 4",  L"Item 5",  L"Item 6",
                                           L"Item 7", L"Item 8", L"Item 9", L"Item 10", L"Item 11", L"Item 12"};
    line("list1 add");
    for (int i = 0; i < 12; ++i) {
        say(" ");
        say_int(SendMessageW(g_list1, LB_ADDSTRING, 0, (LPARAM)k_items[i]));
    }
    line("insert at 1 ");
    say_int(SendMessageW(g_list1, LB_INSERTSTRING, 1, (LPARAM)L"Inserted"));
    say(" at -1 ");
    say_int(SendMessageW(g_list1, LB_INSERTSTRING, (WPARAM)-1, (LPARAM)L"Last"));
    say(" at 99 ");
    say_int(SendMessageW(g_list1, LB_INSERTSTRING, 99, (LPARAM)L"Far"));
    line("count ");
    say_int(SendMessageW(g_list1, LB_GETCOUNT, 0, 0));
    say(" length of 1 ");
    say_int(SendMessageW(g_list1, LB_GETTEXTLEN, 1, 0));
    say(" of 99 ");
    say_int(SendMessageW(g_list1, LB_GETTEXTLEN, 99, 0));
    line("delete 1 ");
    say_int(SendMessageW(g_list1, LB_DELETESTRING, 1, 0));
    say(" last ");
    say_int(SendMessageW(g_list1, LB_DELETESTRING, 12, 0));
    say(" 99 ");
    say_int(SendMessageW(g_list1, LB_DELETESTRING, 99, 0));
    line("list1");
    say_list(g_list1, LB_GETCOUNT, LB_GETTEXT);
    line("find item 1 ");
    say_int(SendMessageW(g_list1, LB_FINDSTRING, (WPARAM)-1, (LPARAM)L"item 1"));
    say(" after 0 ");
    say_int(SendMessageW(g_list1, LB_FINDSTRING, 0, (LPARAM)L"item 1"));
    say(" exact ");
    say_int(SendMessageW(g_list1, LB_FINDSTRINGEXACT, 0, (LPARAM)L"item 1"));
    say(" none ");
    say_int(SendMessageW(g_list1, LB_FINDSTRING, (WPARAM)-1, (LPARAM)L"zzz"));
    line("select item 7 ");
    say_int(SendMessageW(g_list1, LB_SELECTSTRING, (WPARAM)-1, (LPARAM)L"Item 7"));
    say(" cur ");
    say_int(SendMessageW(g_list1, LB_GETCURSEL, 0, 0));
    line("set cur 2 ");
    say_int(SendMessageW(g_list1, LB_SETCURSEL, 2, 0));
    say(" -1 ");
    say_int(SendMessageW(g_list1, LB_SETCURSEL, (WPARAM)-1, 0));
    say(" cur ");
    say_int(SendMessageW(g_list1, LB_GETCURSEL, 0, 0));
    say(" 99 ");
    say_int(SendMessageW(g_list1, LB_SETCURSEL, 99, 0));
    line("data ");
    say_int(SendMessageW(g_list1, LB_GETITEMDATA, 3, 0));
    SendMessageW(g_list1, LB_SETITEMDATA, 3, 77);
    say(" ");
    say_int(SendMessageW(g_list1, LB_GETITEMDATA, 3, 0));
    say(" of 99 ");
    say_int(SendMessageW(g_list1, LB_GETITEMDATA, 99, 0));
    line("item height ");
    say_int(SendMessageW(g_list1, LB_GETITEMHEIGHT, 0, 0));
    line("top ");
    say_int(SendMessageW(g_list1, LB_GETTOPINDEX, 0, 0));
    SendMessageW(g_list1, LB_SETTOPINDEX, 3, 0);
    say(" after setting 3 ");
    say_int(SendMessageW(g_list1, LB_GETTOPINDEX, 0, 0));
    SendMessageW(g_list1, LB_SETTOPINDEX, 50, 0);
    say(" after setting 50 ");
    say_int(SendMessageW(g_list1, LB_GETTOPINDEX, 0, 0));
    SendMessageW(g_list1, LB_SETTOPINDEX, 0, 0);
    line("sel of 2 ");
    say_int(SendMessageW(g_list1, LB_GETSEL, 2, 0));
    SendMessageW(g_list1, LB_SETCURSEL, 2, 0);
    say(" after ");
    say_int(SendMessageW(g_list1, LB_GETSEL, 2, 0));
    SendMessageW(g_list1, LB_SETCURSEL, (WPARAM)-1, 0);
    static const WCHAR *const k_unsorted[] = {L"pear", L"Apple", L"banana", L"apple", L"Cherry"};
    line("list2 add");
    for (int i = 0; i < 5; ++i) {
        say(" ");
        say_int(SendMessageW(g_list2, LB_ADDSTRING, 0, (LPARAM)k_unsorted[i]));
    }
    line("list2");
    say_list(g_list2, LB_GETCOUNT, LB_GETTEXT);
    line("list2 insert at 0 ");
    say_int(SendMessageW(g_list2, LB_INSERTSTRING, 0, (LPARAM)L"zebra"));
    say_list(g_list2, LB_GETCOUNT, LB_GETTEXT);
    line("list2 find b ");
    say_int(SendMessageW(g_list2, LB_FINDSTRING, (WPARAM)-1, (LPARAM)L"b"));
}

static void ask_combo(void) {
    static const WCHAR *const k_items[] = {L"One", L"Two", L"Three", L"Four", L"Five", L"Six", L"Seven", L"Eight"};
    static HWND *const k_combos[] = {&g_simple, &g_dropdown, &g_droplist, &g_owner};
    g_quiet = 1;
    for (int c = 0; c < 4; ++c) {
        HWND h = *k_combos[c];
        line("add to ");
        say_window(h);
        for (int i = 0; i < 8; ++i) {
            say(" ");
            say_int(SendMessageW(h, CB_ADDSTRING, 0, (LPARAM)k_items[i]));
        }
        SendMessageW(h, CB_SETITEMDATA, 2, 300 + c);
    }
    g_quiet = 0;
    line("dropdown");
    say_list(g_dropdown, CB_GETCOUNT, CB_GETLBTEXT);
    line("cur ");
    say_int(SendMessageW(g_dropdown, CB_GETCURSEL, 0, 0));
    say(" text ");
    say_text(g_dropdown);
    line("set cur 2 ");
    say_int(SendMessageW(g_dropdown, CB_SETCURSEL, 2, 0));
    say(" text ");
    say_text(g_dropdown);
    say(" droplist ");
    say_int(SendMessageW(g_droplist, CB_SETCURSEL, 3, 0));
    say(" ");
    say_text(g_droplist);
    say(" simple ");
    say_int(SendMessageW(g_simple, CB_SETCURSEL, 1, 0));
    say(" ");
    say_text(g_simple);
    line("set cur 99 ");
    say_int(SendMessageW(g_dropdown, CB_SETCURSEL, 99, 0));
    say(" text ");
    say_text(g_dropdown);
    say(" cur ");
    say_int(SendMessageW(g_dropdown, CB_GETCURSEL, 0, 0));
    SendMessageW(g_dropdown, CB_SETCURSEL, 2, 0);
    line("find t ");
    say_int(SendMessageW(g_dropdown, CB_FINDSTRING, (WPARAM)-1, (LPARAM)L"t"));
    say(" after 1 ");
    say_int(SendMessageW(g_dropdown, CB_FINDSTRING, 1, (LPARAM)L"t"));
    say(" exact ");
    say_int(SendMessageW(g_dropdown, CB_FINDSTRINGEXACT, (WPARAM)-1, (LPARAM)L"FIVE"));
    line("select fi ");
    say_int(SendMessageW(g_dropdown, CB_SELECTSTRING, (WPARAM)-1, (LPARAM)L"fi"));
    say(" text ");
    say_text(g_dropdown);
    line("text length of 2 ");
    say_int(SendMessageW(g_dropdown, CB_GETLBTEXTLEN, 2, 0));
    say(" of 99 ");
    say_int(SendMessageW(g_dropdown, CB_GETLBTEXTLEN, 99, 0));
    line("data of 2 ");
    say_int(SendMessageW(g_droplist, CB_GETITEMDATA, 2, 0));
    say(" of 99 ");
    say_int(SendMessageW(g_droplist, CB_GETITEMDATA, 99, 0));
    line("insert at 1 ");
    say_int(SendMessageW(g_droplist, CB_INSERTSTRING, 1, (LPARAM)L"Inserted"));
    say(" delete 1 ");
    say_int(SendMessageW(g_droplist, CB_DELETESTRING, 1, 0));
    say(" cur ");
    say_int(SendMessageW(g_droplist, CB_GETCURSEL, 0, 0));
    line("dropped ");
    say_int(SendMessageW(g_dropdown, CB_GETDROPPEDSTATE, 0, 0));
    say(" extended ui ");
    say_int(SendMessageW(g_dropdown, CB_GETEXTENDEDUI, 0, 0));
    line("edit sel ");
    DWORD start = 0;
    DWORD end = 0;
    say_hex((DWORD)SendMessageW(g_dropdown, CB_GETEDITSEL, (WPARAM)&start, (LPARAM)&end));
    say(" ");
    say_int((long)start);
    say(",");
    say_int((long)end);
    say(" on a list ");
    say_int(SendMessageW(g_droplist, CB_GETEDITSEL, 0, 0));
    line("set edit sel ");
    say_int(SendMessageW(g_dropdown, CB_SETEDITSEL, 0, MAKELPARAM(1, 2)));
    say(" on a list ");
    say_int(SendMessageW(g_droplist, CB_SETEDITSEL, 0, MAKELPARAM(1, 2)));
    line("limit ");
    say_int(SendMessageW(g_dropdown, CB_LIMITTEXT, 20, 0));
    say(" on a list ");
    say_int(SendMessageW(g_droplist, CB_LIMITTEXT, 20, 0));
    line("set text ");
    SetWindowTextW(g_dropdown, L"Typed");
    say_text(g_dropdown);
    say(" cur ");
    say_int(SendMessageW(g_dropdown, CB_GETCURSEL, 0, 0));
    say(" list ");
    SetWindowTextW(g_droplist, L"Two");
    say_text(g_droplist);
    say(" cur ");
    say_int(SendMessageW(g_droplist, CB_GETCURSEL, 0, 0));
    SendMessageW(g_dropdown, CB_SETCURSEL, 2, 0);
    line("top ");
    say_int(SendMessageW(g_dropdown, CB_GETTOPINDEX, 0, 0));
    static HWND *const k_all[] = {&g_edit1, &g_edit2, &g_list1, &g_list2, &g_simple, &g_dropdown,
                                  &g_droplist, &g_owner, &g_vscroll, &g_hscroll, &g_pane};
    for (unsigned i = 0; i < sizeof(k_all) / sizeof(k_all[0]); ++i) {
        say_styles(*k_all[i]);
    }
    static HWND *const k_sizes[] = {&g_simple, &g_dropdown, &g_droplist, &g_owner};
    for (int c = 0; c < 4; ++c) {
        RECT r = window_rect(*k_sizes[c]);
        line("size ");
        say_window(*k_sizes[c]);
        say(" ");
        say_int(r.right - r.left);
        say("x");
        say_int(r.bottom - r.top);
        say(" field ");
        say_int(SendMessageW(*k_sizes[c], CB_GETITEMHEIGHT, (WPARAM)-1, 0));
        say(" item ");
        say_int(SendMessageW(*k_sizes[c], CB_GETITEMHEIGHT, 0, 0));
        RECT d;
        SendMessageW(*k_sizes[c], CB_GETDROPPEDCONTROLRECT, 0, (LPARAM)&d);
        say(" dropped ");
        say_int(d.right - d.left);
        say("x");
        say_int(d.bottom - d.top);
    }
}

static void ask_scroll(void) {
    SCROLLINFO si;
    si.cbSize = sizeof(si);
    si.fMask = SIF_ALL;
    GetScrollInfo(g_vscroll, SB_CTL, &si);
    line("vscroll at first ");
    say_int(si.nMin);
    say("..");
    say_int(si.nMax);
    say(" page ");
    say_int((long)si.nPage);
    say(" pos ");
    say_int(si.nPos);
    SetScrollRange(g_vscroll, SB_CTL, 0, 20, FALSE);
    si.fMask = SIF_PAGE;
    si.nPage = 5;
    SetScrollInfo(g_vscroll, SB_CTL, &si, TRUE);
    SetScrollRange(g_hscroll, SB_CTL, 0, 10, TRUE);
    SetScrollPos(g_hscroll, SB_CTL, 3, TRUE);
    si.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
    si.nMin = 0;
    si.nMax = 30;
    si.nPage = 10;
    si.nPos = 4;
    SetScrollInfo(g_pane, SB_VERT, &si, TRUE);
    si.nMax = 20;
    si.nPage = 5;
    si.nPos = 0;
    SetScrollInfo(g_pane, SB_HORZ, &si, TRUE);
    line("pane after ");
    RECT c;
    GetClientRect(g_pane, &c);
    say_int(c.right);
    say("x");
    say_int(c.bottom);
    line("vscroll range by message ");
    int lo = 0;
    int hi = 0;
    SendMessageW(g_vscroll, 0x00e3 /* SBM_GETRANGE */, (WPARAM)&lo, (LPARAM)&hi);
    say_int(lo);
    say("..");
    say_int(hi);
    say(" pos ");
    say_int(SendMessageW(g_vscroll, 0x00e1 /* SBM_GETPOS */, 0, 0));
}

void start(void) {
    g_instance = GetModuleHandleW(NULL);
    say("controls.exe");
    /* A caret that blinks once in ten seconds is on the screen at every
     * dump, wherever its blink had come to on each system. */
    const UINT blink = GetCaretBlinkTime();
    SetCaretBlinkTime(10000);
    WNDCLASSW wc;
    for (unsigned i = 0; i < sizeof(wc); ++i) {
        ((char *)&wc)[i] = 0;
    }
    wc.lpfnWndProc = main_proc;
    wc.hInstance = g_instance;
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = L"Controls";
    RegisterClassW(&wc);
    RECT r = {0, 0, CLIENT_WIDTH, CLIENT_HEIGHT};
    AdjustWindowRect(&r, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, FALSE);
    g_main = CreateWindowExW(0, L"Controls", L"Controls", WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, CW_USEDEFAULT,
                             CW_USEDEFAULT, r.right - r.left, r.bottom - r.top, NULL, NULL, g_instance, NULL);
    create_controls();
    ask_edit();
    ask_list();
    ask_combo();
    ask_scroll();
    line("shown");
    ShowWindow(g_main, SW_SHOW);
    UpdateWindow(g_main);
    MSG m;
    while (GetMessageW(&m, NULL, 0, 0) > 0) {
        if (m.message == WM_KEYDOWN && m.wParam == VK_F2) {
            write_pixels();
            continue;
        }
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    SetCaretBlinkTime(blink);
    say("\r\n");
    ExitProcess(0);
}
