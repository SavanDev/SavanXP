/*
 * hooks.exe -- The WP5 fixture of what MFC does to every window
 * (WIN32_WORDPAD_ROADMAP.md, WP5): hooks (WH_CBT, WH_CALLWNDPROC and its
 * return, WH_GETMESSAGE, WH_MSGFILTER), subclassing with SetWindowLongW and
 * CallWindowProcW, window properties, the window tree, positions and
 * deferred positions, SetParent, non-client edges and scroll bars, carets,
 * ScrollWindowEx, menus built at run time, and a modeless and a modal dialog
 * built in memory, driven through IsDialogMessageW.
 *
 * It needs no input: everything is sent, posted or called. It writes one
 * line per event; the messages of a window are written only inside the
 * sections that trace it, and only those logged() keeps (with the argument
 * "all", every one), so what the window manager decides about a top-level
 * window stays out. No handle, address, screen position or pixel of text is
 * printed: windows are written by name and positions from the main window's
 * client area.
 */
#include <windows.h>

#include "../kernel/print.h"

DECLSPEC_IMPORT int WINAPI lstrcmpW(LPCWSTR a, LPCWSTR b);
DECLSPEC_IMPORT HMODULE WINAPI GetModuleHandleW(LPCWSTR name);
DECLSPEC_IMPORT ATOM WINAPI GlobalAddAtomW(LPCWSTR name);
DECLSPEC_IMPORT ATOM WINAPI GlobalDeleteAtom(ATOM atom);

#define ID_C1 101
#define ID_C2 102
#define ID_C3 103
#define ID_C4 104
#define ID_BUTTON 105
#define ID_SUB 106
#define ID_GRAND 107

static HINSTANCE g_instance;
static int g_all;
static int g_trace;     /* the window procedures write what they get */
static int g_trace_cwp; /* WH_CALLWNDPROC and its return write too */
static int g_trace_filter;
static int g_veto_focus;
static HHOOK g_cbt;
static HHOOK g_cwp;
static HHOOK g_cwpret;
static HHOOK g_getmsg;
static HHOOK g_filter;
static WNDPROC g_button_proc;
static WNDPROC g_sub_old;
static HWND g_main;
static HWND g_c1;
static HWND g_c2;
static HWND g_c3;
static HWND g_c4;
static HWND g_button;
static HWND g_sub;
static HWND g_grand;
static HWND g_owned;
static HWND g_dialog;
static HMENU g_bar;
static HMENU g_file;
static HMENU g_edit;

static const WCHAR k_main_class[] = L"HookMain";
static const WCHAR k_child_class[] = L"HookChild";

/* ---- Names ---------------------------------------------------------------------------- */

static void say_window(HWND h) {
    static const struct {
        HWND *h;
        const char *name;
    } k_names[] = {
        {&g_main, "main"},   {&g_c1, "c1"},       {&g_c2, "c2"},           {&g_c3, "c3"},
        {&g_c4, "c4"},       {&g_button, "button"}, {&g_sub, "sub"},       {&g_grand, "grand"},
        {&g_owned, "owned"}, {&g_dialog, "dialog"},
    };
    if (h == NULL) {
        say("none");
        return;
    }
    if (h == (HWND)1) {
        say("bottom");
        return;
    }
    for (unsigned i = 0; i < sizeof(k_names) / sizeof(k_names[0]); ++i) {
        if (*k_names[i].h == h) {
            say(k_names[i].name);
            return;
        }
    }
    if (g_dialog != NULL && GetParent(h) == g_dialog) {
        say("item ");
        say_int(GetDlgCtrlID(h));
        return;
    }
    say("other");
}

static const char *message_name(UINT m) {
    switch (m) {
    case WM_CREATE: return "WM_CREATE";
    case WM_DESTROY: return "WM_DESTROY";
    case WM_MOVE: return "WM_MOVE";
    case WM_SIZE: return "WM_SIZE";
    case WM_ACTIVATE: return "WM_ACTIVATE";
    case WM_SETFOCUS: return "WM_SETFOCUS";
    case WM_KILLFOCUS: return "WM_KILLFOCUS";
    case WM_ENABLE: return "WM_ENABLE";
    case WM_SETTEXT: return "WM_SETTEXT";
    case WM_GETTEXT: return "WM_GETTEXT";
    case WM_GETTEXTLENGTH: return "WM_GETTEXTLENGTH";
    case WM_PAINT: return "WM_PAINT";
    case WM_CLOSE: return "WM_CLOSE";
    case WM_ERASEBKGND: return "WM_ERASEBKGND";
    case WM_SHOWWINDOW: return "WM_SHOWWINDOW";
    case WM_CANCELMODE: return "WM_CANCELMODE";
    case WM_CHILDACTIVATE: return "WM_CHILDACTIVATE";
    case WM_GETMINMAXINFO: return "WM_GETMINMAXINFO";
    case WM_NEXTDLGCTL: return "WM_NEXTDLGCTL";
    case WM_SETFONT: return "WM_SETFONT";
    case WM_GETFONT: return "WM_GETFONT";
    case WM_WINDOWPOSCHANGING: return "WM_WINDOWPOSCHANGING";
    case WM_WINDOWPOSCHANGED: return "WM_WINDOWPOSCHANGED";
    case WM_NOTIFYFORMAT: return "WM_NOTIFYFORMAT";
    case WM_STYLECHANGING: return "WM_STYLECHANGING";
    case WM_STYLECHANGED: return "WM_STYLECHANGED";
    case WM_NCCREATE: return "WM_NCCREATE";
    case WM_NCDESTROY: return "WM_NCDESTROY";
    case WM_NCCALCSIZE: return "WM_NCCALCSIZE";
    case WM_NCPAINT: return "WM_NCPAINT";
    case WM_GETDLGCODE: return "WM_GETDLGCODE";
    case WM_KEYDOWN: return "WM_KEYDOWN";
    case WM_KEYUP: return "WM_KEYUP";
    case WM_CHAR: return "WM_CHAR";
    case WM_INITDIALOG: return "WM_INITDIALOG";
    case WM_COMMAND: return "WM_COMMAND";
    case WM_SYSCOMMAND: return "WM_SYSCOMMAND";
    case WM_HSCROLL: return "WM_HSCROLL";
    case WM_VSCROLL: return "WM_VSCROLL";
    case WM_CHANGEUISTATE: return "WM_CHANGEUISTATE";
    case WM_UPDATEUISTATE: return "WM_UPDATEUISTATE";
    case WM_QUERYUISTATE: return "WM_QUERYUISTATE";
    case WM_CTLCOLORBTN: return "WM_CTLCOLORBTN";
    case WM_CTLCOLORDLG: return "WM_CTLCOLORDLG";
    case WM_CTLCOLORSTATIC: return "WM_CTLCOLORSTATIC";
    case WM_PARENTNOTIFY: return "WM_PARENTNOTIFY";
    case WM_CAPTURECHANGED: return "WM_CAPTURECHANGED";
    case BM_GETCHECK: return "BM_GETCHECK";
    case BM_SETCHECK: return "BM_SETCHECK";
    case BM_GETSTATE: return "BM_GETSTATE";
    case BM_SETSTATE: return "BM_SETSTATE";
    case BM_SETSTYLE: return "BM_SETSTYLE";
    case BM_CLICK: return "BM_CLICK";
    case DM_GETDEFID: return "DM_GETDEFID";
    case DM_SETDEFID: return "DM_SETDEFID";
    case WM_LBUTTONDOWN: return "WM_LBUTTONDOWN";
    case WM_LBUTTONUP: return "WM_LBUTTONUP";
    default: return 0;
    }
}

/* The messages a traced window writes: the ones named above, and the
 * program's own (WM_USER and up), but not the ones the window manager or
 * the input method bring at times of their own choosing. */
static int logged(UINT m) {
    if (g_all) {
        return 1;
    }
    if (m >= WM_USER && m < 0x8000) {
        return 1;
    }
    switch (m) {
    case WM_GETTEXT:
    case WM_GETTEXTLENGTH:
    case WM_GETFONT:
    case WM_QUERYUISTATE:
    case WM_NOTIFYFORMAT:
    case WM_CTLCOLORBTN:
    case WM_CTLCOLORDLG:
    case WM_CTLCOLORSTATIC:
    case WM_GETDLGCODE:
        return 0;
    default:
        return message_name(m) != 0;
    }
}

static void say_message(UINT m) {
    const char *name = message_name(m);
    if (name != 0) {
        say(name);
    } else if (m >= WM_USER && m < 0x8000) {
        say("WM_USER+");
        say_int((long)(m - WM_USER));
    } else {
        say_hex(m);
    }
}

static void say_rect(const RECT *r) {
    say("(");
    say_int(r->left);
    say(",");
    say_int(r->top);
    say(")-(");
    say_int(r->right);
    say(",");
    say_int(r->bottom);
    say(")");
}

