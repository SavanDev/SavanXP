/*
 * dialog.exe -- The W5 fixture: a window with a menu bar and accelerators,
 * two modal dialogs made of the controls sol.exe's dialogs use, a message
 * box and the shell's About box, driven by an input script (input.txt next
 * to this file) by pointer and by keyboard alone.
 *
 * Its resources (dialog.rc) have the kinds and styles of sol.exe's: a menu
 * of two popups with separators and an accelerator shown after a tab, an
 * accelerator table, a dialog of owner-drawn buttons with a default and a
 * cancel button (101, like Deck), and one of group boxes, radio buttons the
 * program checks itself, automatic check boxes and static text (103, like
 * Options).
 *
 * It writes the messages its window and dialog procedures receive, one per
 * line (the list is in logged(); with the argument "all", every message),
 * where each dialog's controls are, what they say when it closes, and some
 * of their pixels read back with GetPixel: the frames of the buttons and the
 * group box, the check box and radio button glyphs, and the owner-drawn
 * buttons. No text pixel is read: glyphs are the platform's.
 *
 * The main window's client area is 360x240 and starts with a 4x4 magenta
 * marker; a dialog's starts with a cyan one. The input drivers find each
 * window by its marker on a screenshot.
 */
#include <windows.h>

#define CLIENT_WIDTH 360
#define CLIENT_HEIGHT 240

#define ID_NEW 1000
#define ID_UNDO 1001
#define ID_BACKS 1002
#define ID_OPTIONS 1003
#define ID_EXIT 1004
#define ID_CONTENTS 1005
#define ID_WIN 1010
#define ID_ABOUT 2000

static int g_all;
static HINSTANCE g_instance;
static HWND g_main;
static HWND g_dialog;
static HMENU g_menu;
static HMENU g_popups[2];
static HBRUSH g_green;
static HBRUSH g_magenta;
static HBRUSH g_cyan;
static HBRUSH g_yellow;
static int g_undo;
static int g_back = 55;
static int g_draw_three;
static int g_scoring = 302;

/* ---- Output ---------------------------------------------------------------------- */

static char g_out[4096];
static DWORD g_used;

static void flush(void) {
    DWORD written;
    if (g_used != 0) {
        WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), g_out, g_used, &written, NULL);
        g_used = 0;
    }
}

static void say(const char *text) {
    while (*text != 0) {
        if (g_used == sizeof(g_out)) {
            flush();
        }
        g_out[g_used++] = *text++;
    }
}

static void say_int(int value) {
    char digits[12];
    int n = 11;
    unsigned magnitude = value < 0 ? 0u - (unsigned)value : (unsigned)value;
    digits[n] = 0;
    do {
        digits[--n] = (char)('0' + magnitude % 10u);
        magnitude /= 10u;
    } while (magnitude != 0u);
    if (value < 0) {
        digits[--n] = '-';
    }
    say(digits + n);
}

static void say_hex(unsigned value, int width) {
    char digits[9];
    for (int i = 0; i < width; ++i) {
        unsigned nibble = (value >> (4 * (width - 1 - i))) & 0xfu;
        digits[i] = (char)(nibble < 10u ? '0' + nibble : 'a' + nibble - 10u);
    }
    digits[width] = 0;
    say(digits);
}

/* ---- Names -------------------------------------------------------------------------- */

/* The controls of both dialogs; -1 (the group boxes) is never looked up. */
static const int k_ids[] = {54, 55, 56, 57, 58, 59, 300, 301, 302, 303, 304, 305, 306, 307, 308, 310, 311, 1, 2};

static int control_id(HWND h) {
    if (g_dialog == NULL || h == NULL) {
        return 0;
    }
    for (unsigned i = 0; i < sizeof(k_ids) / sizeof(k_ids[0]); ++i) {
        if (GetDlgItem(g_dialog, k_ids[i]) == h) {
            return k_ids[i];
        }
    }
    return 0;
}

static void say_window(HWND h) {
    int id;
    if (h == NULL) {
        say("none");
    } else if (h == g_main) {
        say("main");
    } else if (h == g_dialog) {
        say("dialog");
    } else if ((id = control_id(h)) != 0) {
        say("control ");
        say_int(id);
    } else {
        say("other");
    }
}

