/*
 * clip.exe -- The WP5 fixture of the clipboard and of file lists
 * (WIN32_WORDPAD_ROADMAP.md, WP5; docs/userland/WIN32.md, "The clipboard").
 *
 * Without arguments it checks what one program sees: the calls refused
 * while the clipboard is closed, setting text, Rich Text Format and a format
 * of its own, the formats Windows makes from text (CF_TEXT, CF_OEMTEXT,
 * CF_LOCALE), their order, the owner, a format promised without data and
 * rendered when asked, WM_DESTROYCLIPBOARD, and a file list (CF_HDROP) read
 * with DragQueryFileW and DragQueryPoint, wide and ANSI, and
 * DragAcceptFiles.
 *
 * "clip.exe put" leaves text, Rich Text Format and a format of its own on
 * the clipboard and ends; "clip.exe get" writes what another program left.
 * On Windows XP the two run one after the other; on SavanXP a SUN program
 * reads what put left and leaves the same for get (WIN32.md, "Tests").
 *
 * "clip.exe drop" shows a window that takes dropped files, with a child
 * that does not, and writes the file list of the first WM_DROPFILES it gets
 * (docs/desktop/DRAG_AND_DROP.md); its input script drags two files over
 * the child and drops them there, so the parent takes the drop. Its client
 * area starts with a 4x4 magenta marker, for the win32-drop scenario.
 *
 * Sequence numbers are written as differences, never as values.
 */
#include <windows.h>

#include "../kernel/print.h"

DECLSPEC_IMPORT HMODULE WINAPI GetModuleHandleW(LPCWSTR name);

static HINSTANCE g_instance;
static HWND g_window;
static HWND g_second;
static UINT g_rtf;
static UINT g_private;
static int g_rendered;
static int g_destroyed;
static int g_marker; /* "drop": the 4x4 magenta marker the host finds */

static const WCHAR k_text[] = {'H', 'o', 'l', 'a', '\r', '\n', 'm', 'u', 'n', 'd', 'o', ' ', 0x00f1, 0x00e9, 0};
static const char k_rtf_data[] = "{\\rtf1\\ansi Hola {\\b mundo}}";
static const unsigned char k_private_data[4] = {1, 2, 3, 0xfe};

/* A drop's file list, wide, and where it landed. */
static void dropped(HWND hwnd, HDROP drop) {
    line("dropped on ");
    say(hwnd == g_window ? "the window" : "another");
    say(" count ");
    const UINT count = DragQueryFileW(drop, 0xffffffffu, NULL, 0);
    say_int((long)count);
    for (UINT i = 0; i < count; ++i) {
        WCHAR name[MAX_PATH];
        name[0] = 0;
        line("file ");
        say_int((long)DragQueryFileW(drop, i, name, MAX_PATH));
        say(" ");
        say_w(name);
    }
    POINT pt = {0, 0};
    line("point ");
    say_int(DragQueryPoint(drop, &pt));
    say(" ");
    say_int(pt.x);
    say(",");
    say_int(pt.y);
    DragFinish(drop);
    PostQuitMessage(0);
}

static LRESULT CALLBACK proc(HWND hwnd, UINT m, WPARAM wParam, LPARAM lParam) {
    switch (m) {
    case WM_RENDERFORMAT:
        if (wParam == CF_UNICODETEXT) {
            HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, sizeof(k_text));
            WCHAR *p = GlobalLock(h);
            for (unsigned i = 0; i < sizeof(k_text) / sizeof(WCHAR); ++i) {
                p[i] = k_text[i];
            }
            GlobalUnlock(h);
            SetClipboardData(CF_UNICODETEXT, h);
            g_rendered = 1;
        }
        return 0;
    case WM_RENDERALLFORMATS:
        return 0;
    case WM_PAINT:
        if (g_marker) {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(hwnd, &ps);
            RECT marker = {0, 0, 4, 4};
            HBRUSH magenta = CreateSolidBrush(RGB(255, 0, 255));
            FillRect(dc, &marker, magenta);
            DeleteObject(magenta);
            EndPaint(hwnd, &ps);
            return 0;
        }
        return DefWindowProcW(hwnd, m, wParam, lParam);
    case WM_DROPFILES:
        dropped(hwnd, (HDROP)wParam);
        return 0;
    case WM_TIMER:
        line("no drop");
        PostQuitMessage(0);
        return 0;
    case WM_DESTROYCLIPBOARD:
        if (hwnd == g_window) {
            ++g_destroyed;
        }
        return 0;
    default:
        return DefWindowProcW(hwnd, m, wParam, lParam);
    }
}