/* A window's rectangle in the main window's client coordinates. */
static void say_window_rect(HWND h) {
    RECT r;
    GetWindowRect(h, &r);
    MapWindowPoints(NULL, g_main, (POINT *)&r, 2);
    say_rect(&r);
}

/* What a traced message's parameters say, for the ones whose parameters do
 * not depend on the window manager. */
static void say_details(HWND hwnd, UINT m, WPARAM wParam, LPARAM lParam) {
    switch (m) {
    case WM_WINDOWPOSCHANGING:
    case WM_WINDOWPOSCHANGED: {
        const WINDOWPOS *p = (const WINDOWPOS *)lParam;
        say(" flags ");
        say_hex(p->flags);
        if ((p->flags & SWP_NOMOVE) == 0) {
            say(" at ");
            say_int(p->x);
            say(",");
            say_int(p->y);
        }
        if ((p->flags & SWP_NOSIZE) == 0) {
            say(" size ");
            say_int(p->cx);
            say("x");
            say_int(p->cy);
        }
        /* A top-level window's place is the window manager's. */
        if ((p->flags & SWP_NOZORDER) == 0 && (GetWindowLongW(hwnd, GWL_STYLE) & WS_CHILD) != 0) {
            say(" after ");
            say_window(p->hwndInsertAfter);
        }
        break;
    }
    case WM_NCCALCSIZE:
        say(" calc ");
        say_int((long)wParam);
        if (wParam != 0) {
            const NCCALCSIZE_PARAMS *p = (const NCCALCSIZE_PARAMS *)lParam;
            say(" new ");
            say_rect(&p->rgrc[0]);
            say(" old ");
            say_rect(&p->rgrc[1]);
            say(" client ");
            say_rect(&p->rgrc[2]);
        }
        break;
    case WM_MOVE:
    case WM_SIZE:
        say(" ");
        say_int(GET_X_LPARAM(lParam));
        say(",");
        say_int(GET_Y_LPARAM(lParam));
        if (m == WM_SIZE) {
            say(" kind ");
            say_int((long)wParam);
        }
        break;
    case WM_STYLECHANGING:
    case WM_STYLECHANGED: {
        const STYLESTRUCT *s = (const STYLESTRUCT *)lParam;
        say(wParam == (WPARAM)GWL_STYLE ? " style " : " exstyle ");
        say_hex(s->styleOld);
        say(" to ");
        say_hex(s->styleNew);
        break;
    }
    case WM_SHOWWINDOW:
    case WM_ENABLE:
    case WM_SETTEXT:
        if (m == WM_SETTEXT) {
            say(" ");
            say_w((const WCHAR *)lParam);
        } else {
            say(" ");
            say_int((long)wParam);
            say(" ");
            say_int((long)lParam);
        }
        break;
    case WM_ACTIVATE:
        /* The other window may be one of the input method's. */
        say(" ");
        say_int(LOWORD(wParam));
        break;
    case WM_SETFOCUS:
    case WM_KILLFOCUS:
        say(" other ");
        say_window((HWND)wParam);
        break;
    case WM_COMMAND:
        say(" id ");
        say_int(LOWORD(wParam));
        say(" code ");
        say_int(HIWORD(wParam));
        say(" from ");
        say_window((HWND)lParam);
        break;
    case WM_PARENTNOTIFY:
        say(" event ");
        say_hex(LOWORD(wParam));
        if (LOWORD(wParam) == WM_CREATE || LOWORD(wParam) == WM_DESTROY) {
            say(" id ");
            say_int(HIWORD(wParam));
            say(" ");
            say_window((HWND)lParam);
        }
        break;
    case WM_INITDIALOG:
        say(" focus ");
        say_window((HWND)wParam);
        say(" param ");
        say_int(lParam);
        break;
    case WM_SYSCOMMAND:
        say(" ");
        say_hex((DWORD)wParam & 0xfff0u);
        break;
    case WM_NCPAINT:
    case WM_ERASEBKGND:
    case WM_PAINT:
    case WM_NCCREATE:
    case WM_CREATE:
    case WM_DESTROY:
    case WM_NCDESTROY:
    case WM_CLOSE:
    case WM_GETMINMAXINFO:
    case WM_CANCELMODE:
    case WM_SETFONT:
        break;
    default:
        say(" ");
        say_hex((DWORD)wParam);
        say(" ");
        say_hex((DWORD)lParam);
        break;
    }
    (void)hwnd;
}

static void trace_message(const char *who, HWND hwnd, UINT m, WPARAM wParam, LPARAM lParam) {
    if (!g_trace || !logged(m)) {
        return;
    }
    line(who);
    say_window(hwnd);
    say(" ");
    say_message(m);
    say_details(hwnd, m, wParam, lParam);
}

/* ---- Window procedures -------------------------------------------------------------------- */

static LRESULT CALLBACK main_proc(HWND hwnd, UINT m, WPARAM wParam, LPARAM lParam) {
    /* The main window's frame messages are the window manager's: only the
     * ones the program causes are written. */
    if (m == WM_CREATE || m == WM_NCCREATE || m == WM_DESTROY || m == WM_NCDESTROY || m == WM_CLOSE ||
        m == WM_COMMAND || m == WM_SYSCOMMAND || m == WM_PARENTNOTIFY || m == WM_STYLECHANGED ||
        m == WM_STYLECHANGING || (m >= WM_USER && m < 0x8000) || g_all) {
        trace_message("main", hwnd, m, wParam, lParam);
    }
    if (m == WM_USER + 20) {
        return 2020;
    }
    return DefWindowProcW(hwnd, m, wParam, lParam);
}

static LRESULT CALLBACK child_proc(HWND hwnd, UINT m, WPARAM wParam, LPARAM lParam) {
    trace_message("child", hwnd, m, wParam, lParam);
    if (m == WM_USER + 7) {
        return (LRESULT)(wParam + lParam);
    }
    return DefWindowProcW(hwnd, m, wParam, lParam);
}

/* The subclass the CBT hook puts on "sub" before it gets any message, as
 * MFC attaches its objects. */
static LRESULT CALLBACK sub_proc(HWND hwnd, UINT m, WPARAM wParam, LPARAM lParam) {
    trace_message("subclass", hwnd, m, wParam, lParam);
    return CallWindowProcW(g_sub_old, hwnd, m, wParam, lParam);
}

static LRESULT CALLBACK button_sub(HWND hwnd, UINT m, WPARAM wParam, LPARAM lParam) {
    if (m == BM_SETCHECK || m == BM_GETCHECK) {
        line("button subclass");
        say_message(m);
        say(" ");
        say_int((long)wParam);
    }
    return CallWindowProcW(g_button_proc, hwnd, m, wParam, lParam);
}

/* ---- Hooks ---------------------------------------------------------------------------------- */

static int ours(LPCWSTR cls) {
    if ((ULONG_PTR)cls < 0x10000u) {
        return 0;
    }
    static const WCHAR *const k_classes[] = {k_main_class, k_child_class, L"Button", L"#32770"};
    for (unsigned i = 0; i < sizeof(k_classes) / sizeof(k_classes[0]); ++i) {
        const WCHAR *a = cls;
        const WCHAR *b = k_classes[i];
        while (*a != 0 && (*a | 0x20) == (*b | 0x20)) {
            ++a;
            ++b;
        }
        if (*a == 0 && *b == 0) {
            return 1;
        }
    }
    return 0;
}

static int ours_window(HWND h) {
    WCHAR name[64];
    return GetClassNameW(h, name, 64) > 0 && ours(name);
}

static const char *cbt_name(int code) {
    switch (code) {
    case HCBT_MOVESIZE: return "movesize";
    case HCBT_MINMAX: return "minmax";
    case HCBT_QS: return "qs";
    case HCBT_CREATEWND: return "createwnd";
    case HCBT_DESTROYWND: return "destroywnd";
    case HCBT_ACTIVATE: return "activate";
    case HCBT_CLICKSKIPPED: return "clickskipped";
    case HCBT_KEYSKIPPED: return "keyskipped";
    case HCBT_SYSCOMMAND: return "syscommand";
    case HCBT_SETFOCUS: return "setfocus";
    default: return "?";
    }
}