static void say_menu(HMENU h) {
    if (h == NULL) {
        say("none");
    } else if (h == g_menu) {
        say("bar");
    } else if (h == g_popups[0]) {
        say("game");
    } else if (h == g_popups[1]) {
        say("help");
    } else {
        say("other");
    }
}

static const struct {
    UINT message;
    const char *name;
} k_names[] = {
    {WM_CREATE, "WM_CREATE"},
    {WM_DESTROY, "WM_DESTROY"},
    {WM_MOVE, "WM_MOVE"},
    {WM_SIZE, "WM_SIZE"},
    {WM_ACTIVATE, "WM_ACTIVATE"},
    {WM_SETFOCUS, "WM_SETFOCUS"},
    {WM_KILLFOCUS, "WM_KILLFOCUS"},
    {WM_ENABLE, "WM_ENABLE"},
    {WM_SETTEXT, "WM_SETTEXT"},
    {WM_GETTEXT, "WM_GETTEXT"},
    {WM_GETTEXTLENGTH, "WM_GETTEXTLENGTH"},
    {WM_PAINT, "WM_PAINT"},
    {WM_CLOSE, "WM_CLOSE"},
    {WM_ERASEBKGND, "WM_ERASEBKGND"},
    {WM_SHOWWINDOW, "WM_SHOWWINDOW"},
    {WM_ACTIVATEAPP, "WM_ACTIVATEAPP"},
    {WM_CANCELMODE, "WM_CANCELMODE"},
    {WM_SETCURSOR, "WM_SETCURSOR"},
    {WM_MOUSEACTIVATE, "WM_MOUSEACTIVATE"},
    {WM_GETMINMAXINFO, "WM_GETMINMAXINFO"},
    {WM_NEXTDLGCTL, "WM_NEXTDLGCTL"},
    {WM_DRAWITEM, "WM_DRAWITEM"},
    {WM_SETFONT, "WM_SETFONT"},
    {WM_GETFONT, "WM_GETFONT"},
    {WM_WINDOWPOSCHANGING, "WM_WINDOWPOSCHANGING"},
    {WM_WINDOWPOSCHANGED, "WM_WINDOWPOSCHANGED"},
    {WM_HELP, "WM_HELP"},
    {WM_NOTIFYFORMAT, "WM_NOTIFYFORMAT"},
    {WM_CONTEXTMENU, "WM_CONTEXTMENU"},
    {WM_GETICON, "WM_GETICON"},
    {WM_NCCREATE, "WM_NCCREATE"},
    {WM_NCDESTROY, "WM_NCDESTROY"},
    {WM_NCCALCSIZE, "WM_NCCALCSIZE"},
    {WM_NCHITTEST, "WM_NCHITTEST"},
    {WM_NCPAINT, "WM_NCPAINT"},
    {WM_NCACTIVATE, "WM_NCACTIVATE"},
    {WM_GETDLGCODE, "WM_GETDLGCODE"},
    {WM_KEYDOWN, "WM_KEYDOWN"},
    {WM_KEYUP, "WM_KEYUP"},
    {WM_CHAR, "WM_CHAR"},
    {WM_SYSKEYDOWN, "WM_SYSKEYDOWN"},
    {WM_SYSKEYUP, "WM_SYSKEYUP"},
    {WM_SYSCHAR, "WM_SYSCHAR"},
    {WM_INITDIALOG, "WM_INITDIALOG"},
    {WM_COMMAND, "WM_COMMAND"},
    {WM_SYSCOMMAND, "WM_SYSCOMMAND"},
    {WM_TIMER, "WM_TIMER"},
    {WM_INITMENU, "WM_INITMENU"},
    {WM_INITMENUPOPUP, "WM_INITMENUPOPUP"},
    {WM_MENUSELECT, "WM_MENUSELECT"},
    {WM_MENUCHAR, "WM_MENUCHAR"},
    {WM_ENTERIDLE, "WM_ENTERIDLE"},
    {WM_UNINITMENUPOPUP, "WM_UNINITMENUPOPUP"},
    {WM_CHANGEUISTATE, "WM_CHANGEUISTATE"},
    {WM_UPDATEUISTATE, "WM_UPDATEUISTATE"},
    {WM_QUERYUISTATE, "WM_QUERYUISTATE"},
    {WM_CTLCOLORMSGBOX, "WM_CTLCOLORMSGBOX"},
    {WM_CTLCOLORBTN, "WM_CTLCOLORBTN"},
    {WM_CTLCOLORDLG, "WM_CTLCOLORDLG"},
    {WM_CTLCOLORSTATIC, "WM_CTLCOLORSTATIC"},
    {WM_MOUSEMOVE, "WM_MOUSEMOVE"},
    {WM_LBUTTONDOWN, "WM_LBUTTONDOWN"},
    {WM_LBUTTONUP, "WM_LBUTTONUP"},
    {WM_LBUTTONDBLCLK, "WM_LBUTTONDBLCLK"},
    {WM_RBUTTONDOWN, "WM_RBUTTONDOWN"},
    {WM_RBUTTONUP, "WM_RBUTTONUP"},
    {WM_PARENTNOTIFY, "WM_PARENTNOTIFY"},
    {WM_NCMOUSEMOVE, "WM_NCMOUSEMOVE"},
    {WM_NCLBUTTONDOWN, "WM_NCLBUTTONDOWN"},
    {WM_NCLBUTTONUP, "WM_NCLBUTTONUP"},
    {DM_GETDEFID, "DM_GETDEFID"},
    {WM_ENTERMENULOOP, "WM_ENTERMENULOOP"},
    {WM_EXITMENULOOP, "WM_EXITMENULOOP"},
    {WM_CAPTURECHANGED, "WM_CAPTURECHANGED"},
    {WM_QUERYNEWPALETTE, "WM_QUERYNEWPALETTE"},
};

