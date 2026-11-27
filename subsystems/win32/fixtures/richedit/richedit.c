/*
 * richedit.exe -- The WP7a fixture of the RichEdit control's text
 * (WIN32_WORDPAD_ROADMAP.md, WP7): a RICHEDIT50W window from msftedit.dll,
 * Microsoft's on Windows XP (the user's own file, next to the program) and
 * SSW's here, asked what WordPad asks of it.
 *
 * Without input it asks the class and the control's defaults, then sets
 * text and reads it back with every length and text message, selects,
 * replaces, undoes and redoes, types with WM_CHAR, hits the text limit,
 * lays lines out and asks positions both ways, moves the formatting
 * rectangle, wraps and stops wrapping, scrolls, finds word breaks, and goes
 * through the clipboard. The parent writes the control's WM_COMMAND and
 * WM_NOTIFY notifications as they come.
 *
 * The control's font is Liberation Sans at -13, whose metrics and widths
 * are Windows XP's here (WP6), so positions and line breaks compare. Windows
 * XP is given the face from the files next to the program.
 *
 * The client area is 400x300 and starts with a 4x4 magenta marker.
 */
#include <windows.h>
#include <richedit.h>

#include "../kernel/print.h"

#define W(s) ((const WCHAR *)L##s)

DECLSPEC_IMPORT HMODULE WINAPI LoadLibraryW(LPCWSTR name);
DECLSPEC_IMPORT HMODULE WINAPI GetModuleHandleW(LPCWSTR name);

#define CLIENT_WIDTH 400
#define CLIENT_HEIGHT 300
#define ID_RICH 100
#define ID_SINGLE 101

static HINSTANCE g_instance;
static HWND g_main;
static HWND g_rich;
static HFONT g_font;
static int g_log; /* the parent writes notifications */

/* ---- Output -------------------------------------------------------------------------------- */

/* Text in brackets, with CR, LF and anything outside printable ASCII
 * written as escapes, so every byte of the output is plain. */
static void say_text(const WCHAR *text, int n) {
    char out[8];
    say("[");
    for (int i = 0; i < n; ++i) {
        const unsigned c = text[i];
        if (c == '\r') {
            say("\\r");
        } else if (c == '\n') {
            say("\\n");
        } else if (c == '\\') {
            say("\\\\");
        } else if (c >= 0x20 && c < 0x7f) {
            out[0] = (char)c;
            out[1] = 0;
            say(out);
        } else {
            static const char k_hex[] = "0123456789abcdef";
            out[0] = '\\';
            out[1] = 'u';
            out[2] = k_hex[(c >> 12) & 15u];
            out[3] = k_hex[(c >> 8) & 15u];
            out[4] = k_hex[(c >> 4) & 15u];
            out[5] = k_hex[c & 15u];
            out[6] = 0;
            say(out);
        }
    }
    say("]");
}

static int wlen(const WCHAR *s) {
    int n = 0;
    while (s[n] != 0) {
        ++n;
    }
    return n;
}

static LRESULT rich(UINT msg, WPARAM w, LPARAM l) {
    return SendMessageW(g_rich, msg, w, l);
}

/* Sends a message, then writes its label and result: whatever the control
 * tells its parent on the way comes first. */
static LRESULT ask(const char *label, UINT msg, WPARAM w, LPARAM l, int hex) {
    const LRESULT got = rich(msg, w, l);
    line(label);
    say("returns ");
    if (hex) {
        say_hex((unsigned long)got);
    } else {
        say_int((long)got);
    }
    return got;
}

static void pump(void) {
    MSG m;
    while (PeekMessageW(&m, NULL, 0, 0, PM_REMOVE)) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
}

/* ---- What the parent hears ---------------------------------------------------------------- */

static const char *command_name(int code) {
    switch (code) {
    case EN_SETFOCUS: return "EN_SETFOCUS";
    case EN_KILLFOCUS: return "EN_KILLFOCUS";
    case EN_CHANGE: return "EN_CHANGE";
    case EN_UPDATE: return "EN_UPDATE";
    case EN_ERRSPACE: return "EN_ERRSPACE";
    case EN_MAXTEXT: return "EN_MAXTEXT";
    case EN_HSCROLL: return "EN_HSCROLL";
    case EN_VSCROLL: return "EN_VSCROLL";
    default: return 0;
    }
}

static const char *notify_name(UINT code) {
    switch (code) {
    case EN_MSGFILTER: return "EN_MSGFILTER";
    case EN_REQUESTRESIZE: return "EN_REQUESTRESIZE";
    case EN_SELCHANGE: return "EN_SELCHANGE";
    case EN_DROPFILES: return "EN_DROPFILES";
    case EN_PROTECTED: return "EN_PROTECTED";
    case EN_SAVECLIPBOARD: return "EN_SAVECLIPBOARD";
    case EN_LINK: return "EN_LINK";
    case EN_DRAGDROPDONE: return "EN_DRAGDROPDONE";
    default: return 0;
    }
}

static const char *id_name(UINT id) {
    return id == ID_RICH ? "rich" : id == ID_SINGLE ? "single" : "other";
}