static LRESULT CALLBACK cbt_hook(int code, WPARAM wParam, LPARAM lParam) {
    if (code == HCBT_CREATEWND) {
        const CBT_CREATEWNDW *c = (const CBT_CREATEWNDW *)lParam;
        if (ours(c->lpcs->lpszClass)) {
            line("cbt createwnd");
            say_w(c->lpcs->lpszClass);
            say(" ");
            say_w(c->lpcs->lpszName != NULL ? c->lpcs->lpszName : L"");
            say(" style ");
            say_hex((DWORD)c->lpcs->style);
            say(" exstyle ");
            say_hex(c->lpcs->dwExStyle);
            if (c->lpcs->style & WS_CHILD) {
                say(" at ");
                say_int(c->lpcs->x);
                say(",");
                say_int(c->lpcs->y);
                say(" size ");
                say_int(c->lpcs->cx);
                say("x");
                say_int(c->lpcs->cy);
            }
            say(" after ");
            say_window(c->hwndInsertAfter);
            say(" class now ");
            say_yes(ours_window((HWND)wParam));
            if (c->lpcs->lpszName != NULL && lstrcmpW(c->lpcs->lpszName, L"sub") == 0) {
                g_sub_old = (WNDPROC)SetWindowLongW((HWND)wParam, GWL_WNDPROC, (LONG)sub_proc);
                say(", subclassed: old is child_proc ");
                say_yes(g_sub_old == child_proc);
            }
            if (c->lpcs->lpszName != NULL && lstrcmpW(c->lpcs->lpszName, L"veto") == 0) {
                say(", vetoed");
                return 1;
            }
        }
    } else if (code == HCBT_ACTIVATE || code == HCBT_SETFOCUS || code == HCBT_DESTROYWND ||
               code == HCBT_SYSCOMMAND || code == HCBT_MINMAX) {
        if (code == HCBT_SYSCOMMAND || ours_window((HWND)wParam)) {
            line("cbt ");
            say(cbt_name(code));
            say(" ");
            if (code == HCBT_SYSCOMMAND) {
                say_hex((DWORD)wParam & 0xfff0u);
            } else {
                say_window((HWND)wParam);
            }
            if (code == HCBT_ACTIVATE) {
                const CBTACTIVATESTRUCT *a = (const CBTACTIVATESTRUCT *)lParam;
                say(" mouse ");
                say_int(a->fMouse);
            } else if (code == HCBT_SETFOCUS) {
                say(" from ");
                say_window((HWND)lParam);
                if (g_veto_focus && (HWND)wParam == g_c3) {
                    say(", vetoed");
                    return 1;
                }
            } else if (code == HCBT_MINMAX) {
                say(" show ");
                say_int(LOWORD(lParam));
            }
        }
    }
    return CallNextHookEx(g_cbt, code, wParam, lParam);
}

static LRESULT CALLBACK cwp_hook(int code, WPARAM wParam, LPARAM lParam) {
    const CWPSTRUCT *c = (const CWPSTRUCT *)lParam;
    if (g_trace_cwp && code == HC_ACTION && c->hwnd == g_c1 && logged(c->message)) {
        line("callwndproc");
        say_window(c->hwnd);
        say(" ");
        say_message(c->message);
        say(" current thread ");
        say_yes(wParam != 0);
    }
    return CallNextHookEx(g_cwp, code, wParam, lParam);
}

static LRESULT CALLBACK cwpret_hook(int code, WPARAM wParam, LPARAM lParam) {
    const CWPRETSTRUCT *c = (const CWPRETSTRUCT *)lParam;
    if (g_trace_cwp && code == HC_ACTION && c->hwnd == g_c1 && logged(c->message)) {
        line("callwndprocret");
        say_window(c->hwnd);
        say(" ");
        say_message(c->message);
        say(" result ");
        say_int(c->lResult);
    }
    return CallNextHookEx(g_cwpret, code, wParam, lParam);
}

static LRESULT CALLBACK getmsg_hook(int code, WPARAM wParam, LPARAM lParam) {
    MSG *m = (MSG *)lParam;
    if (code == HC_ACTION && m->message >= WM_USER + 30 && m->message < WM_USER + 40) {
        line("getmessage ");
        say_message(m->message);
        say(" for ");
        say_window(m->hwnd);
        say(wParam == PM_REMOVE ? " remove" : " keep");
        say(" wparam ");
        say_int((long)m->wParam);
        if (m->message == WM_USER + 31) {
            m->wParam = 99;
        }
    }
    return CallNextHookEx(g_getmsg, code, wParam, lParam);
}

static LRESULT CALLBACK filter_hook(int code, WPARAM wParam, LPARAM lParam) {
    MSG *m = (MSG *)lParam;
    if (g_trace_filter && m != NULL && (m->message >= WM_USER || m->message == WM_KEYDOWN || m->message == WM_CHAR)) {
        line("msgfilter code ");
        say_int(code);
        say(" ");
        say_message(m->message);
        say(" for ");
        say_window(m->hwnd);
        if (m->message == WM_USER + 2) {
            say(", eaten");
            return 1;
        }
    }
    (void)wParam;
    return CallNextHookEx(g_filter, code, wParam, lParam);
}

/* ---- Helpers ---------------------------------------------------------------------------------- */