/* What is written by default, chosen from what "all" showed on Windows XP:
 * the messages a program acts on and that come from what it and the user
 * do. Left out: what the window manager decides (the frame's painting and
 * activation, WM_GETTEXT for the caption, the WM_WINDOWPOS* pairs, the input
 * method's), what timing decides (WM_ENTERIDLE, pointer moves, hit tests,
 * cursor queries), paint messages (Windows XP repaints the main window where
 * a dialog covered it; SavanXP's compositor does not need to) and the
 * WM_CTLCOLOR* every paint brings. */
static int logged(UINT message) {
    switch (message) {
    case WM_CREATE:
    case WM_DESTROY:
    case WM_ACTIVATE:
    case WM_SETFOCUS:
    case WM_KILLFOCUS:
    case WM_ENABLE:
    case WM_SHOWWINDOW:
    case WM_CANCELMODE:
    case WM_SETFONT:
    case WM_CHANGEUISTATE:
    case WM_UPDATEUISTATE:
    case DM_GETDEFID:
    case WM_NCLBUTTONDOWN:
    case WM_CLOSE:
    case WM_NCDESTROY:
    case WM_KEYDOWN:
    case WM_KEYUP:
    case WM_CHAR:
    case WM_SYSKEYDOWN:
    case WM_SYSKEYUP:
    case WM_SYSCHAR:
    case WM_INITDIALOG:
    case WM_COMMAND:
    case WM_SYSCOMMAND:
    case WM_DRAWITEM:
    case WM_INITMENU:
    case WM_INITMENUPOPUP:
    case WM_MENUSELECT:
    case WM_MENUCHAR:
    case WM_UNINITMENUPOPUP:
    case WM_ENTERMENULOOP:
    case WM_EXITMENULOOP:
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
    case WM_CAPTURECHANGED:
        return 1;
    default:
        return 0;
    }
}