static HGLOBAL global_of(const void *bytes, unsigned size) {
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE | GMEM_DDESHARE, size);
    unsigned char *p = GlobalLock(h);
    for (unsigned i = 0; i < size; ++i) {
        p[i] = ((const unsigned char *)bytes)[i];
    }
    GlobalUnlock(h);
    return h;
}

static void say_format(UINT f) {
    WCHAR name[64];
    if (f >= 0xc000u && GetClipboardFormatNameW(f, name, 64) > 0) {
        say_w(name);
    } else {
        say_int((long)f);
    }
}

static void say_bytes(HANDLE h) {
    const unsigned char *p = GlobalLock(h);
    const SIZE_T size = GlobalSize(h);
    say("size ");
    say_int((long)size);
    say(":");
    for (SIZE_T i = 0; i < size && i < 64; ++i) {
        say(" ");
        say_int(p[i]);
    }
    GlobalUnlock(h);
}

static void say_enum(void) {
    line("formats");
    for (UINT f = EnumClipboardFormats(0); f != 0; f = EnumClipboardFormats(f)) {
        say(" ");
        say_format(f);
    }
    say(" count ");
    say_int(CountClipboardFormats());
}

static void say_available(void) {
    static const UINT k_formats[] = {CF_TEXT, CF_OEMTEXT, CF_UNICODETEXT, CF_LOCALE, CF_HDROP, CF_BITMAP, CF_DIB};
    line("available");
    for (unsigned i = 0; i < sizeof(k_formats) / sizeof(k_formats[0]); ++i) {
        say(" ");
        say_int((long)k_formats[i]);
        say(IsClipboardFormatAvailable(k_formats[i]) ? "+" : "-");
    }
    say(" rtf");
    say(IsClipboardFormatAvailable(g_rtf) ? "+" : "-");
    say(" private");
    say(IsClipboardFormatAvailable(g_private) ? "+" : "-");
}

/* What another program left: every format, the text as UTF-16 units. */
static void get(void) {
    say_available();
    line("open");
    say_result(OpenClipboard(NULL));
    say_enum();
    HANDLE h = GetClipboardData(CF_UNICODETEXT);
    line("unicode ");
    if (h != NULL) {
        const WCHAR *w = GlobalLock(h);
        const SIZE_T units = GlobalSize(h) / 2;
        for (SIZE_T i = 0; i < units && w[i] != 0; ++i) {
            say(" ");
            say_int(w[i]);
        }
        GlobalUnlock(h);
    } else {
        say("none");
    }
    h = GetClipboardData(CF_TEXT);
    line("text ");
    if (h != NULL) {
        const unsigned char *p = GlobalLock(h);
        for (unsigned i = 0; p[i] != 0; ++i) {
            say(" ");
            say_int(p[i]);
        }
        GlobalUnlock(h);
    } else {
        say("none");
    }
    h = GetClipboardData(g_rtf);
    line("rtf ");
    if (h != NULL) {
        const char *p = GlobalLock(h);
        char copy[128];
        int n = 0;
        while (p[n] != 0 && n < 127) {
            copy[n] = p[n];
            ++n;
        }
        copy[n] = 0;
        GlobalUnlock(h);
        say(copy);
    } else {
        say("none");
    }
    h = GetClipboardData(g_private);
    line("private ");
    if (h != NULL) {
        say_bytes(h);
    } else {
        say("none");
    }
    line("owner ");
    say_yes(GetClipboardOwner() != NULL);
    line("close");
    say_result(CloseClipboard());
}