static LRESULT CALLBACK main_proc(HWND h, UINT msg, WPARAM w, LPARAM l) {
    switch (msg) {
    case WM_COMMAND:
        if (g_log && l != 0) {
            const char *name = command_name(HIWORD(w));
            line("  command ");
            say(id_name(LOWORD(w)));
            say(" ");
            if (name != 0) {
                say(name);
            } else {
                say_hex(HIWORD(w));
            }
        }
        return 0;
    case WM_NOTIFY:
        if (g_log) {
            const NMHDR *n = (const NMHDR *)l;
            const char *name = notify_name(n->code);
            line("  notify ");
            say(id_name((UINT)n->idFrom));
            say(" ");
            if (name != 0) {
                say(name);
            } else {
                say_hex(n->code);
            }
            say(n->hwndFrom == GetDlgItem(h, (int)n->idFrom) ? "" : " (hwndFrom differs)");
            if (n->code == EN_SELCHANGE) {
                const SELCHANGE *s = (const SELCHANGE *)l;
                say(" ");
                say_int(s->chrg.cpMin);
                say("-");
                say_int(s->chrg.cpMax);
                say(" type ");
                say_hex(s->seltyp);
            } else if (n->code == EN_REQUESTRESIZE) {
                const REQRESIZE *r = (const REQRESIZE *)l;
                say(" ");
                say_int(r->rc.left);
                say(",");
                say_int(r->rc.top);
                say(",");
                say_int(r->rc.right);
                say(",");
                say_int(r->rc.bottom);
            }
        }
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT marker = {0, 0, 4, 4};
        HBRUSH magenta = CreateSolidBrush(RGB(255, 0, 255));
        FillRect(dc, &marker, magenta);
        DeleteObject(magenta);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_CLOSE:
        DestroyWindow(h);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcW(h, msg, w, l);
    }
}

/* ---- Reading the control back ------------------------------------------------------------- */

static void show_text(void) {
    WCHAR text[1024];
    const LRESULT length = rich(WM_GETTEXTLENGTH, 0, 0);
    const LRESULT got = rich(WM_GETTEXT, 1024, (LPARAM)text);
    line("  text ");
    say_int((long)length);
    say("/");
    say_int((long)got);
    say(" ");
    say_text(text, (int)got);
}

static void show_sel(void) {
    CHARRANGE r = {-7, -7};
    DWORD start = 7, end = 7;
    rich(EM_EXGETSEL, 0, (LPARAM)&r);
    const LRESULT packed = rich(EM_GETSEL, (WPARAM)&start, (LPARAM)&end);
    line("  sel ");
    say_int(r.cpMin);
    say("-");
    say_int(r.cpMax);
    say(" getsel ");
    say_hex((unsigned long)packed);
    say(" ");
    say_int((long)start);
    say("-");
    say_int((long)end);
}

static void show_lengths(void) {
    static const struct {
        DWORD flags;
        UINT codepage;
        const char *name;
    } k_cases[] = {
        {GTL_DEFAULT, 1200, "default"},
        {GTL_USECRLF, 1200, "crlf"},
        {GTL_PRECISE | GTL_NUMCHARS, 1200, "precise chars"},
        {GTL_USECRLF | GTL_PRECISE | GTL_NUMCHARS, 1200, "crlf precise chars"},
        {GTL_CLOSE, 1200, "close"},
        {GTL_NUMBYTES, 1200, "bytes 1200"},
        {GTL_NUMBYTES | GTL_PRECISE, 1252, "precise bytes 1252"},
        {GTL_USECRLF | GTL_NUMBYTES | GTL_PRECISE, 0, "crlf precise bytes acp"},
        {GTL_NUMCHARS | GTL_NUMBYTES, 1200, "chars and bytes"},
    };
    line("  lengths");
    for (unsigned i = 0; i < sizeof k_cases / sizeof k_cases[0]; ++i) {
        GETTEXTLENGTHEX g = {k_cases[i].flags, k_cases[i].codepage};
        say(" ");
        say(k_cases[i].name);
        say("=");
        say_int((long)rich(EM_GETTEXTLENGTHEX, (WPARAM)&g, 0));
    }
}

static void show_gettextex(DWORD flags, DWORD cb, const char *name) {
    WCHAR text[600];
    for (int i = 0; i < 600; ++i) {
        text[i] = '#';
    }
    GETTEXTEX g = {cb, flags, 1200, NULL, NULL};
    const LRESULT got = rich(EM_GETTEXTEX, (WPARAM)&g, (LPARAM)text);
    line("  gettextex ");
    say(name);
    say(" ");
    say_int((long)got);
    say(" ");
    int n = 0;
    while (n < 600 && text[n] != 0 && text[n] != '#') {
        ++n;
    }
    say_text(text, n);
    say(n < 600 && text[n] == 0 ? " terminated" : " not terminated");
}

static void show_range(LONG a, LONG b) {
    WCHAR text[600];
    for (int i = 0; i < 600; ++i) {
        text[i] = '#';
    }
    TEXTRANGEW t = {{a, b}, text};
    const LRESULT got = rich(EM_GETTEXTRANGE, 0, (LPARAM)&t);
    if (got == 0) {
        line("  range ");
        say_int(a);
        say(",");
        say_int(b);
        say(" 0 ");
        say(text[0] == 0 ? "terminated" : "untouched");
        return;
    }
    line("  range ");
    say_int(a);
    say(",");
    say_int(b);
    say(" ");
    say_int((long)got);
    say(" ");
    int n = 0;
    while (n < 600 && text[n] != 0 && text[n] != '#') {
        ++n;
    }
    say_text(text, n);
}