static void log_message(const char *who, UINT message, WPARAM wParam, LPARAM lParam) {
    if (!g_all && !logged(message)) {
        return;
    }
    say(who);
    const char *name = 0;
    for (unsigned i = 0; i < sizeof(k_names) / sizeof(k_names[0]); ++i) {
        if (k_names[i].message == message) {
            name = k_names[i].name;
        }
    }
    if (name != 0) {
        say(name);
    } else {
        say("0x");
        say_hex(message, 4);
    }
    switch (message) {
    case WM_ACTIVATE:
        say(" ");
        say_int(LOWORD(wParam));
        say(" ");
        say_window((HWND)lParam);
        break;
    case WM_SETFOCUS:
    case WM_KILLFOCUS:
        say(" ");
        say_window((HWND)wParam);
        break;
    case WM_ENABLE:
        say(" ");
        say_int((int)wParam);
        break;
    case WM_SHOWWINDOW:
        say(" ");
        say_int((int)wParam);
        say(" ");
        say_int((int)lParam);
        break;
    case WM_NCLBUTTONDOWN:
    case WM_NCLBUTTONUP:
    case WM_NCMOUSEMOVE:
        say(" hit ");
        say_int((int)wParam);
        break;
    case WM_SETFONT:
        say(wParam != 0 ? " a font" : " none");
        say(lParam != 0 ? " redraw" : "");
        break;
    case WM_CANCELMODE:
    case DM_GETDEFID:
        break;
    case WM_KEYDOWN:
    case WM_KEYUP:
    case WM_SYSKEYDOWN:
    case WM_SYSKEYUP:
        say(" 0x");
        say_hex(wParam, 2);
        break;
    case WM_CHAR:
    case WM_SYSCHAR:
        say(" 0x");
        say_hex(wParam, 4);
        break;
    case WM_SYSCOMMAND:
        say(" 0x");
        say_hex(wParam & 0xfff0u, 4);
        if ((wParam & 0xfff0u) == SC_KEYMENU) {
            say(" 0x");
            say_hex((unsigned)lParam, 4);
        }
        break;
    case WM_INITDIALOG:
        say(" focus ");
        say_window((HWND)wParam);
        say(" param ");
        say_int((int)lParam);
        break;
    case WM_COMMAND:
        say(" ");
        say_int(LOWORD(wParam));
        say(" code ");
        say_int(HIWORD(wParam));
        say(" from ");
        say_window((HWND)lParam);
        break;
    case WM_DRAWITEM: {
        const DRAWITEMSTRUCT *d = (const DRAWITEMSTRUCT *)lParam;
        say(" type ");
        say_int((int)d->CtlType);
        say(" id ");
        say_int((int)d->CtlID);
        say(" action 0x");
        say_hex(d->itemAction, 2);
        say(" state 0x");
        say_hex(d->itemState, 4);
        say(" ");
        say_int(d->rcItem.left);
        say(",");
        say_int(d->rcItem.top);
        say(" ");
        say_int(d->rcItem.right);
        say(",");
        say_int(d->rcItem.bottom);
        say(d->hwndItem == GetDlgItem(g_dialog, (int)d->CtlID) ? " its window" : " another window");
        break;
    }
    case WM_INITMENU:
    case WM_UNINITMENUPOPUP:
        say(" ");
        say_menu((HMENU)wParam);
        break;
    case WM_INITMENUPOPUP:
        say(" ");
        say_menu((HMENU)wParam);
        say(" index ");
        say_int(LOWORD(lParam));
        say(HIWORD(lParam) ? " system" : "");
        break;
    case WM_MENUSELECT:
        say(" item ");
        say_int(LOWORD(wParam));
        say(" flags 0x");
        say_hex(HIWORD(wParam), 4);
        say(" ");
        say_menu((HMENU)lParam);
        break;
    case WM_MENUCHAR:
        say(" 0x");
        say_hex(LOWORD(wParam), 4);
        say(" flags 0x");
        say_hex(HIWORD(wParam), 4);
        say(" ");
        say_menu((HMENU)lParam);
        break;
    case WM_ENTERMENULOOP:
    case WM_EXITMENULOOP:
        say(" ");
        say_int((int)wParam);
        break;
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
        say(" ");
        say_int(GET_X_LPARAM(lParam));
        say(",");
        say_int(GET_Y_LPARAM(lParam));
        break;
    case WM_CAPTURECHANGED:
        say(" to ");
        say_window((HWND)lParam);
        break;
    case WM_CHANGEUISTATE:
    case WM_UPDATEUISTATE:
        say(" 0x");
        say_hex(wParam, 8);
        break;
    case WM_CTLCOLORBTN:
    case WM_CTLCOLORDLG:
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORMSGBOX:
        say(" ");
        say_window((HWND)lParam);
        break;
    default:
        if (g_all) {
            say(" 0x");
            say_hex(wParam, 8);
            say(" 0x");
            say_hex((unsigned)lParam, 8);
        }
        break;
    }
    say("\n");
}