static void put(void) {
    line("put");
    say_result(OpenClipboard(g_window));
    say_result(EmptyClipboard());
    SetClipboardData(CF_UNICODETEXT, global_of(k_text, sizeof(k_text)));
    SetClipboardData(g_rtf, global_of(k_rtf_data, sizeof(k_rtf_data)));
    SetClipboardData(g_private, global_of(k_private_data, sizeof(k_private_data)));
    say_result(CloseClipboard());
}

static HGLOBAL make_drop(int wide, int nonclient) {
    static const char *const k_paths[] = {"C:\\a.txt", "C:\\folder\\b c.txt"};
    unsigned size = sizeof(DROPFILES);
    for (int i = 0; i < 2; ++i) {
        size += (unsigned)(lstrlenA(k_paths[i]) + 1) * (wide ? 2u : 1u);
    }
    size += wide ? 2u : 1u;
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, size);
    unsigned char *p = GlobalLock(h);
    DROPFILES *d = (DROPFILES *)p;
    d->pFiles = sizeof(DROPFILES);
    d->pt.x = 12;
    d->pt.y = 34;
    d->fNC = nonclient;
    d->fWide = wide;
    unsigned at = sizeof(DROPFILES);
    for (int i = 0; i < 2; ++i) {
        for (const char *c = k_paths[i];; ++c) {
            if (wide) {
                p[at++] = (unsigned char)*c;
                p[at++] = 0;
            } else {
                p[at++] = (unsigned char)*c;
            }
            if (*c == 0) {
                break;
            }
        }
    }
    GlobalUnlock(h);
    return h;
}

static void drops(void) {
    for (int wide = 1; wide >= 0; --wide) {
        HDROP drop = make_drop(wide, wide == 0);
        line(wide ? "wide drop count " : "ansi drop count ");
        say_int((long)DragQueryFileW(drop, 0xffffffffu, NULL, 0));
        for (UINT i = 0; i < 3; ++i) {
            WCHAR name[MAX_PATH];
            say(" | ");
            say_int((long)DragQueryFileW(drop, i, NULL, 0));
            say(" ");
            name[0] = 0;
            say_int((long)DragQueryFileW(drop, i, name, MAX_PATH));
            say(" ");
            say_w(name);
        }
        WCHAR cut[5];
        line("cut ");
        say_int((long)DragQueryFileW(drop, 1, cut, 5));
        say(" ");
        say_w(cut);
        POINT pt = {0, 0};
        line("point ");
        say_int(DragQueryPoint(drop, &pt));
        say(" ");
        say_int(pt.x);
        say(",");
        say_int(pt.y);
        DragFinish(drop);
    }
    line("accept files");
    DragAcceptFiles(g_window, TRUE);
    say(" ");
    say_hex((DWORD)GetWindowLongW(g_window, GWL_EXSTYLE) & WS_EX_ACCEPTFILES);
    DragAcceptFiles(g_window, FALSE);
    say(" ");
    say_hex((DWORD)GetWindowLongW(g_window, GWL_EXSTYLE) & WS_EX_ACCEPTFILES);

    /* A file list through the clipboard, by this program: its own handle. */
    OpenClipboard(g_window);
    EmptyClipboard();
    HGLOBAL drop = make_drop(1, 0);
    SetClipboardData(CF_HDROP, drop);
    CloseClipboard();
    line("hdrop available ");
    say_yes(IsClipboardFormatAvailable(CF_HDROP));
    OpenClipboard(g_window);
    HDROP back = GetClipboardData(CF_HDROP);
    line("hdrop back ");
    say_yes(back == drop);
    say(" count ");
    say_int((long)DragQueryFileW(back, 0xffffffffu, NULL, 0));
    CloseClipboard();
}