static void show_lines(void) {
    const LRESULT count = rich(EM_GETLINECOUNT, 0, 0);
    line("  lines ");
    say_int((long)count);
    say(" first visible ");
    say_int((long)rich(EM_GETFIRSTVISIBLELINE, 0, 0));
    for (LRESULT i = 0; i <= count; ++i) {
        const LRESULT index = rich(EM_LINEINDEX, (WPARAM)i, 0);
        line("   line ");
        say_int((long)i);
        say(" index ");
        say_int((long)index);
        say(" length ");
        say_int((long)rich(EM_LINELENGTH, (WPARAM)index, 0));
        WCHAR text[256];
        text[0] = 255;
        const LRESULT got = rich(EM_GETLINE, (WPARAM)i, (LPARAM)text);
        say(" getline ");
        say_int((long)got);
        say(" ");
        say_text(text, (int)got);
        say(" fromchar ");
        say_int((long)rich(EM_EXLINEFROMCHAR, 0, (LPARAM)index));
    }
    say(" -1 index ");
    say_int((long)rich(EM_LINEINDEX, (WPARAM)-1, 0));
    say(" -1 length ");
    say_int((long)rich(EM_LINELENGTH, (WPARAM)-1, 0));
    say(" linefromchar -1 ");
    say_int((long)rich(EM_LINEFROMCHAR, (WPARAM)-1, 0));
}

static void show_rect(const char *label) {
    RECT r = {-7, -7, -7, -7};
    rich(EM_GETRECT, 0, (LPARAM)&r);
    line("  rect ");
    say(label);
    say(" ");
    say_int(r.left);
    say(",");
    say_int(r.top);
    say(",");
    say_int(r.right);
    say(",");
    say_int(r.bottom);
    say((GetWindowLongW(g_rich, GWL_STYLE) & WS_VSCROLL) != 0 ? " bar shown" : " no bar");
}

static void show_state(void) {
    line("  modify ");
    say_int((long)rich(EM_GETMODIFY, 0, 0));
    say(" canundo ");
    say_int((long)rich(EM_CANUNDO, 0, 0));
    say(" canredo ");
    say_int((long)rich(EM_CANREDO, 0, 0));
}

static void set_text(const WCHAR *text) {
    line("settext ");
    say_text(text, wlen(text));
    const LRESULT ok = rich(WM_SETTEXT, 0, (LPARAM)text);
    line("  returns ");
    say_int((long)ok);
    show_text();
    show_sel();
}

static void exsetsel(LONG a, LONG b) {
    CHARRANGE r = {a, b};
    line("exsetsel ");
    say_int(a);
    say(",");
    say_int(b);
    const LRESULT got = rich(EM_EXSETSEL, 0, (LPARAM)&r);
    line("  returns ");
    say_int((long)got);
    show_sel();
}

static void setsel(LONG a, LONG b) {
    line("setsel ");
    say_int(a);
    say(",");
    say_int(b);
    const LRESULT got = rich(EM_SETSEL, (WPARAM)a, (LPARAM)b);
    line("  returns ");
    say_int((long)got);
    show_sel();
}

static void show_seltext(void) {
    WCHAR text[600];
    text[0] = '#';
    const LRESULT got = rich(EM_GETSELTEXT, 0, (LPARAM)text);
    line("  seltext ");
    say_int((long)got);
    say(" ");
    say_text(text, (int)got);
    say(" type ");
    say_hex((unsigned long)rich(EM_SELECTIONTYPE, 0, 0));
}

static void replace(const WCHAR *text, BOOL undo) {
    line("replacesel ");
    say_text(text, wlen(text));
    say(undo ? " undoable" : " not undoable");
    const LRESULT got = rich(EM_REPLACESEL, (WPARAM)undo, (LPARAM)text);
    line("  returns ");
    say_int((long)got);
    show_text();
    show_sel();
    show_state();
}

static void type(const char *keys) {
    line("type ");
    for (const char *c = keys; *c != 0; ++c) {
        WCHAR one = (WCHAR)(unsigned char)*c;
        say_text(&one, 1);
        rich(WM_CHAR, (WPARAM)(unsigned char)*c, 1);
    }
    show_text();
    show_sel();
    show_state();
}

static void undo_all(const char *label) {
    line(label);
    for (int i = 0; i < 12; ++i) {
        if (!rich(EM_CANUNDO, 0, 0)) {
            say(" (nothing to undo)");
            break;
        }
        const LRESULT got = rich(EM_UNDO, 0, 0);
        line("  undo returns ");
        say_int((long)got);
        show_text();
    }
    for (int i = 0; i < 12; ++i) {
        if (!rich(EM_CANREDO, 0, 0)) {
            line("  (nothing to redo)");
            break;
        }
        const LRESULT got = rich(EM_REDO, 0, 0);
        line("  redo returns ");
        say_int((long)got);
        show_text();
    }
}

static void positions(const LONG *cps, int ncps, const POINTL *points, int npoints) {
    line("  posfromchar");
    for (int i = 0; i < ncps; ++i) {
        POINTL p = {-7, -7};
        rich(EM_POSFROMCHAR, (WPARAM)&p, cps[i]);
        say(" ");
        say_int(cps[i]);
        say("=");
        say_int(p.x);
        say(",");
        say_int(p.y);
    }
    line("  charfrompos");
    for (int i = 0; i < npoints; ++i) {
        POINTL p = points[i];
        say(" ");
        say_int(p.x);
        say(",");
        say_int(p.y);
        say("=");
        say_int((long)rich(EM_CHARFROMPOS, 0, (LPARAM)&p));
    }
}

static void show_scroll(void) {
    SCROLLINFO si = {sizeof si, SIF_ALL, -7, -7, 7, -7, -7};
    const BOOL ok = GetScrollInfo(g_rich, SB_VERT, &si);
    POINT pos = {-7, -7};
    rich(EM_GETSCROLLPOS, 0, (LPARAM)&pos);
    line("  scroll ");
    say(ok ? "" : "(none) ");
    say_int(si.nMin);
    say("..");
    say_int(si.nMax);
    say(" page ");
    say_int((long)si.nPage);
    say(" pos ");
    say_int(si.nPos);
    say(" scrollpos ");
    say_int(pos.x);
    say(",");
    say_int(pos.y);
    say(" first visible ");
    say_int((long)rich(EM_GETFIRSTVISIBLELINE, 0, 0));
    say((GetWindowLongW(g_rich, GWL_STYLE) & WS_VSCROLL) != 0 ? " bar shown" : " no bar");
}