/* ---- Drawing and reading back ---------------------------------------------------------- */

static void fill(HDC dc, HBRUSH brush, int left, int top, int width, int height) {
    HGDIOBJ old = SelectObject(dc, brush);
    PatBlt(dc, left, top, width, height, PATCOPY);
    SelectObject(dc, old);
}

static void marker(HWND hwnd, HBRUSH brush) {
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(hwnd, &ps);
    fill(dc, brush, 0, 0, 4, 4);
    EndPaint(hwnd, &ps);
}

/* Where a control is in its dialog's client area, and its size. */
static void layout(HWND dialog) {
    POINT origin = {0, 0};
    ClientToScreen(dialog, &origin);
    RECT client;
    GetClientRect(dialog, &client);
    say("layout client ");
    say_int(client.right);
    say("x");
    say_int(client.bottom);
    say("\n");
    for (unsigned i = 0; i < sizeof(k_ids) / sizeof(k_ids[0]); ++i) {
        HWND control = GetDlgItem(dialog, k_ids[i]);
        if (control == NULL) {
            continue;
        }
        POINT at = {0, 0};
        ClientToScreen(control, &at);
        RECT r;
        GetClientRect(control, &r);
        say("layout ");
        say_int(k_ids[i]);
        say(" ");
        say_int(at.x - origin.x);
        say(",");
        say_int(at.y - origin.y);
        say(" ");
        say_int(r.right);
        say("x");
        say_int(r.bottom);
        say("\n");
    }
}

/* Rows top to bottom - 1 of a control's columns left to right - 1, as runs
 * "RRGGBB*count"; a row like the one above it is written "=". bottom and
 * right count from the far edge when they are not positive, and top does
 * when it is negative. */
static void pixels(HWND dialog, int id, int left, int top, int right, int bottom) {
    HWND control = GetDlgItem(dialog, id);
    RECT r;
    GetClientRect(control, &r);
    if (right <= 0) {
        right += r.right;
    }
    if (bottom <= 0) {
        bottom += r.bottom;
    }
    if (top < 0) {
        top += r.bottom;
    }
    HDC dc = GetDC(control);
    static COLORREF row[256];
    static COLORREF above[256];
    say("pixels ");
    say_int(id);
    say(" ");
    say_int(left);
    say(",");
    say_int(top);
    say(" ");
    say_int(right);
    say(",");
    say_int(bottom);
    say("\n");
    for (int y = top; y < bottom; ++y) {
        int same = y != top;
        for (int x = left; x < right; ++x) {
            row[x - left] = GetPixel(dc, x, y);
            same = same && row[x - left] == above[x - left];
        }
        say("  ");
        say_int(y);
        if (same) {
            say(" =\n");
            continue;
        }
        for (int x = 0; x < right - left;) {
            int run = 1;
            while (x + run < right - left && row[x + run] == row[x]) {
                ++run;
            }
            say(" ");
            say_hex(GetRValue(row[x]) << 16 | GetGValue(row[x]) << 8 | GetBValue(row[x]), 6);
            say("*");
            say_int(run);
            x += run;
        }
        say("\n");
        for (int x = 0; x < right - left; ++x) {
            above[x] = row[x];
        }
    }
    ReleaseDC(control, dc);
}

/* ---- The back dialog (101) ---------------------------------------------------------------- */

static const COLORREF k_backs[] = {RGB(0, 0, 160), RGB(160, 0, 0), RGB(0, 120, 0),
                                   RGB(160, 120, 0), RGB(96, 0, 128), RGB(0, 120, 160)};