/* Waits for one drop, or ten seconds. */
static void drop_target(void) {
    g_marker = 1;
    /* The window manager owns a top-level window's size: made anew at it. */
    DestroyWindow(g_window);
    g_window = CreateWindowExW(0, L"Clip", L"clip", WS_OVERLAPPEDWINDOW, 0, 0, 240, 160, NULL, NULL, g_instance,
                               NULL);
    CreateWindowExW(0, L"STATIC", L"child", WS_CHILD | WS_VISIBLE, 120, 60, 100, 50, g_window, NULL, g_instance, NULL);
    DragAcceptFiles(g_window, TRUE);
    ShowWindow(g_window, SW_SHOW);
    UpdateWindow(g_window);
    line("ready");
    SetTimer(g_window, 1, 10000, NULL);
    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    KillTimer(g_window, 1);
}

static void self(void) {
    line("rtf registered ");
    say_yes(g_rtf >= 0xc000u);
    say(" again ");
    say_yes(RegisterClipboardFormatW(L"rich text format") == g_rtf);
    say(" is a message ");
    say_yes(RegisterWindowMessageW(L"Rich Text Format") == g_rtf);
    WCHAR name[64];
    line("name ");
    say_int(GetClipboardFormatNameW(g_rtf, name, 64));
    say(" ");
    say_w(name);
    line("name cut ");
    say_int(GetClipboardFormatNameW(g_rtf, name, 5));
    say(" ");
    say_w(name);
    line("name of CF_TEXT ");
    SetLastError(0);
    say_int(GetClipboardFormatNameW(CF_TEXT, name, 64));
    say_error();

    line("closed: set ");
    SetLastError(0);
    say_yes(SetClipboardData(CF_UNICODETEXT, global_of(k_text, sizeof(k_text))) != NULL);
    say_error();
    line("closed: get ");
    SetLastError(0);
    say_yes(GetClipboardData(CF_UNICODETEXT) != NULL);
    say_error();
    line("closed: empty ");
    SetLastError(0);
    say_result(EmptyClipboard());
    line("closed: close ");
    SetLastError(0);
    say_result(CloseClipboard());
    line("closed: enum ");
    SetLastError(0);
    say_int((long)EnumClipboardFormats(0));
    say_error();

    line("open");
    say_result(OpenClipboard(g_window));
    say(" again ");
    say_result(OpenClipboard(g_window));
    say(" another window ");
    SetLastError(0);
    say_result(OpenClipboard(g_second));
    say(" open window ");
    say_yes(GetOpenClipboardWindow() == g_window);
    DWORD before = GetClipboardSequenceNumber();
    say_result(EmptyClipboard());
    line("sequence after empty +");
    say_int((long)(GetClipboardSequenceNumber() - before));
    say(" owner ");
    say_yes(GetClipboardOwner() == g_window);
    HGLOBAL text = global_of(k_text, sizeof(k_text));
    line("set text ");
    say_yes(SetClipboardData(CF_UNICODETEXT, text) == text);
    say(" +");
    say_int((long)(GetClipboardSequenceNumber() - before));
    SetClipboardData(g_rtf, global_of(k_rtf_data, sizeof(k_rtf_data)));
    SetClipboardData(g_private, global_of(k_private_data, sizeof(k_private_data)));
    say(" all set +");
    say_int((long)(GetClipboardSequenceNumber() - before));
    line("get while open is the same ");
    say_yes(GetClipboardData(CF_UNICODETEXT) == text);
    say_enum();
    line("close");
    say_result(CloseClipboard());
    say(" +");
    say_int((long)(GetClipboardSequenceNumber() - before));
    say(" open window ");
    say_yes(GetOpenClipboardWindow() != NULL);
    say(" owner ");
    say_yes(GetClipboardOwner() == g_window);
    say_available();

    OpenClipboard(NULL);
    say_enum();
    line("unicode ");
    say_bytes(GetClipboardData(CF_UNICODETEXT));
    line("text ");
    say_bytes(GetClipboardData(CF_TEXT));
    line("oem ");
    say_bytes(GetClipboardData(CF_OEMTEXT));
    line("locale ");
    say_bytes(GetClipboardData(CF_LOCALE));
    line("rtf ");
    say_bytes(GetClipboardData(g_rtf));
    line("private ");
    say_bytes(GetClipboardData(g_private));
    line("bitmap ");
    say_yes(GetClipboardData(CF_BITMAP) != NULL);
    say_enum();
    CloseClipboard();

    /* Text set as CF_TEXT: the other two are made from it. */
    OpenClipboard(g_window);
    EmptyClipboard();
    static const char k_ansi[] = "caf\xe9\r\nx";
    SetClipboardData(CF_TEXT, global_of(k_ansi, sizeof(k_ansi)));
    CloseClipboard();
    OpenClipboard(g_window);
    say_enum();
    line("from text: unicode ");
    say_bytes(GetClipboardData(CF_UNICODETEXT));
    line("from text: oem ");
    say_bytes(GetClipboardData(CF_OEMTEXT));
    CloseClipboard();

    /* A format promised without data, rendered when it is asked for. */
    OpenClipboard(g_window);
    EmptyClipboard();
    SetClipboardData(CF_UNICODETEXT, NULL);
    CloseClipboard();
    line("promised available ");
    say_yes(IsClipboardFormatAvailable(CF_UNICODETEXT));
    OpenClipboard(g_window);
    HANDLE rendered = GetClipboardData(CF_UNICODETEXT);
    line("promised ");
    say_yes(rendered != NULL);
    say(" rendered ");
    say_yes(g_rendered);
    if (rendered != NULL) {
        say(" ");
        say_bytes(rendered);
    }
    CloseClipboard();

    /* Another window empties it: the owner hears it. */
    g_destroyed = 0;
    OpenClipboard(g_second);
    EmptyClipboard();
    line("destroy clipboard heard ");
    say_int(g_destroyed);
    say(" owner is the second ");
    say_yes(GetClipboardOwner() == g_second);
    CloseClipboard();

    drops();

    OpenClipboard(g_window);
    EmptyClipboard();
    CloseClipboard();
    line("emptied ");
    say_int(CountClipboardFormats());
}