static void show_clipboard(void) {
    line("  clipboard");
    if (!OpenClipboard(g_main)) {
        say(" cannot open");
        return;
    }
    /* Only the plain clipboard's text: Windows XP's control also offers OLE's
     * formats (DataObject, Ole Private Data), which SSW's does not have, and
     * its RTF is WP7c's. */
    say(" unicode ");
    say_yes(IsClipboardFormatAvailable(CF_UNICODETEXT));
    say(" text ");
    say_yes(IsClipboardFormatAvailable(CF_TEXT));
    HANDLE h = GetClipboardData(CF_UNICODETEXT);
    if (h != NULL) {
        const WCHAR *text = (const WCHAR *)GlobalLock(h);
        line("   unicode ");
        say_text(text, wlen(text));
        GlobalUnlock(h);
    }
    CloseClipboard();
}

static void put_clipboard(const WCHAR *text) {
    const int n = wlen(text);
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, (SIZE_T)(n + 1) * 2u);
    WCHAR *p = (WCHAR *)GlobalLock(h);
    for (int i = 0; i <= n; ++i) {
        p[i] = text[i];
    }
    GlobalUnlock(h);
    OpenClipboard(g_main);
    EmptyClipboard();
    SetClipboardData(CF_UNICODETEXT, h);
    CloseClipboard();
}

/* Each part starts from an empty clipboard, whatever ran before it. */
static void empty_clipboard(void) {
    if (OpenClipboard(NULL)) {
        EmptyClipboard();
        CloseClipboard();
    }
}

/* ---- The sections ------------------------------------------------------------------------- */

static void the_class(void) {
    line("class");
    WNDCLASSW wc;
    const BOOL found = GetClassInfoW(NULL, MSFTEDIT_CLASS, &wc);
    say(found ? "RICHEDIT50W" : "RICHEDIT50W missing");
    if (found) {
        say(" style ");
        say_hex(wc.style);
        say(" class extra ");
        say_int(wc.cbClsExtra);
        say(" window extra ");
        say_int(wc.cbWndExtra);
        say(" cursor ");
        say(wc.hCursor == NULL ? "none" : wc.hCursor == LoadCursorW(NULL, IDC_IBEAM) ? "ibeam" : "other");
        say(" background ");
        say_hex((unsigned long)wc.hbrBackground);
        say(" menu ");
        say_yes(wc.lpszMenuName != NULL);
    }
    static const WCHAR *const k_others[] = {W("RICHEDIT50A"), W("RichEdit20W"), W("RichEdit20A"), W("RichEdit")};
    for (unsigned i = 0; i < 4; ++i) {
        line("  other class ");
        say_text(k_others[i], wlen(k_others[i]));
        say(GetClassInfoW(NULL, k_others[i], &wc) ? " registered" : " not registered");
    }
}

static void defaults(void) {
    line("styles ");
    say_hex((unsigned long)GetWindowLongW(g_rich, GWL_STYLE));
    say(" ex ");
    say_hex((unsigned long)GetWindowLongW(g_rich, GWL_EXSTYLE));
    RECT client;
    GetClientRect(g_rich, &client);
    say(" client ");
    say_int(client.right);
    say("x");
    say_int(client.bottom);
    line("dlgcode ");
    say_hex((unsigned long)rich(WM_GETDLGCODE, 0, 0));
    line("eventmask ");
    say_hex((unsigned long)rich(EM_GETEVENTMASK, 0, 0));
    say(" options ");
    say_hex((unsigned long)rich(EM_GETOPTIONS, 0, 0));
    say(" limit ");
    say_int((long)rich(EM_GETLIMITTEXT, 0, 0));
    say(" textmode ");
    say_hex((unsigned long)rich(EM_GETTEXTMODE, 0, 0));
    say(" editstyle ");
    say_hex((unsigned long)rich(EM_GETEDITSTYLE, 0, 0));
    say(" langoptions ");
    say_hex((unsigned long)rich(EM_GETLANGOPTIONS, 0, 0));
    say(" wordwrap ");
    say_hex((unsigned long)rich(EM_GETWORDWRAPMODE, 0, 0));
    say(" urldetect ");
    say_int((long)rich(EM_GETAUTOURLDETECT, 0, 0));
    line("font ");
    say_yes(rich(WM_GETFONT, 0, 0) != 0);
    DWORD numerator = 7, denominator = 7;
    say(" zoom ");
    say_int((long)rich(EM_GETZOOM, (WPARAM)&numerator, (LPARAM)&denominator));
    say(" ");
    say_int((long)numerator);
    say("/");
    say_int((long)denominator);
    say(" canpaste ");
    say_int((long)rich(EM_CANPASTE, 0, 0));
    show_rect("default");
    show_text();
    show_sel();
    show_state();
    show_lengths();
    show_lines();
    show_scroll();
}