static void draw_back(const DRAWITEMSTRUCT *d) {
    RECT r;
    CopyRect(&r, &d->rcItem);
    HBRUSH face = CreateSolidBrush(GetSysColor(COLOR_BTNFACE));
    HBRUSH back = CreateSolidBrush(k_backs[d->CtlID - 54]);
    HBRUSH highlight = CreateSolidBrush(GetSysColor(COLOR_HIGHLIGHT));
    fill(d->hDC, face, r.left, r.top, r.right - r.left, r.bottom - r.top);
    if ((int)d->CtlID == g_back) {
        FrameRect(d->hDC, &r, highlight);
        InflateRect(&r, -1, -1);
        FrameRect(d->hDC, &r, highlight);
    }
    InflateRect(&r, -3, -3);
    if ((int)d->CtlID == g_back) {
        InflateRect(&r, 1, 1);
    }
    fill(d->hDC, back, r.left, r.top, r.right - r.left, r.bottom - r.top);
    if ((d->itemState & ODS_FOCUS) != 0) {
        InflateRect(&r, -3, -3);
        FrameRect(d->hDC, &r, (HBRUSH)GetStockObject(WHITE_BRUSH));
    }
    if ((d->itemState & ODS_SELECTED) != 0) {
        InflateRect(&r, -2, -2);
        InvertRect(d->hDC, &r);
    }
    DeleteObject(face);
    DeleteObject(back);
    DeleteObject(highlight);
}

static INT_PTR WINAPI backs_proc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_INITDIALOG) {
        g_dialog = hwnd;
    }
    log_message("backs ", message, wParam, lParam);
    switch (message) {
    case WM_INITDIALOG:
        layout(hwnd);
        return TRUE;
    case WM_PAINT:
        marker(hwnd, g_cyan);
        return TRUE;
    case WM_DRAWITEM:
        draw_back((const DRAWITEMSTRUCT *)lParam);
        return TRUE;
    case WM_COMMAND: {
        int id = LOWORD(wParam);
        if (id >= 54 && id <= 59) {
            InvalidateRect(GetDlgItem(hwnd, g_back), NULL, FALSE);
            g_back = id;
            InvalidateRect(GetDlgItem(hwnd, id), NULL, FALSE);
            if (HIWORD(wParam) == BN_DOUBLECLICKED) {
                say("backs double click\n");
                EndDialog(hwnd, IDOK);
            }
            return TRUE;
        }
        if (id == IDOK || id == IDCANCEL) {
            flush();
            pixels(hwnd, g_back, 0, 0, 0, 0);
            pixels(hwnd, 54, 0, 0, 0, 0);
            pixels(hwnd, IDOK, 0, 0, 0, 3);
            pixels(hwnd, IDOK, 0, -3, 0, 0);
            pixels(hwnd, IDCANCEL, 0, 0, 0, 3);
            EndDialog(hwnd, id);
            say("backs ended\n");
            return TRUE;
        }
        return FALSE;
    }
    }
    return FALSE;
}

/* ---- The settings dialog (103) -------------------------------------------------------------- */

static void enable_keep(HWND hwnd) {
    EnableWindow(GetDlgItem(hwnd, 308), g_scoring == 303);
    EnableWindow(GetDlgItem(hwnd, 310), g_scoring == 303);
}

static void states(HWND hwnd) {
    say("settings states");
    for (int id = 300; id <= 308; ++id) {
        say(" ");
        say_int((int)IsDlgButtonChecked(hwnd, id));
    }
    say("\n");
}