void start(void) {
    g_instance = GetModuleHandleW(NULL);
    say("clip.exe");
    WNDCLASSW wc;
    for (unsigned i = 0; i < sizeof(wc); ++i) {
        ((char *)&wc)[i] = 0;
    }
    wc.lpfnWndProc = proc;
    wc.hInstance = g_instance;
    wc.lpszClassName = L"Clip";
    RegisterClassW(&wc);
    g_window = CreateWindowExW(0, L"Clip", L"clip", WS_OVERLAPPEDWINDOW, 0, 0, 100, 100, NULL, NULL, g_instance,
                               NULL);
    g_second = CreateWindowExW(0, L"Clip", L"second", WS_OVERLAPPEDWINDOW, 0, 0, 100, 100, NULL, NULL, g_instance,
                               NULL);
    g_rtf = RegisterClipboardFormatW(L"Rich Text Format");
    g_private = RegisterClipboardFormatW(L"SavanXP Test");
    const WCHAR *command = GetCommandLineW();
    int mode = 0;
    for (const WCHAR *c = command; *c != 0; ++c) {
        if (c[0] == ' ' && c[1] == 'p' && c[2] == 'u' && c[3] == 't') {
            mode = 1;
        } else if (c[0] == ' ' && c[1] == 'g' && c[2] == 'e' && c[3] == 't') {
            mode = 2;
        } else if (c[0] == ' ' && c[1] == 'd' && c[2] == 'r' && c[3] == 'o' && c[4] == 'p') {
            mode = 3;
        }
    }
    if (mode == 1) {
        put();
    } else if (mode == 2) {
        get();
    } else if (mode == 3) {
        drop_target();
    } else {
        self();
    }
    DestroyWindow(g_second);
    DestroyWindow(g_window);
    say("\r\n");
    ExitProcess(0);
}