static void text_and_lengths(void) {
    set_text(W(""));
    set_text(W("Hello world"));
    show_lengths();
    show_state();
    set_text(W("one\r\ntwo\nthree\rfour"));
    show_lengths();
    show_lines();
    show_gettextex(GT_DEFAULT, 1200, "default");
    show_gettextex(GT_USECRLF, 1200, "crlf");
    show_gettextex(GT_DEFAULT, 10, "default in 10 bytes");
    show_gettextex(GT_USECRLF, 12, "crlf in 12 bytes");
    show_range(2, 9);
    show_range(0, -1);
    show_range(5, 100);
    show_range(9, 2);
    set_text(W("end\r\n"));
    show_lengths();
    show_lines();
    set_text(W("a\r\r\nb\n\nc"));
    show_lengths();
    show_lines();
    set_text(W("tab\there \x00e9t\x00e9 \x20ac"));
    show_lengths();
    rich(EM_SETMODIFY, TRUE, 0);
    line("setmodify TRUE");
    show_state();
    set_text(W("after modify"));
    show_state();
}

static void selection(void) {
    set_text(W("The quick brown fox jumps over the lazy dog"));
    exsetsel(4, 9);
    show_seltext();
    exsetsel(9, 4);
    show_seltext();
    exsetsel(0, -1);
    show_seltext();
    exsetsel(-1, 0);
    exsetsel(-1, -1);
    exsetsel(10, 1000);
    exsetsel(1000, 1000);
    exsetsel(-5, 3);
    exsetsel(3, -5);
    exsetsel(20, 20);
    show_seltext();
    setsel(4, 9);
    setsel(9, 4);
    setsel(0, -1);
    setsel(-1, 0);
    setsel(-1, -1);
    setsel(5, 2);
    ask("hideselection TRUE ", EM_HIDESELECTION, TRUE, 0, 0);
    show_sel();
    ask("hideselection FALSE ", EM_HIDESELECTION, FALSE, 0, 0);
}

static void replacing(void) {
    set_text(W("The quick brown fox"));
    show_state();
    exsetsel(4, 9);
    replace(W("slow"), TRUE);
    undo_all("undo the replacement");
    exsetsel(4, 9);
    replace(W("one\r\ntwo"), TRUE);
    show_lines();
    exsetsel(0, 3);
    replace(W(""), TRUE);
    exsetsel(3, 3);
    replace(W("ABC"), FALSE);
    undo_all("undo after one that cannot be undone");
    ask("emptyundobuffer ", EM_EMPTYUNDOBUFFER, 0, 0, 0);
    show_state();
    ask("stopgrouptyping ", EM_STOPGROUPTYPING, 0, 0, 0);
}

static void typing(void) {
    set_text(W(""));
    type("ab cd");
    undo_all("undo the typing");
    set_text(W(""));
    type("ab");
    rich(EM_STOPGROUPTYPING, 0, 0);
    line("stopgrouptyping");
    type("cd");
    undo_all("undo with a stop between");
    set_text(W("xyz"));
    exsetsel(1, 2);
    type("Q");
    type("\b\b");
    exsetsel(3, 3);
    type("\r");
    type("\n");
    type("\t");
    show_lines();
    undo_all("undo enter and backspace");
    exsetsel(1, 1);
    type("\x7f");
    ask("undolimit 0 ", EM_SETUNDOLIMIT, 0, 0, 0);
    set_text(W("abc"));
    exsetsel(3, 3);
    type("d");
    replace(W("e"), TRUE);
    ask("undolimit 100 ", EM_SETUNDOLIMIT, 100, 0, 0);
}

static void limits(void) {
    ask("exlimittext 10 ", EM_EXLIMITTEXT, 0, 10, 0);
    say(" limit ");
    say_int((long)rich(EM_GETLIMITTEXT, 0, 0));
    set_text(W("0123456789abcdef"));
    line("  limit ");
    say_int((long)rich(EM_GETLIMITTEXT, 0, 0));
    set_text(W("01234567"));
    line("  limit ");
    say_int((long)rich(EM_GETLIMITTEXT, 0, 0));
    exsetsel(8, 8);
    replace(W("ABCD"), TRUE);
    type("x");
    exsetsel(0, 2);
    type("yz");
    ask("exlimittext 10 ", EM_EXLIMITTEXT, 0, 10, 0);
    set_text(W("0123456"));
    line("  limit ");
    say_int((long)rich(EM_GETLIMITTEXT, 0, 0));
    exsetsel(7, 7);
    replace(W("ABCDE"), TRUE);
    exsetsel(9, 9);
    type("xy");
    exsetsel(0, 3);
    replace(W("PQRSTU"), TRUE);
    exsetsel(0, 0);
    put_clipboard(W("long paste"));
    ask("paste ", WM_PASTE, 0, 0, 0);
    show_text();
    show_sel();
    ask("exlimittext 0 ", EM_EXLIMITTEXT, 0, 0, 0);
    say(" limit ");
    say_int((long)rich(EM_GETLIMITTEXT, 0, 0));
    ask("limittext 5 ", EM_LIMITTEXT, 5, 0, 0);
    say(" limit ");
    say_int((long)rich(EM_GETLIMITTEXT, 0, 0));
    ask("limittext 0 ", EM_LIMITTEXT, 0, 0, 0);
    say(" limit ");
    say_int((long)rich(EM_GETLIMITTEXT, 0, 0));
    ask("exlimittext 100000 ", EM_EXLIMITTEXT, 0, 100000, 0);
    say(" limit ");
    say_int((long)rich(EM_GETLIMITTEXT, 0, 0));
}

static const WCHAR k_paragraph[] =
    L"WordPad lays a paragraph out in lines as wide as the window allows, breaking after spaces and "
    L"keeping words whole.\rA second paragraph follows the first.\r\rThe third, after an empty one, "
    L"has a word much too long to fit: Pneumonoultramicroscopicsilicovolcanoconiosisandsomemoreletters.";