static INT_PTR WINAPI settings_proc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_INITDIALOG) {
        g_dialog = hwnd;
    }
    log_message("settings ", message, wParam, lParam);
    switch (message) {
    case WM_INITDIALOG:
        layout(hwnd);
        CheckRadioButton(hwnd, 300, 301, g_draw_three ? 301 : 300);
        CheckRadioButton(hwnd, 302, 304, g_scoring);
        CheckDlgButton(hwnd, 305, BST_CHECKED);
        CheckDlgButton(hwnd, 306, BST_CHECKED);
        CheckDlgButton(hwnd, 307, BST_UNCHECKED);
        CheckDlgButton(hwnd, 308, BST_UNCHECKED);
        enable_keep(hwnd);
        states(hwnd);
        return TRUE;
    case WM_PAINT:
        marker(hwnd, g_cyan);
        return TRUE;
    case WM_CTLCOLORDLG:
    case WM_CTLCOLORSTATIC:
        SetBkColor((HDC)wParam, RGB(255, 255, 192));
        return (INT_PTR)g_yellow;
    case WM_COMMAND: {
        int id = LOWORD(wParam);
        if (id == 300 || id == 301) {
            g_draw_three = id == 301;
            CheckRadioButton(hwnd, 300, 301, id);
            return TRUE;
        }
        if (id >= 302 && id <= 304) {
            g_scoring = id;
            CheckRadioButton(hwnd, 302, 304, id);
            enable_keep(hwnd);
            return TRUE;
        }
        if (id == IDOK || id == IDCANCEL) {
            states(hwnd);
            flush();
            pixels(hwnd, 305, 0, 0, 16, 0);
            pixels(hwnd, 307, 0, 0, 16, 0);
            pixels(hwnd, g_draw_three ? 301 : 300, 0, 0, 16, 0);
            pixels(hwnd, g_draw_three ? 300 : 301, 0, 0, 16, 0);
            pixels(hwnd, IDOK, 0, 0, 0, 3);
            pixels(hwnd, IDOK, 0, -3, 0, 0);
            pixels(hwnd, IDCANCEL, 0, 0, 0, 3);
            EndDialog(hwnd, id);
            say("settings ended\n");
            return TRUE;
        }
        return FALSE;
    }
    }
    return FALSE;
}

/* ---- The main window --------------------------------------------------------------------- */

static void command(HWND hwnd, int id) {
    static const WCHAR k_question[] = {'D', 'e', 'a', 'l', ' ', 'a', 'g', 'a', 'i', 'n', '?', 0};
    static const WCHAR k_caption[] = {'D', 'i', 'a', 'l', 'o', 'g', ' ', 't', 'e', 's', 't', 0};
    static const WCHAR k_other[] = {'A', ' ', 'm', 'e', 'n', 'u', ' ', 'a', 'n', 'd', ' ', 't', 'w', 'o', ' ',
                                    'd', 'i', 'a', 'l', 'o', 'g', 's', 0};
    INT_PTR result;
    switch (id) {
    case ID_NEW:
        result = MessageBoxW(hwnd, k_question, k_caption, MB_YESNO | MB_ICONEXCLAMATION);
        say("message box ");
        say_int((int)result);
        say("\n");
        g_undo = result == IDYES;
        break;
    case ID_UNDO:
        say("undo\n");
        g_undo = 0;
        break;
    case ID_BACKS:
        result = DialogBoxParamW(g_instance, MAKEINTRESOURCEW(101), hwnd, backs_proc, 1234);
        g_dialog = NULL;
        say("backs ");
        say_int((int)result);
        say(" back ");
        say_int(g_back);
        say("\n");
        break;
    case ID_OPTIONS:
        result = DialogBoxParamW(g_instance, MAKEINTRESOURCEW(103), hwnd, settings_proc, 0);
        g_dialog = NULL;
        say("settings ");
        say_int((int)result);
        say("\n");
        break;
    case ID_EXIT:
        PostMessageW(hwnd, WM_CLOSE, 0, 0);
        break;
    case ID_WIN:
        say("won\n");
        break;
    case ID_ABOUT:
        result = ShellAboutW(hwnd, k_caption, k_other, NULL);
        say("about ");
        say_int((int)result);
        say("\n");
        break;
    default:
        break;
    }
}

static LRESULT WINAPI main_proc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_INITMENUPOPUP && LOWORD(lParam) < 2 && HIWORD(lParam) == 0) {
        g_popups[LOWORD(lParam)] = (HMENU)wParam;
    }
    log_message("main ", message, wParam, lParam);
    switch (message) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        fill(dc, g_magenta, 0, 0, 4, 4);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_INITMENUPOPUP:
        if (LOWORD(lParam) == 0 && HIWORD(lParam) == 0) {
            UINT previous = EnableMenuItem(GetMenu(hwnd), ID_UNDO, g_undo ? MF_ENABLED : MF_GRAYED);
            say("main undo was 0x");
            say_hex(previous, 2);
            say("\n");
        }
        break;
    case WM_COMMAND:
        command(hwnd, LOWORD(wParam));
        return 0;
    case WM_DESTROY:
        PostQuitMessage(5);
        return 0;
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