static void pump(void) {
    MSG m;
    while (PeekMessageW(&m, NULL, 0, 0, PM_REMOVE)) {
        if (m.message == WM_QUIT) {
            break;
        }
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
}

static HWND make_child(const WCHAR *cls, const WCHAR *name, DWORD style, DWORD ex_style, int x, int y, int cx, int cy,
                       HWND parent, int id) {
    return CreateWindowExW(ex_style, cls, name, style | WS_CHILD, x, y, cx, cy, parent, (HMENU)(ULONG_PTR)id,
                           g_instance, NULL);
}

static void say_order(HWND parent) {
    line("order");
    for (HWND h = GetWindow(parent, GW_CHILD); h != NULL; h = GetWindow(h, GW_HWNDNEXT)) {
        say(" ");
        say_window(h);
    }
}

static void say_geometry(const char *label, HWND h) {
    line(label);
    say_window_rect(h);
    RECT c;
    GetClientRect(h, &c);
    say(" client ");
    say_int(c.right);
    say("x");
    say_int(c.bottom);
    POINT origin = {0, 0};
    MapWindowPoints(h, g_main, &origin, 1);
    say(" at ");
    say_int(origin.x);
    say(",");
    say_int(origin.y);
}

static BOOL CALLBACK enum_child(HWND h, LPARAM stop) {
    say(" ");
    say_window(h);
    return h != (HWND)stop;
}

/* ---- Sections ---------------------------------------------------------------------------------- */

static void install_hooks(void) {
    line("hooks");
    const DWORD self = GetCurrentThreadId();
    g_cbt = SetWindowsHookExW(WH_CBT, cbt_hook, NULL, self);
    g_cwp = SetWindowsHookExW(WH_CALLWNDPROC, cwp_hook, NULL, self);
    g_cwpret = SetWindowsHookExW(WH_CALLWNDPROCRET, cwpret_hook, NULL, self);
    g_getmsg = SetWindowsHookExW(WH_GETMESSAGE, getmsg_hook, NULL, self);
    g_filter = SetWindowsHookExW(WH_MSGFILTER, filter_hook, NULL, self);
    say_yes(g_cbt != NULL && g_cwp != NULL && g_cwpret != NULL && g_getmsg != NULL && g_filter != NULL);
    line("a hook of no kind");
    SetLastError(0);
    say_yes(SetWindowsHookExW(99, cbt_hook, NULL, self) != NULL);
    say_error();
    line("a global hook without a module");
    SetLastError(0);
    say_yes(SetWindowsHookExW(WH_CBT, cbt_hook, NULL, 0) != NULL);
    say_error();
    line("unhook a handle that is none");
    SetLastError(0);
    say_result(UnhookWindowsHookEx((HHOOK)0x1234));
}

static void create_windows(void) {
    WNDCLASSW wc;
    static const WCHAR k_empty[] = {0};
    for (unsigned i = 0; i < sizeof(wc); ++i) {
        ((char *)&wc)[i] = 0;
    }
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = main_proc;
    wc.cbWndExtra = 8;
    wc.hInstance = g_instance;
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = k_main_class;
    line("register main");
    say_yes(RegisterClassW(&wc) != 0);
    wc.style = 0;
    wc.lpfnWndProc = child_proc;
    wc.cbWndExtra = 4;
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = k_child_class;
    line("register child");
    say_yes(RegisterClassW(&wc) != 0);

    g_trace = 1;
    g_main = CreateWindowExW(0, k_main_class, L"Hooks", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, 40, 40, 460, 380,
                             NULL, NULL, g_instance, NULL);
    line("main made");
    say_yes(g_main != NULL);
    g_c1 = make_child(k_child_class, L"one", WS_VISIBLE | WS_BORDER, 0, 10, 10, 100, 60, g_main, ID_C1);
    g_c2 = make_child(k_child_class, L"two", WS_VISIBLE, WS_EX_CLIENTEDGE, 120, 10, 100, 60, g_main, ID_C2);
    g_c3 = make_child(k_child_class, L"three", WS_VISIBLE, WS_EX_STATICEDGE, 230, 10, 100, 60, g_main, ID_C3);
    g_c4 = make_child(k_child_class, L"four", WS_VISIBLE | WS_VSCROLL | WS_HSCROLL, WS_EX_CLIENTEDGE, 10, 80, 150,
                      100, g_main, ID_C4);
    g_button = make_child(L"Button", L"&Check", WS_VISIBLE | BS_AUTOCHECKBOX, 0, 170, 80, 100, 20, g_main, ID_BUTTON);
    g_sub = make_child(k_child_class, L"sub", WS_VISIBLE, 0, 280, 80, 60, 40, g_main, ID_SUB);
    g_grand = make_child(k_child_class, L"grand", WS_VISIBLE, 0, 5, 5, 30, 20, g_c1, ID_GRAND);
    line("veto made");
    SetLastError(0);
    HWND veto = make_child(k_child_class, L"veto", WS_VISIBLE, 0, 0, 0, 10, 10, g_main, 199);
    say_yes(veto != NULL);
    say_error();
    g_owned = CreateWindowExW(0, k_child_class, L"owned", WS_POPUP, 0, 0, 50, 50, g_main, NULL, g_instance, NULL);
    g_trace = 0;
    (void)k_empty;
}

static void window_longs(void) {
    line("c1 style");
    say_hex((DWORD)GetWindowLongW(g_c1, GWL_STYLE));
    say(" exstyle ");
    say_hex((DWORD)GetWindowLongW(g_c1, GWL_EXSTYLE));
    say(" id ");
    say_int(GetWindowLongW(g_c1, GWL_ID));
    say(" parent ");
    say_window((HWND)GetWindowLongW(g_c1, GWL_HWNDPARENT));
    say(" instance ours ");
    say_yes((HINSTANCE)GetWindowLongW(g_c1, GWL_HINSTANCE) == g_instance);
    say(" userdata ");
    say_int(GetWindowLongW(g_c1, GWL_USERDATA));
    say(" proc ours ");
    say_yes((WNDPROC)GetWindowLongW(g_c1, GWL_WNDPROC) == child_proc);
    line("main style");
    say_hex((DWORD)GetWindowLongW(g_main, GWL_STYLE));
    say(" exstyle ");
    say_hex((DWORD)GetWindowLongW(g_main, GWL_EXSTYLE));
    say(" parent ");
    say_window((HWND)GetWindowLongW(g_main, GWL_HWNDPARENT));
    line("owned style");
    say_hex((DWORD)GetWindowLongW(g_owned, GWL_STYLE));
    say(" owner ");
    say_window((HWND)GetWindowLongW(g_owned, GWL_HWNDPARENT));
    say(" parent ");
    say_window(GetParent(g_owned));
    say(" GW_OWNER ");
    say_window(GetWindow(g_owned, GW_OWNER));

    line("extra 0 set");
    SetLastError(0);
    say_int(SetWindowLongW(g_c1, 0, 0x12345678));
    say(" get ");
    say_hex((DWORD)GetWindowLongW(g_c1, 0));
    say_error();
    line("extra 1");
    SetLastError(0);
    say_int(GetWindowLongW(g_c1, 1));
    say_error();
    line("extra 4");
    SetLastError(0);
    say_int(GetWindowLongW(g_c1, 4));
    say_error();
    line("main extra 4 set");
    SetLastError(0);
    say_int(SetWindowLongW(g_main, 4, 77));
    say(" get ");
    say_int(GetWindowLongW(g_main, 4));
    say_error();
    line("index -100");
    SetLastError(0);
    say_int(GetWindowLongW(g_c1, -100));
    say_error();
    line("no window");
    SetLastError(0);
    say_int(GetWindowLongW((HWND)0x1234, GWL_STYLE));
    say_error();

    line("userdata set");
    say_int(SetWindowLongW(g_c1, GWL_USERDATA, 55));
    say(" again ");
    say_int(SetWindowLongW(g_c1, GWL_USERDATA, 66));
    say(" get ");
    say_int(GetWindowLongW(g_c1, GWL_USERDATA));
    line("id set");
    say_int(SetWindowLongW(g_c1, GWL_ID, 151));
    say(" get ");
    say_int(GetDlgCtrlID(g_c1));
    SetWindowLongW(g_c1, GWL_ID, ID_C1);

    g_trace = 1;
    line("style set");
    say_hex((DWORD)SetWindowLongW(g_c1, GWL_STYLE, GetWindowLongW(g_c1, GWL_STYLE) | WS_TABSTOP));
    line("exstyle set");
    say_hex((DWORD)SetWindowLongW(g_c1, GWL_EXSTYLE, WS_EX_STATICEDGE));
    say(" client after ");
    RECT r;
    GetClientRect(g_c1, &r);
    say_int(r.right);
    say("x");
    say_int(r.bottom);
    SetWindowLongW(g_c1, GWL_EXSTYLE, 0);
    SetWindowLongW(g_c1, GWL_STYLE, GetWindowLongW(g_c1, GWL_STYLE) & ~WS_TABSTOP);
    line("main style set");
    say_hex((DWORD)SetWindowLongW(g_main, GWL_STYLE, GetWindowLongW(g_main, GWL_STYLE) & ~WS_MAXIMIZEBOX));
    g_trace = 0;

    line("subclass the button");
    g_button_proc = (WNDPROC)SetWindowLongW(g_button, GWL_WNDPROC, (LONG)button_sub);
    say_yes(g_button_proc != NULL);
    say(" now ");
    say_yes((WNDPROC)GetWindowLongW(g_button, GWL_WNDPROC) == button_sub);
    SendMessageW(g_button, BM_SETCHECK, BST_CHECKED, 0);
    line("checked");
    say_int(SendMessageW(g_button, BM_GETCHECK, 0, 0));
    line("call the old procedure straight");
    say_int(CallWindowProcW(g_button_proc, g_button, BM_GETCHECK, 0, 0));
    line("restore");
    say_yes((WNDPROC)SetWindowLongW(g_button, GWL_WNDPROC, (LONG)g_button_proc) == button_sub);
    SendMessageW(g_button, BM_SETCHECK, BST_UNCHECKED, 0);
    line("sub's procedure is the subclass");
    say_yes((WNDPROC)GetWindowLongW(g_sub, GWL_WNDPROC) == sub_proc);
}

static void classes(void) {
    static const WCHAR *const k_names[] = {L"Button", L"Static", L"Edit", L"ListBox", L"ComboBox",
                                           L"ScrollBar", L"#32770", L"HookChild", L"NoSuchClass"};
    for (unsigned i = 0; i < sizeof(k_names) / sizeof(k_names[0]); ++i) {
        WNDCLASSW wc;
        line("class ");
        say_w(k_names[i]);
        SetLastError(0);
        const BOOL ok = GetClassInfoW(i == 7 ? g_instance : NULL, k_names[i], &wc);
        if (!ok) {
            say(" none");
            say_error();
            continue;
        }
        say(" style ");
        say_hex(wc.style);
        say(" class extra ");
        say_int(wc.cbClsExtra);
        say(" window extra ");
        say_int(wc.cbWndExtra);
        say(" background ");
        say_int((long)(ULONG_PTR)wc.hbrBackground);
        say(" cursor ");
        say_yes(wc.hCursor != NULL);
        say(" menu ");
        say_yes(wc.lpszMenuName != NULL);
    }
    static HWND *const k_windows[] = {&g_main, &g_c1, &g_button};
    for (unsigned i = 0; i < 3; ++i) {
        WCHAR name[64];
        line("class name of ");
        say_window(*k_windows[i]);
        say(" ");
        say_int(GetClassNameW(*k_windows[i], name, 64));
        say(" ");
        say_w(name);
    }
    WCHAR shortname[4];
    line("class name cut");
    say_int(GetClassNameW(g_main, shortname, 4));
    say(" ");
    say_w(shortname);
    line("unregister a class in use");
    SetLastError(0);
    say_result(UnregisterClassW(k_child_class, g_instance));
    line("unregister a class that is none");
    SetLastError(0);
    say_result(UnregisterClassW(L"NoSuchClass", g_instance));
}

static void properties(void) {
    line("set alpha");
    say_result(SetPropW(g_c1, L"Alpha", (HANDLE)1));
    const ATOM beta = GlobalAddAtomW(L"HookBeta");
    line("set beta by atom");
    say_result(SetPropW(g_c1, (LPCWSTR)(ULONG_PTR)beta, (HANDLE)2));
    line("get alpha");
    say_int((long)GetPropW(g_c1, L"Alpha"));
    say(" as ALPHA ");
    say_int((long)GetPropW(g_c1, L"ALPHA"));
    say(" beta by name ");
    say_int((long)GetPropW(g_c1, L"HookBeta"));
    say(" beta by atom ");
    say_int((long)GetPropW(g_c1, (LPCWSTR)(ULONG_PTR)beta));
    line("replace alpha");
    say_result(SetPropW(g_c1, L"Alpha", (HANDLE)3));
    say(" get ");
    say_int((long)GetPropW(g_c1, L"Alpha"));
    line("remove alpha");
    say_int((long)RemovePropW(g_c1, L"Alpha"));
    say(" again ");
    say_int((long)RemovePropW(g_c1, L"Alpha"));
    say(" get ");
    say_int((long)GetPropW(g_c1, L"Alpha"));
    line("on another window");
    say_int((long)GetPropW(g_c2, L"HookBeta"));
    line("on no window");
    SetLastError(0);
    say_result(SetPropW((HWND)0x1234, L"Alpha", (HANDLE)1));
    GlobalDeleteAtom(beta);
}

static void tree(void) {
    say_order(g_main);
    say_order(g_c1);
    line("parent of c1");
    say_window(GetParent(g_c1));
    say(" of main ");
    say_window(GetParent(g_main));
    say(" of grand ");
    say_window(GetParent(g_grand));
    line("first");
    say_window(GetWindow(g_c2, GW_HWNDFIRST));
    say(" last ");
    say_window(GetWindow(g_c2, GW_HWNDLAST));
    say(" prev of c2 ");
    say_window(GetWindow(g_c2, GW_HWNDPREV));
    say(" next of c2 ");
    say_window(GetWindow(g_c2, GW_HWNDNEXT));
    say(" top ");
    say_window(GetTopWindow(g_main));
    say(" child of c2 ");
    say_window(GetWindow(g_c2, GW_CHILD));
    line("is child");
    say_yes(IsChild(g_main, g_c1));
    say(" grand ");
    say_yes(IsChild(g_main, g_grand));
    say(" reverse ");
    say_yes(IsChild(g_c1, g_main));
    say(" owned ");
    say_yes(IsChild(g_main, g_owned));
    line("enumerate");
    EnumChildWindows(g_main, enum_child, 0);
    line("enumerate until c3");
    EnumChildWindows(g_main, enum_child, (LPARAM)g_c3);
    line("ids");
    say_int(GetDlgCtrlID(g_c1));
    say(" ");
    say_int(GetDlgCtrlID(g_grand));
    say(" main ");
    say_int(GetDlgCtrlID(g_main));
    WCHAR text[64];
    line("text of c2");
    say_int(GetWindowTextW(g_c2, text, 64));
    say(" ");
    say_w(text);
    say(" length ");
    say_int(GetWindowTextLengthW(g_c2));
    line("text cut");
    say_int(GetWindowTextW(g_c2, text, 3));
    say(" ");
    say_w(text);
    g_trace = 1;
    line("set text");
    say_result(SetWindowTextW(g_c2, L"deux"));
    g_trace = 0;
    GetWindowTextW(g_c2, text, 64);
    say(" ");
    say_w(text);
    line("visible");
    say_yes(IsWindowVisible(g_c1));
    say(" main ");
    say_yes(IsWindowVisible(g_main));
    say(" enabled ");
    say_yes(IsWindowEnabled(g_c1));
    say(" zoomed ");
    say_yes(IsZoomed(g_main));
    say(" iconic ");
    say_yes(IsIconic(g_main));
    line("is window");
    say_yes(IsWindow(g_c1));
    say(" ");
    say_yes(IsWindow((HWND)0x1234));
    say(" ");
    say_yes(IsWindow(NULL));
}

static void positions(void) {
    say_geometry("c1", g_c1);
    say_geometry("c2", g_c2);
    say_geometry("c3", g_c3);
    say_geometry("c4", g_c4);
    say_geometry("grand", g_grand);
    POINT p = {3, 4};
    ClientToScreen(g_c2, &p);
    ScreenToClient(g_main, &p);
    line("c2's 3,4 in main");
    say_int(p.x);
    say(",");
    say_int(p.y);
    POINT q[2] = {{1, 1}, {2, 2}};
    line("map c1 to c3");
    say_int(MapWindowPoints(g_c1, g_c3, q, 2));
    say(" ");
    say_int(q[0].x);
    say(",");
    say_int(q[0].y);

    g_trace = 1;
    line("move c1");
    say_result(SetWindowPos(g_c1, NULL, 20, 30, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE));
    say_geometry("c1", g_c1);
    line("size c1");
    say_result(SetWindowPos(g_c1, NULL, 0, 0, 120, 70, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE));
    say_geometry("c1", g_c1);
    line("c1 to the bottom");
    say_result(SetWindowPos(g_c1, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE));
    say_order(g_main);
    line("c1 after c2");
    say_result(SetWindowPos(g_c1, g_c2, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE));
    say_order(g_main);
    line("c1 to the top");
    say_result(SetWindowPos(g_c1, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE));
    say_order(g_main);
    line("c1 frame changed");
    say_result(SetWindowPos(g_c1, NULL, 0, 0, 0, 0,
                            SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED));
    line("c1 hidden");
    say_result(SetWindowPos(g_c1, NULL, 0, 0, 0, 0,
                            SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_HIDEWINDOW));
    say(" visible ");
    say_yes(IsWindowVisible(g_c1));
    line("c1 shown");
    say_result(SetWindowPos(g_c1, NULL, 0, 0, 0, 0,
                            SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW));
    line("c1 nothing");
    say_result(SetWindowPos(g_c1, NULL, 20, 30, 120, 70, SWP_NOZORDER | SWP_NOACTIVATE));
    line("move window c1");
    say_result(MoveWindow(g_c1, 10, 10, 100, 60, TRUE));
    line("move window c1 to where it is");
    say_result(MoveWindow(g_c1, 10, 10, 100, 60, FALSE));
    line("defer");
    HDWP d = BeginDeferWindowPos(2);
    say_yes(d != NULL);
    d = DeferWindowPos(d, g_c2, NULL, 125, 15, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    d = DeferWindowPos(d, g_c3, NULL, 0, 0, 90, 50, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    say(" ");
    say_yes(d != NULL);
    line("end defer");
    say_result(EndDeferWindowPos(d));
    say_geometry("c2", g_c2);
    say_geometry("c3", g_c3);
    line("c2 into c4");
    say_window(SetParent(g_c2, g_c4));
    say(" parent ");
    say_window(GetParent(g_c2));
    say_geometry("c2", g_c2);
    say_order(g_c4);
    line("c2 back");
    say_window(SetParent(g_c2, g_main));
    say_geometry("c2", g_c2);
    say_order(g_main);
    SetWindowPos(g_c2, g_c1, 120, 10, 100, 60, SWP_NOACTIVATE);
    SetWindowPos(g_c3, g_c2, 230, 10, 100, 60, SWP_NOACTIVATE);
    g_trace = 0;
    say_order(g_main);
}

static void scrolling_info(void) {
    SCROLLINFO si;
    si.cbSize = sizeof(si);
    si.fMask = SIF_ALL;
    line("c4 vertical at first");
    say_result(GetScrollInfo(g_c4, SB_VERT, &si));
    say(" ");
    say_int(si.nMin);
    say("..");
    say_int(si.nMax);
    say(" page ");
    say_int((long)si.nPage);
    say(" pos ");
    say_int(si.nPos);
    si.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
    si.nMin = 0;
    si.nMax = 50;
    si.nPage = 10;
    si.nPos = 45;
    g_trace = 1;
    line("set range, page and 45");
    say_int(SetScrollInfo(g_c4, SB_VERT, &si, TRUE));
    g_trace = 0;
    si.fMask = SIF_ALL;
    GetScrollInfo(g_c4, SB_VERT, &si);
    say(" now ");
    say_int(si.nMin);
    say("..");
    say_int(si.nMax);
    say(" page ");
    say_int((long)si.nPage);
    say(" pos ");
    say_int(si.nPos);
    line("set pos 5");
    say_int(SetScrollPos(g_c4, SB_VERT, 5, TRUE));
    say(" get ");
    say_int(GetScrollPos(g_c4, SB_VERT));
    line("set pos -3");
    say_int(SetScrollPos(g_c4, SB_VERT, -3, TRUE));
    say(" get ");
    say_int(GetScrollPos(g_c4, SB_VERT));
    line("page 100");
    si.fMask = SIF_PAGE;
    si.nPage = 100;
    say_int(SetScrollInfo(g_c4, SB_VERT, &si, TRUE));
    si.fMask = SIF_ALL;
    GetScrollInfo(g_c4, SB_VERT, &si);
    say(" page ");
    say_int((long)si.nPage);
    line("range 10..20");
    say_result(SetScrollRange(g_c4, SB_HORZ, 10, 20, TRUE));
    int lo = 0;
    int hi = 0;
    GetScrollRange(g_c4, SB_HORZ, &lo, &hi);
    say(" ");
    say_int(lo);
    say("..");
    say_int(hi);
    say(" pos ");
    say_int(GetScrollPos(g_c4, SB_HORZ));
    line("c1 has no bar");
    SetLastError(0);
    si.fMask = SIF_ALL;
    say_result(GetScrollInfo(g_c1, SB_VERT, &si));
    line("c1 SB_CTL");
    SetLastError(0);
    say_result(GetScrollInfo(g_c1, SB_CTL, &si));
    line("c1 set pos");
    SetLastError(0);
    say_int(SetScrollPos(g_c1, SB_VERT, 3, FALSE));
    say_error();
    say(" style now ");
    say_hex((DWORD)GetWindowLongW(g_c1, GWL_STYLE));
    g_trace = 1;
    line("hide c4's horizontal bar");
    say_result(ShowScrollBar(g_c4, SB_HORZ, FALSE));
    g_trace = 0;
    say_geometry("c4", g_c4);
    say(" style ");
    say_hex((DWORD)GetWindowLongW(g_c4, GWL_STYLE));
    g_trace = 1;
    line("show it");
    say_result(ShowScrollBar(g_c4, SB_HORZ, TRUE));
    g_trace = 0;
    say_geometry("c4", g_c4);
    line("disable both arrows");
    say_result(EnableScrollBar(g_c4, SB_VERT, ESB_DISABLE_BOTH));
    line("again");
    SetLastError(0);
    say_result(EnableScrollBar(g_c4, SB_VERT, ESB_DISABLE_BOTH));
    line("enable");
    say_result(EnableScrollBar(g_c4, SB_VERT, ESB_ENABLE_BOTH));
    line("c1 now");
    SetWindowLongW(g_c1, GWL_STYLE, GetWindowLongW(g_c1, GWL_STYLE) & ~(WS_VSCROLL | WS_HSCROLL));
    SetWindowPos(g_c1, NULL, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    say_geometry("c1", g_c1);
}

static void show_and_focus(void) {
    g_trace = 1;
    line("show main");
    ShowWindow(g_main, SW_SHOW);
    UpdateWindow(g_main);
    g_trace = 0;
    pump();
    line("focus");
    say_window(GetFocus());
    say(" active ");
    say_window(GetActiveWindow());
    say(" foreground ");
    say_yes(GetForegroundWindow() == g_main);
    g_trace = 1;
    line("focus c1");
    say_window(SetFocus(g_c1));
    line("focus c2");
    say_window(SetFocus(g_c2));
    g_veto_focus = 1;
    line("focus c3, vetoed");
    say_window(SetFocus(g_c3));
    g_veto_focus = 0;
    say(" focus ");
    say_window(GetFocus());
    line("focus none");
    say_window(SetFocus(NULL));
    say(" focus ");
    say_window(GetFocus());
    line("focus c1 again");
    say_window(SetFocus(g_c1));
    g_trace = 0;
}

static void carets(void) {
    POINT p;
    line("caret pos with none");
    SetLastError(0);
    say_result(SetCaretPos(1, 2));
    line("create");
    say_result(CreateCaret(g_c1, NULL, 2, 12));
    line("pos");
    say_result(SetCaretPos(5, 6));
    say_result(GetCaretPos(&p));
    say(" ");
    say_int(p.x);
    say(",");
    say_int(p.y);
    line("show");
    say_result(ShowCaret(g_c1));
    line("show on another window");
    SetLastError(0);
    say_result(ShowCaret(g_c2));
    line("hide");
    say_result(HideCaret(g_c1));
    line("hide NULL");
    say_result(HideCaret(NULL));
    line("show NULL");
    say_result(ShowCaret(NULL));
    say_result(ShowCaret(NULL));
    line("a new caret on c2");
    say_result(CreateCaret(g_c2, NULL, 1, 10));
    say_result(GetCaretPos(&p));
    say(" ");
    say_int(p.x);
    say(",");
    say_int(p.y);
    line("hide on c1");
    SetLastError(0);
    say_result(HideCaret(g_c1));
    line("destroy");
    say_result(DestroyCaret());
    line("destroy again");
    SetLastError(0);
    say_result(DestroyCaret());
    line("blink");
    say_int((long)GetCaretBlinkTime());
}

/* Four squares on white, each 10 pixels, read back after scrolling. */
static void draw_squares(HWND h) {
    HDC dc = GetDC(h);
    RECT all;
    GetClientRect(h, &all);
    HBRUSH white = CreateSolidBrush(RGB(255, 255, 255));
    FillRect(dc, &all, white);
    DeleteObject(white);
    static const COLORREF k_colors[4] = {RGB(255, 0, 0), RGB(0, 255, 0), RGB(0, 0, 255), RGB(0, 0, 0)};
    for (int i = 0; i < 4; ++i) {
        HBRUSH b = CreateSolidBrush(k_colors[i]);
        RECT r = {10 + 20 * i, 10, 20 + 20 * i, 20};
        FillRect(dc, &r, b);
        DeleteObject(b);
    }
    ReleaseDC(h, dc);
}

static void say_color(COLORREF c) {
    switch (c) {
    case RGB(255, 255, 255): say("white"); break;
    case RGB(255, 0, 0): say("red"); break;
    case RGB(0, 255, 0): say("green"); break;
    case RGB(0, 0, 255): say("blue"); break;
    case RGB(0, 0, 0): say("black"); break;
    default: say_hex(c); break;
    }
}

static void say_pixels(HWND h) {
    static const POINT k_points[] = {{12, 12}, {17, 19}, {22, 17}, {32, 12}, {52, 12}, {75, 25}, {15, 25}, {5, 5}};
    HDC dc = GetDC(h);
    for (unsigned i = 0; i < sizeof(k_points) / sizeof(k_points[0]); ++i) {
        say(" ");
        say_color(GetPixel(dc, k_points[i].x, k_points[i].y));
    }
    ReleaseDC(h, dc);
}

static void say_update(HWND h) {
    RECT r;
    const BOOL any = GetUpdateRect(h, &r, FALSE);
    say(" update ");
    if (any) {
        say_rect(&r);
    } else {
        say("none");
    }
}

static void scrolling(void) {
    ShowWindow(g_grand, SW_HIDE);
    pump();
    ValidateRect(g_c1, NULL);
    draw_squares(g_c1);
    line("before");
    say_pixels(g_c1);
    RECT updated;
    line("scroll 5,7");
    say_int(ScrollWindowEx(g_c1, 5, 7, NULL, NULL, NULL, &updated, SW_INVALIDATE));
    say(" ");
    say_rect(&updated);
    say_update(g_c1);
    say_pixels(g_c1);
    line("validate");
    say_result(ValidateRect(g_c1, NULL));
    say_update(g_c1);
    draw_squares(g_c1);
    RECT part = {20, 0, 60, 30};
    line("scroll part -10,0");
    say_int(ScrollWindowEx(g_c1, -10, 0, &part, NULL, NULL, &updated, SW_INVALIDATE));
    say(" ");
    say_rect(&updated);
    say_update(g_c1);
    say_pixels(g_c1);
    ValidateRect(g_c1, NULL);
    draw_squares(g_c1);
    RECT clip = {0, 0, 40, 40};
    line("scroll 0,4 clipped");
    say_int(ScrollWindowEx(g_c1, 0, 4, NULL, &clip, NULL, &updated, 0));
    say(" ");
    say_rect(&updated);
    say_update(g_c1);
    say_pixels(g_c1);
    ValidateRect(g_c1, NULL);
    line("scroll nothing");
    say_int(ScrollWindowEx(g_c1, 0, 0, NULL, NULL, NULL, &updated, SW_INVALIDATE));
    say(" ");
    say_rect(&updated);
    ShowWindow(g_grand, SW_SHOWNA);
    pump();
    /* Windows XP's WM_MOVE for a child scrolled with its parent counts its
     * y from elsewhere; where the child ends up is what is compared. */
    line("scroll children");
    say_int(ScrollWindowEx(g_c1, 3, 2, NULL, NULL, NULL, NULL, SW_SCROLLCHILDREN | SW_INVALIDATE | SW_ERASE));
    say_geometry("grand", g_grand);
    say_update(g_c1);
    line("scroll window back");
    say_result(ScrollWindow(g_c1, -3, -2, NULL, NULL));
    say_geometry("grand", g_grand);
    pump();
    g_trace = 1;
    line("redraw");
    say_result(RedrawWindow(g_c1, NULL, NULL, RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_UPDATENOW | RDW_ALLCHILDREN));
    line("redraw without children");
    say_result(RedrawWindow(g_c1, NULL, NULL, RDW_INVALIDATE | RDW_ERASE | RDW_UPDATENOW | RDW_NOCHILDREN));
    line("invalidate, then validate a part");
    RECT half = {0, 0, 50, 70};
    InvalidateRect(g_c1, NULL, FALSE);
    ValidateRect(g_c1, &half);
    say_update(g_c1);
    g_trace = 0;
    line("update");
    UpdateWindow(g_c1);
    say_update(g_c1);
}

static void say_menu_items(HMENU m) {
    const int n = GetMenuItemCount(m);
    say(" count ");
    say_int(n);
    for (int i = 0; i < n; ++i) {
        WCHAR text[64];
        say(" | ");
        say_int((long)(int)GetMenuItemID(m, i));
        say(" ");
        say_hex(GetMenuState(m, (UINT)i, MF_BYPOSITION));
        say(" ");
        GetMenuStringW(m, (UINT)i, text, 64, MF_BYPOSITION);
        say_w(text);
    }
}

static void menus(void) {
    g_bar = CreateMenu();
    g_file = CreatePopupMenu();
    g_edit = CreatePopupMenu();
    line("made");
    say_yes(g_bar != NULL && g_file != NULL && g_edit != NULL);
    say(" is menu ");
    say_yes(IsMenu(g_bar));
    say(" ");
    say_yes(IsMenu((HMENU)0x1234));
    line("append");
    say_result(AppendMenuW(g_file, MF_STRING, 200, L"&Open\tCtrl+O"));
    say_result(AppendMenuW(g_file, MF_SEPARATOR, 0, NULL));
    say_result(AppendMenuW(g_file, MF_STRING | MF_GRAYED, 201, L"&Save"));
    say_result(AppendMenuW(g_file, MF_STRING | MF_CHECKED, 202, L"&Wrap"));
    say_result(AppendMenuW(g_edit, MF_STRING, 300, L"Cu&t"));
    say_result(AppendMenuW(g_bar, MF_POPUP, (UINT_PTR)g_file, L"&File"));
    say_result(AppendMenuW(g_bar, MF_POPUP, (UINT_PTR)g_edit, L"&Edit"));
    line("file");
    say_menu_items(g_file);
    line("bar");
    say_menu_items(g_bar);
    line("insert");
    say_result(InsertMenuW(g_file, 1, MF_BYPOSITION | MF_STRING, 203, L"&New"));
    say_result(InsertMenuW(g_file, 202, MF_BYCOMMAND | MF_STRING, 204, L"Before wrap"));
    say_result(InsertMenuW(g_file, 99, MF_BYPOSITION | MF_STRING, 205, L"At the end"));
    say_result(InsertMenuW(g_bar, 202, MF_BYCOMMAND | MF_STRING, 206, L"Through the bar"));
    line("file");
    say_menu_items(g_file);
    line("check wrap");
    say_hex(CheckMenuItem(g_bar, 202, MF_BYCOMMAND | MF_UNCHECKED));
    say(" ");
    say_hex(CheckMenuItem(g_file, 202, MF_BYCOMMAND | MF_UNCHECKED));
    say(" ");
    say_hex(CheckMenuItem(g_file, 0, MF_BYPOSITION | MF_CHECKED));
    say(" missing ");
    say_hex(CheckMenuItem(g_file, 999, MF_BYCOMMAND | MF_CHECKED));
    line("enable save");
    say_int(EnableMenuItem(g_bar, 201, MF_BYCOMMAND | MF_ENABLED));
    say(" ");
    say_int(EnableMenuItem(g_bar, 201, MF_BYCOMMAND | MF_DISABLED));
    say(" ");
    say_int(EnableMenuItem(g_bar, 999, MF_BYCOMMAND | MF_DISABLED));
    line("modify");
    say_result(ModifyMenuW(g_file, 203, MF_BYCOMMAND | MF_STRING, 207, L"&Blank"));
    say_result(ModifyMenuW(g_file, 0, MF_BYPOSITION | MF_STRING | MF_GRAYED, 200, L"&Open..."));
    line("file");
    say_menu_items(g_file);
    line("state of the separator");
    say_hex(GetMenuState(g_file, 2, MF_BYPOSITION));
    say(" missing ");
    say_hex(GetMenuState(g_file, 999, MF_BYCOMMAND));
    WCHAR text[8];
    line("string cut");
    say_int(GetMenuStringW(g_file, 207, text, 4, MF_BYCOMMAND));
    say(" ");
    say_w(text);
    say(" length ");
    say_int(GetMenuStringW(g_file, 207, NULL, 0, MF_BYCOMMAND));
    line("sub menu");
    say_yes(GetSubMenu(g_bar, 0) == g_file);
    say(" ");
    say_yes(GetSubMenu(g_bar, 1) == g_edit);
    say(" ");
    say_yes(GetSubMenu(g_file, 0) == NULL);
    say(" id of a popup ");
    say_int((long)(int)GetMenuItemID(g_bar, 0));
    say(" out of range ");
    say_int((long)(int)GetMenuItemID(g_bar, 9));

    RECT before;
    RECT after;
    GetClientRect(g_main, &before);
    g_trace = 1;
    line("set the menu");
    say_result(SetMenu(g_main, g_bar));
    g_trace = 0;
    GetClientRect(g_main, &after);
    /* How much smaller is the window manager's on SavanXP. */
    say(" smaller ");
    say_yes(after.bottom < before.bottom);
    say(" menu ");
    say_yes(GetMenu(g_main) == g_bar);
    line("draw the bar");
    say_result(DrawMenuBar(g_main));
    line("remove edit");
    say_result(RemoveMenu(g_bar, 1, MF_BYPOSITION));
    say(" still a menu ");
    say_yes(IsMenu(g_edit));
    line("bar");
    say_menu_items(g_bar);
    line("delete file");
    say_result(DeleteMenu(g_bar, 0, MF_BYPOSITION));
    say(" still a menu ");
    say_yes(IsMenu(g_file));
    line("delete missing");
    SetLastError(0);
    say_result(DeleteMenu(g_bar, 5, MF_BYPOSITION));
    g_trace = 1;
    line("no menu");
    say_result(SetMenu(g_main, NULL));
    g_trace = 0;
    GetClientRect(g_main, &after);
    say(" as before ");
    say_yes(after.bottom == before.bottom);
    line("destroy");
    say_result(DestroyMenu(g_bar));
    say(" again ");
    SetLastError(0);
    say_result(DestroyMenu(g_bar));
    say(" edit ");
    say_result(DestroyMenu(g_edit));
    line("count of none");
    SetLastError(0);
    say_int(GetMenuItemCount(g_bar));
    say_error();
    line("check marks");
    say_hex((DWORD)GetMenuCheckMarkDimensions());
    line("system menu");
    say_yes(GetSystemMenu(g_main, FALSE) != NULL);
}

static INT_PTR CALLBACK dialog_proc(HWND hwnd, UINT m, WPARAM wParam, LPARAM lParam) {
    trace_message("dialog", hwnd, m, wParam, lParam);
    switch (m) {
    case WM_INITDIALOG:
        if (lParam == 88) {
            PostMessageW(hwnd, WM_USER + 2, 0, 0);
            PostMessageW(hwnd, WM_USER + 1, 0, 0);
        }
        return TRUE;
    case WM_USER + 1:
        EndDialog(hwnd, 42);
        return TRUE;
    case WM_USER + 5:
        SetWindowLongW(hwnd, DWL_MSGRESULT, 1234);
        return TRUE;
    case WM_COMMAND:
        return TRUE;
    default:
        return FALSE;
    }
}

/* A dialog template in memory: the classic format, MS Shell Dlg 8. */
static WORD g_template[256];
static int g_at;

static void put(WORD w) {
    g_template[g_at++] = w;
}

static void put_text(const WCHAR *t) {
    do {
        put(*t);
    } while (*t++ != 0);
}

static void put_item(DWORD style, int x, int y, int cx, int cy, WORD id, WORD cls, const WCHAR *text) {
    if (g_at & 1) {
        put(0);
    }
    style |= WS_CHILD | WS_VISIBLE;
    put(LOWORD(style));
    put(HIWORD(style));
    put(0);
    put(0);
    put((WORD)x);
    put((WORD)y);
    put((WORD)cx);
    put((WORD)cy);
    put(id);
    put(0xffff);
    put(cls);
    put_text(text);
    put(0);
}

static const DLGTEMPLATE *make_template(DWORD style) {
    g_at = 0;
    style |= DS_SETFONT | DS_MODALFRAME | WS_POPUP | WS_CAPTION | WS_SYSMENU;
    put(LOWORD(style));
    put(HIWORD(style));
    put(0);
    put(0);
    put(6);
    put(10);
    put(10);
    put(180);
    put(90);
    put(0);
    put(0);
    put_text(L"Modeless");
    put(8);
    put_text(L"MS Shell Dlg");
    put_item(BS_PUSHBUTTON | WS_TABSTOP | WS_GROUP, 10, 10, 60, 14, 301, 0x80, L"&First");
    put_item(BS_AUTOCHECKBOX | WS_TABSTOP, 10, 30, 60, 12, 302, 0x80, L"&Second");
    put_item(BS_AUTORADIOBUTTON | WS_TABSTOP | WS_GROUP, 10, 50, 60, 12, 303, 0x80, L"&Up");
    put_item(BS_AUTORADIOBUTTON, 10, 64, 60, 12, 304, 0x80, L"&Down");
    put_item(BS_DEFPUSHBUTTON | WS_TABSTOP | WS_GROUP, 110, 10, 50, 14, IDOK, 0x80, L"OK");
    put_item(BS_PUSHBUTTON | WS_TABSTOP, 110, 30, 50, 14, IDCANCEL, 0x80, L"Cancel");
    return (const DLGTEMPLATE *)g_template;
}

static void feed(UINT message, WPARAM wParam) {
    MSG m;
    m.hwnd = GetFocus();
    m.message = message;
    m.wParam = wParam;
    m.lParam = 1;
    m.time = 0;
    m.pt.x = 0;
    m.pt.y = 0;
    line("feed ");
    say_message(message);
    say(" ");
    say_hex((DWORD)wParam);
    say(" to ");
    say_window(m.hwnd);
    const BOOL taken = IsDialogMessageW(g_dialog, &m);
    line("taken ");
    say_yes(taken);
    say(", focus ");
    say_window(GetFocus());
}

static void dialogs(void) {
    g_trace = 1;
    g_trace_filter = 1;
    line("modeless");
    g_dialog = CreateDialogIndirectParamW(g_instance, make_template(WS_VISIBLE), g_main, dialog_proc, 77);
    say_yes(g_dialog != NULL);
    say(" owner enabled ");
    say_yes(IsWindowEnabled(g_main));
    say(" visible ");
    say_yes(IsWindowVisible(g_dialog));
    say(" focus ");
    say_window(GetFocus());
    g_trace = 0;
    pump();
    g_trace = 1;
    line("dialog procedure ours");
    say_yes((DLGPROC)GetWindowLongW(g_dialog, DWL_DLGPROC) == dialog_proc);
    say(" user ");
    say_int(SetWindowLongW(g_dialog, DWL_USER, 9));
    say(" ");
    say_int(GetWindowLongW(g_dialog, DWL_USER));
    line("result through DWL_MSGRESULT");
    say_int(SendMessageW(g_dialog, WM_USER + 5, 0, 0));
    line("base units");
    say_hex((DWORD)GetDialogBaseUnits());
    RECT r = {0, 0, 4, 8};
    MapDialogRect(g_dialog, &r);
    say(" map ");
    say_rect(&r);
    HWND first = GetDlgItem(g_dialog, 301);
    line("next tab item");
    say_window(GetNextDlgTabItem(g_dialog, first, FALSE));
    say(" previous ");
    say_window(GetNextDlgTabItem(g_dialog, first, TRUE));
    say(" from none ");
    say_window(GetNextDlgTabItem(g_dialog, NULL, FALSE));
    line("next group item");
    say_window(GetNextDlgGroupItem(g_dialog, GetDlgItem(g_dialog, 303), FALSE));
    say(" previous ");
    say_window(GetNextDlgGroupItem(g_dialog, GetDlgItem(g_dialog, 303), TRUE));
    say(" of first ");
    say_window(GetNextDlgGroupItem(g_dialog, first, FALSE));
    line("set int -5");
    say_result(SetDlgItemInt(g_dialog, 301, (UINT)-5, TRUE));
    WCHAR text[16];
    say(" text ");
    say_int((long)GetDlgItemTextW(g_dialog, 301, text, 16));
    say(" ");
    say_w(text);
    BOOL ok = FALSE;
    say(" signed ");
    say_int((long)(int)GetDlgItemInt(g_dialog, 301, &ok, TRUE));
    say(" ");
    say_yes(ok);
    say(" unsigned ");
    say_int((long)(int)GetDlgItemInt(g_dialog, 301, &ok, FALSE));
    say(" ");
    say_yes(ok);
    SetDlgItemTextW(g_dialog, 301, L"12a");
    line("12a");
    say_int((long)(int)GetDlgItemInt(g_dialog, 301, &ok, FALSE));
    say(" ");
    say_yes(ok);
    SetDlgItemTextW(g_dialog, 301, L" 34");
    line(" 34");
    say_int((long)(int)GetDlgItemInt(g_dialog, 301, &ok, FALSE));
    say(" ");
    say_yes(ok);
    SetDlgItemTextW(g_dialog, 301, L"&First");
    line("send to an item");
    say_int(SendDlgItemMessageW(g_dialog, 302, BM_GETCHECK, 0, 0));
    line("missing item");
    SetLastError(0);
    say_int(SendDlgItemMessageW(g_dialog, 999, BM_GETCHECK, 0, 0));
    say_error();

    feed(WM_KEYDOWN, VK_TAB);
    feed(WM_KEYDOWN, VK_TAB);
    feed(WM_KEYDOWN, VK_DOWN);
    feed(WM_KEYDOWN, VK_DOWN);
    feed(WM_CHAR, 's');
    line("second checked");
    say_int((long)IsDlgButtonChecked(g_dialog, 302));
    feed(WM_CHAR, 'f');
    feed(WM_KEYDOWN, VK_RETURN);
    feed(WM_KEYDOWN, VK_ESCAPE);
    feed(WM_KEYDOWN, 'X');
    line("for the main window");
    MSG other;
    other.hwnd = g_main;
    other.message = WM_KEYDOWN;
    other.wParam = VK_TAB;
    other.lParam = 1;
    other.time = 0;
    other.pt.x = other.pt.y = 0;
    say_yes(IsDialogMessageW(g_dialog, &other));
    g_trace = 0;
    pump();
    g_trace = 1;
    line("destroy the modeless dialog");
    say_result(DestroyWindow(g_dialog));
    g_dialog = NULL;
    g_trace = 0;
    pump();

    g_trace = 1;
    line("modal");
    const INT_PTR result = DialogBoxIndirectParamW(g_instance, make_template(0), g_main, dialog_proc, 88);
    line("modal result");
    say_int(result);
    say(" focus ");
    say_window(GetFocus());
    g_dialog = NULL;
    g_trace = 0;
    g_trace_filter = 0;
    pump();
}

static void messages(void) {
    g_trace_cwp = 1;
    g_trace = 1;
    line("send");
    say_int(SendMessageW(g_c1, WM_USER + 7, 3, 4));
    line("post and dispatch");
    PostMessageW(g_c1, WM_USER + 8, 0, 0);
    pump();
    ULONG_PTR answer = 0;
    line("send with a time-out");
    say_int(SendMessageTimeoutW(g_c1, WM_USER + 7, 5, 6, 0, 100, &answer));
    say(" ");
    say_int((long)answer);
    line("in send");
    say_yes(InSendMessage());
    line("show c1's grandchild");
    ShowWindow(g_grand, SW_HIDE);
    g_trace_cwp = 0;
    g_trace = 0;
    pump();
    line("get message");
    PostMessageW(g_main, WM_USER + 31, 1, 0);
    PostMessageW(g_main, WM_USER + 32, 2, 0);
    MSG m;
    say_yes(PeekMessageW(&m, NULL, WM_USER + 31, WM_USER + 31, PM_NOREMOVE));
    line("peeked wparam");
    say_int((long)m.wParam);
    GetMessageW(&m, NULL, WM_USER + 31, WM_USER + 31);
    line("got wparam");
    say_int((long)m.wParam);
    GetMessageW(&m, NULL, WM_USER + 32, WM_USER + 32);
    line("time and position");
    say_yes(GetMessageTime() == (LONG)m.time);
    say(" ");
    say_yes(GetMessagePos() == (DWORD)MAKELPARAM(m.pt.x, m.pt.y));
}

static void ending(void) {
    g_trace = 1;
    line("close through the system command");
    SendMessageW(g_main, WM_SYSCOMMAND, SC_CLOSE, 0);
    g_trace = 0;
    line("main is gone");
    say_yes(!IsWindow(g_main));
    say(" owned too ");
    say_yes(!IsWindow(g_owned));
    line("unhook");
    say_result(UnhookWindowsHookEx(g_cbt));
    say_result(UnhookWindowsHookEx(g_cwp));
    say_result(UnhookWindowsHookEx(g_cwpret));
    say_result(UnhookWindowsHookEx(g_getmsg));
    say_result(UnhookWindowsHookEx(g_filter));
    line("unhook again");
    SetLastError(0);
    say_result(UnhookWindowsHookEx(g_cbt));
    line("unregister");
    say_result(UnregisterClassW(k_child_class, g_instance));
    say_result(UnregisterClassW(k_main_class, g_instance));
    say("\r\n");
}

void start(void) {
    const WCHAR *command = GetCommandLineW();
    for (const WCHAR *c = command; *c != 0; ++c) {
        if (c[0] == ' ' && c[1] == 'a' && c[2] == 'l' && c[3] == 'l' && (c[4] == 0 || c[4] == ' ')) {
            g_all = 1;
        }
    }
    g_instance = GetModuleHandleW(NULL);
    say("hooks.exe");
    install_hooks();
    create_windows();
    window_longs();
    classes();
    properties();
    tree();
    positions();
    scrolling_info();
    show_and_focus();
    carets();
    scrolling();
    menus();
    dialogs();
    messages();
    ending();
    ExitProcess(0);
}