static void layout(void) {
    set_text((const WCHAR *)k_paragraph);
    show_lines();
    static const LONG k_cps[] = {0, 1, 7, 8, 20, 95, 96, 120, 157, 158, 159, 230, 290, 291, 400, -1};
    static const POINTL k_points[] = {{0, 0},   {1, 1},     {3, 5},     {30, 5},    {31, 5},   {50, 20},
                                      {150, 40}, {299, 5},  {320, 5},   {5, 200},   {500, 500}, {-5, -5},
                                      {5, 64},   {100, 100}};
    positions(k_cps, sizeof k_cps / sizeof k_cps[0], k_points, sizeof k_points / sizeof k_points[0]);
    show_rect("before setrect");
    RECT r = {20, 10, 200, 100};
    ask("setrect 20,10,200,100 ", EM_SETRECT, 0, (LPARAM)&r, 0);
    show_rect("after");
    show_lines();
    positions(k_cps, 4, k_points, 6);
    ask("setrect NULL ", EM_SETRECT, 0, 0, 0);
    show_rect("after");
    RECT r2 = {-10, -10, 1000, 1000};
    ask("setrectnp -10,-10,1000,1000 ", EM_SETRECTNP, 0, (LPARAM)&r2, 0);
    show_rect("after");
    show_lines();
    ask("setrect NULL ", EM_SETRECT, 0, 0, 0);
    show_rect("after");
    ask("settargetdevice NULL,1 ", EM_SETTARGETDEVICE, 0, 1, 0);
    show_rect("without wrapping");
    show_lines();
    positions(k_cps, sizeof k_cps / sizeof k_cps[0], k_points, 3);
    ask("settargetdevice NULL,0 ", EM_SETTARGETDEVICE, 0, 0, 0);
    show_rect("wrapping again");
    show_lines();
    ask("setoptions OR selectionbar ", EM_SETOPTIONS, ECOOP_OR, ECO_SELECTIONBAR, 1);
    show_rect("with selection bar");
    show_lines();
    positions(k_cps, 3, k_points, 3);
    ask("setoptions AND ~selectionbar ", EM_SETOPTIONS, ECOOP_AND, ~(LPARAM)ECO_SELECTIONBAR, 1);
    show_rect("without");
    show_lines();
    ask("setoptions SET autowordselection|autovscroll ", EM_SETOPTIONS, ECOOP_SET, ECO_AUTOWORDSELECTION | ECO_AUTOVSCROLL, 1);
    say(" style ");
    say_hex((unsigned long)GetWindowLongW(g_rich, GWL_STYLE));
    ask("setoptions SET as created ", EM_SETOPTIONS, ECOOP_SET,
                                ECO_AUTOVSCROLL | ECO_NOHIDESEL | ECO_SAVESEL, 1);
    say(" style ");
    say_hex((unsigned long)GetWindowLongW(g_rich, GWL_STYLE));
}

static void scrolling(void) {
    static WCHAR many[400];
    int n = 0;
    for (int i = 1; i <= 40; ++i) {
        many[n++] = 'l';
        many[n++] = 'i';
        many[n++] = 'n';
        many[n++] = 'e';
        many[n++] = ' ';
        if (i >= 10) {
            many[n++] = (WCHAR)('0' + i / 10);
        }
        many[n++] = (WCHAR)('0' + i % 10);
        if (i < 40) {
            many[n++] = '\r';
        }
    }
    many[n] = 0;
    set_text(many);
    show_scroll();
    ask("linescroll 0,5 ", EM_LINESCROLL, 0, 5, 0);
    show_scroll();
    ask("linescroll 0,-2 ", EM_LINESCROLL, 0, -2, 0);
    show_scroll();
    ask("scroll SB_PAGEDOWN ", EM_SCROLL, SB_PAGEDOWN, 0, 1);
    show_scroll();
    ask("scroll SB_LINEUP ", EM_SCROLL, SB_LINEUP, 0, 1);
    show_scroll();
    ask("scroll SB_PAGEUP ", EM_SCROLL, SB_PAGEUP, 0, 1);
    show_scroll();
    ask("scroll SB_PAGEUP ", EM_SCROLL, SB_PAGEUP, 0, 1);
    show_scroll();
    exsetsel(250, 250);
    ask("scrollcaret ", EM_SCROLLCARET, 0, 0, 0);
    show_scroll();
    POINT to = {0, 30};
    ask("setscrollpos 0,30 ", EM_SETSCROLLPOS, 0, (LPARAM)&to, 0);
    show_scroll();
    ask("vscroll SB_TOP ", WM_VSCROLL, SB_TOP, 0, 0);
    show_scroll();
    ask("vscroll SB_BOTTOM ", WM_VSCROLL, SB_BOTTOM, 0, 0);
    show_scroll();
    ask("vscroll SB_THUMBPOSITION 100 ", WM_VSCROLL, MAKEWPARAM(SB_THUMBPOSITION, 100), 0, 0);
    show_scroll();
    static const LONG k_cps[] = {0, 250, 280};
    static const POINTL k_points[] = {{5, 5}, {5, 60}};
    positions(k_cps, 3, k_points, 2);
    exsetsel(0, 0);
    line("after a selection at the start");
    show_scroll();
    set_text(W("short"));
    show_scroll();
}