/* ---- Start ---------------------------------------------------------------------------- */

static int same(const WCHAR *a, const char *b) {
    while (*a != 0 && *a == (WCHAR)*b) {
        ++a;
        ++b;
    }
    return *a == 0 && *b == 0;
}

static int run(void) {
    /* "all" as the last word of the command line. */
    const WCHAR *line = GetCommandLineW();
    int n = lstrlenW(line);
    while (n > 0 && line[n - 1] == ' ') {
        --n;
    }
    g_all = n >= 4 && same(line + n - 3, "all") && line[n - 4] == ' ';

    INITCOMMONCONTROLSEX icc = {sizeof(icc), ICC_WIN95_CLASSES};
    say(InitCommonControlsEx(&icc) ? "common controls\n" : "no common controls\n");

    g_instance = GetModuleHandleA(NULL);
    g_green = CreateSolidBrush(RGB(0, 128, 0));
    g_magenta = CreateSolidBrush(RGB(255, 0, 255));
    g_cyan = CreateSolidBrush(RGB(0, 255, 255));
    g_yellow = CreateSolidBrush(RGB(255, 255, 192));

    static const WCHAR k_class[] = {'S', 'x', 'w', 'D', 'i', 'a', 'l', 'o', 'g', 0};
    static const WCHAR k_title[] = {'D', 'i', 'a', 'l', 'o', 'g', ' ', 't', 'e', 's', 't', 0};
    WNDCLASSEXW wc = {0};
    wc.cbSize = sizeof(wc);
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = main_proc;
    wc.hInstance = g_instance;
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.hbrBackground = g_green;
    wc.lpszMenuName = MAKEINTRESOURCEW(1);
    wc.lpszClassName = k_class;
    if (RegisterClassExW(&wc) == 0) {
        say("RegisterClassExW failed\n");
        return 1;
    }
    HACCEL accelerators = LoadAcceleratorsW(g_instance, MAKEINTRESOURCEW(2));
    RECT r = {0, 0, CLIENT_WIDTH, CLIENT_HEIGHT};
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, TRUE);
    say("adjusted ");
    say_int(r.left);
    say(",");
    say_int(r.top);
    say(" ");
    say_int(r.right);
    say(",");
    say_int(r.bottom);
    say("\n");
    g_main = CreateWindowExW(0, k_class, k_title, WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, 60, 60,
                             r.right - r.left, r.bottom - r.top, NULL, NULL, g_instance, NULL);
    if (g_main == NULL) {
        say("CreateWindowExW failed\n");
        return 1;
    }
    g_menu = GetMenu(g_main);
    say(g_menu != NULL ? "menu\n" : "no menu\n");
    say("undo enabled 0x");
    say_hex(EnableMenuItem(g_menu, ID_UNDO, MF_GRAYED), 2);
    say(", missing item 0x");
    say_hex(EnableMenuItem(g_menu, 4242, MF_GRAYED), 8);
    say(", by position 0x");
    say_hex(EnableMenuItem(g_menu, 1, MF_BYPOSITION | MF_ENABLED), 2);
    say("\n");
    RECT client;
    GetClientRect(g_main, &client);
    say("client ");
    say_int(client.right);
    say("x");
    say_int(client.bottom);
    say("\n");
    ShowWindow(g_main, SW_SHOWNORMAL);
    UpdateWindow(g_main);

    MSG msg;
    int got;
    while ((got = GetMessageW(&msg, NULL, 0, 0)) > 0) {
        if (!TranslateAcceleratorW(g_main, accelerators, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    say("quit ");
    say_int((int)msg.wParam);
    say("\n");
    return (int)msg.wParam;
}

void start(void) {
    int status = run();
    flush();
    ExitProcess((unsigned)status);
}