static void word_breaks(void) {
    static const WCHAR k_text[] = L"The quick,  brown fox.\rNext-line (here) 123.45 x";
    set_text((const WCHAR *)k_text);
    static const struct {
        int code;
        const char *name;
    } k_codes[] = {
        {WB_LEFT, "left"},
        {WB_RIGHT, "right"},
        {WB_ISDELIMITER, "isdelimiter"},
        {WB_CLASSIFY, "classify"},
        {WB_MOVEWORDLEFT, "movewordleft"},
        {WB_MOVEWORDRIGHT, "movewordright"},
        {WB_LEFTBREAK, "leftbreak"},
        {WB_RIGHTBREAK, "rightbreak"},
    };
    const int n = wlen((const WCHAR *)k_text);
    for (unsigned k = 0; k < sizeof k_codes / sizeof k_codes[0]; ++k) {
        line("findwordbreak ");
        say(k_codes[k].name);
        for (int cp = 0; cp <= n + 1; ++cp) {
            say(" ");
            const LRESULT r = rich(EM_FINDWORDBREAK, (WPARAM)k_codes[k].code, cp);
            if (k_codes[k].code == WB_CLASSIFY) {
                say_hex((unsigned long)r);
            } else {
                say_int((long)r);
            }
        }
    }
}

static void clipboard(void) {
    set_text(W("Hello there\rsecond"));
    exsetsel(0, 5);
    ask("copy ", WM_COPY, 0, 0, 0);
    show_clipboard();
    show_text();
    exsetsel(6, 18);
    ask("copy across a paragraph ", WM_COPY, 0, 0, 0);
    show_clipboard();
    put_clipboard(W("pasted\r\ntext\n"));
    line("canpaste 0 ");
    say_int((long)rich(EM_CANPASTE, 0, 0));
    say(" unicode ");
    say_int((long)rich(EM_CANPASTE, CF_UNICODETEXT, 0));
    say(" text ");
    say_int((long)rich(EM_CANPASTE, CF_TEXT, 0));
    say(" bitmap ");
    say_int((long)rich(EM_CANPASTE, 2, 0));
    exsetsel(5, 5);
    ask("paste ", WM_PASTE, 0, 0, 0);
    show_text();
    show_sel();
    show_state();
    show_lines();
    exsetsel(0, 5);
    ask("cut ", WM_CUT, 0, 0, 0);
    show_text();
    show_sel();
    show_clipboard();
    undo_all("undo cut and paste");
    exsetsel(3, 3);
    ask("copy of an empty selection ", WM_COPY, 0, 0, 0);
    show_clipboard();
    static const WCHAR *const k_pastes[] = {W("x\ny"), W("a\r\nb\r\n"), W("tail\r"), W("\r\n"), W("one\r\r\ntwo"),
                                            W("\n"), W("end\n\n")};
    for (unsigned i = 0; i < sizeof k_pastes / sizeof k_pastes[0]; ++i) {
        set_text(W("[]"));
        exsetsel(1, 1);
        put_clipboard(k_pastes[i]);
        line("paste ");
        say_text(k_pastes[i], wlen(k_pastes[i]));
        ask(" ", WM_PASTE, 0, 0, 0);
        show_text();
        show_sel();
    }
    OpenClipboard(g_main);
    EmptyClipboard();
    CloseClipboard();
    line("canpaste on an empty clipboard ");
    say_int((long)rich(EM_CANPASTE, 0, 0));
    ask("paste on an empty clipboard ", WM_PASTE, 0, 0, 0);
    show_text();
}

static void read_only(void) {
    set_text(W("locked"));
    ask("setreadonly TRUE ", EM_SETREADONLY, TRUE, 0, 0);
    say(" style ");
    say_hex((unsigned long)GetWindowLongW(g_rich, GWL_STYLE));
    say(" options ");
    say_hex((unsigned long)rich(EM_GETOPTIONS, 0, 0));
    exsetsel(6, 6);
    type("x");
    replace(W("Y"), TRUE);
    set_text(W("still settable"));
    put_clipboard(W("P"));
    ask("paste ", WM_PASTE, 0, 0, 0);
    show_text();
    exsetsel(0, 5);
    ask("cut ", WM_CUT, 0, 0, 0);
    show_text();
    ask("setreadonly FALSE ", EM_SETREADONLY, FALSE, 0, 0);
    say(" style ");
    say_hex((unsigned long)GetWindowLongW(g_rich, GWL_STYLE));
}

static void single_line(void) {
    HWND single = CreateWindowExW(0, MSFTEDIT_CLASS, W("one\r\ntwo"), WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 10,
                                  200, 200, 24, g_main, (HMENU)ID_SINGLE, g_instance, NULL);
    HWND keep = g_rich;
    g_rich = single;
    rich(WM_SETFONT, (WPARAM)g_font, TRUE);
    line("single line styles ");
    say_hex((unsigned long)GetWindowLongW(single, GWL_STYLE));
    say(" dlgcode ");
    say_hex((unsigned long)rich(WM_GETDLGCODE, 0, 0));
    say(" options ");
    say_hex((unsigned long)rich(EM_GETOPTIONS, 0, 0));
    show_text();
    show_lines();
    show_rect("single");
    set_text(W("first\rsecond"));
    exsetsel(2, 2);
    replace(W("a\r\nb"), TRUE);
    type("\r");
    show_text();
    show_lines();
    DestroyWindow(single);
    g_rich = keep;
}

/* The Liberation files next to the program, by their 8.3 names, given to
 * Windows XP for this process's session. Where they are already installed
 * (SavanXP and the harness) the call fails and nothing changes. */
static void add_liberation(void) {
    static const char *const k_files[2] = {"LSANS", "LSANSB"};
    WCHAR path[300];
    DWORD n = GetModuleFileNameW(NULL, path, 260);
    while (n > 0 && path[n - 1] != '\\') {
        --n;
    }
    for (unsigned i = 0; i < 2; ++i) {
        DWORD k = n;
        for (const char *c = k_files[i]; *c != 0; ++c) {
            path[k++] = (WCHAR)*c;
        }
        for (const char *c = ".TTF"; *c != 0; ++c) {
            path[k++] = (WCHAR)*c;
        }
        path[k] = 0;
        AddFontResourceW(path);
    }
}

/* Whether a word of the command line, after the program's name, is `word`. */
static int argument(const WCHAR *word) {
    const WCHAR *c = GetCommandLineW();
    int first = 1;
    while (*c != 0) {
        while (*c == ' ') {
            ++c;
        }
        const WCHAR *start = c;
        while (*c != 0 && *c != ' ') {
            ++c;
        }
        if (!first && c > start) {
            int i = 0;
            while (start + i < c && word[i] != 0 && start[i] == word[i]) {
                ++i;
            }
            if (start + i == c && word[i] == 0) {
                return 1;
            }
        }
        first = 0;
    }
    return 0;
}

static void make_main(void) {
    WNDCLASSW wc = {0};
    wc.lpfnWndProc = main_proc;
    wc.hInstance = g_instance;
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = W("RichFixture");
    RegisterClassW(&wc);
    RECT frame = {0, 0, CLIENT_WIDTH, CLIENT_HEIGHT};
    const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    AdjustWindowRect(&frame, style, FALSE);
    g_main = CreateWindowExW(0, W("RichFixture"), W("richedit"), style, 40, 40, frame.right - frame.left,
                             frame.bottom - frame.top, NULL, NULL, g_instance, NULL);
    LOGFONTW lf = {0};
    lf.lfHeight = -13;
    lf.lfWeight = FW_NORMAL;
    lf.lfCharSet = ANSI_CHARSET;
    static const WCHAR k_face[] = L"Liberation Sans";
    for (int i = 0; k_face[i] != 0; ++i) {
        lf.lfFaceName[i] = k_face[i];
    }
    g_font = CreateFontIndirectW(&lf);
}

/* What the input script left: the text, the selection, the view. */
static void input_state(void) {
    line("state");
    show_text();
    show_sel();
    show_lines();
    show_scroll();
    show_state();
    line("  focus ");
    say(GetFocus() == g_rich ? "rich" : GetFocus() == g_main ? "main" : "other");
}

/* With "input": the control driven by input.txt. Its parent writes what it
 * hears; F2, caught by this loop and never seen by the control, writes the
 * state. Nothing printed depends on timing: EN_UPDATE, which follows
 * painting, is not asked for. */
static void run_input(void) {
    say("richedit.exe input: msftedit.dll ");
    say(LoadLibraryW(W("msftedit.dll")) != NULL ? "loaded" : "missing");
    make_main();
    empty_clipboard();
    g_rich = CreateWindowExW(0, MSFTEDIT_CLASS, W(""),
                             WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_AUTOVSCROLL | ES_NOHIDESEL |
                                 ES_SAVESEL,
                             10, 10, 300, 150, g_main, (HMENU)ID_RICH, g_instance, NULL);
    rich(WM_SETFONT, (WPARAM)g_font, TRUE);
    rich(EM_SETEVENTMASK, 0, ENM_CHANGE | ENM_SELCHANGE | ENM_SCROLL);
    ShowWindow(g_main, SW_SHOWNORMAL);
    UpdateWindow(g_main);
    g_log = 1;
    MSG m;
    while (GetMessageW(&m, NULL, 0, 0) > 0) {
        if (m.message == WM_KEYDOWN && m.wParam == VK_F2) {
            input_state();
            continue;
        }
        if (m.message == WM_KEYUP && m.wParam == VK_F2) {
            continue;
        }
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    g_log = 0;
    say("\r\n");
    ExitProcess(0);
}

void start(void) {
    g_instance = GetModuleHandleW(NULL);
    add_liberation();
    if (argument(W("input"))) {
        run_input();
    }
    say("richedit.exe: msftedit.dll ");
    say(LoadLibraryW(W("msftedit.dll")) != NULL ? "loaded" : "missing");
    the_class();
    make_main();
    empty_clipboard();

    g_log = 1;
    line("create");
    g_rich = CreateWindowExW(0, MSFTEDIT_CLASS, W(""),
                             WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_AUTOVSCROLL | ES_NOHIDESEL |
                                 ES_SAVESEL,
                             10, 10, 300, 150, g_main, (HMENU)ID_RICH, g_instance, NULL);
    g_log = 0;
    say(" ");
    say(g_rich != NULL ? "made" : "failed");
    if (g_rich == NULL) {
        ExitProcess(1);
    }
    ShowWindow(g_main, SW_SHOWNORMAL);
    UpdateWindow(g_main);
    pump();
    defaults();

    ask("seteventmask ", EM_SETEVENTMASK, 0,
                                ENM_CHANGE | ENM_UPDATE | ENM_SELCHANGE | ENM_SCROLL, 1);
    say(" now ");
    say_hex((unsigned long)rich(EM_GETEVENTMASK, 0, 0));
    g_log = 1;
    line("setfont");
    rich(WM_SETFONT, (WPARAM)g_font, TRUE);
    say(" font ");
    say_yes(rich(WM_GETFONT, 0, 0) == (LRESULT)g_font);
    pump();

    text_and_lengths();
    pump();
    selection();
    pump();
    replacing();
    pump();
    typing();
    pump();
    limits();
    pump();
    layout();
    pump();
    scrolling();
    pump();
    word_breaks();
    pump();
    clipboard();
    pump();
    read_only();
    pump();
    single_line();
    pump();

    line("destroy");
    DestroyWindow(g_main);
    pump();
    g_log = 0;
    say("\r\n");
    ExitProcess(0);
}
