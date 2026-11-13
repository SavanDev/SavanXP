/*
 * window.exe -- The W3 fixture: a top-level window with a child, painted
 * with the drawing calls of the W0 matrix, driven by an input script
 * (input.txt next to this file), closed from its frame.
 *
 * It writes the messages its two window procedures receive, one per line,
 * and when the key P arrives, the pixels of its client area outside the text
 * box as runs of colour. Everything it writes is the same on Windows XP: no
 * handle, time or screen position, and no message whose presence depends on
 * the window manager rather than on the program (the list is in
 * logged()). With the argument "all" it writes every message instead, which
 * is how that list was chosen.
 *
 * Its client area is 320x240. The pixel at (0, 0) to (3, 3) is magenta, the
 * marker the input drivers find the client area by on a screenshot.
 */
#include <windows.h>

#define CLIENT_WIDTH 320
#define CLIENT_HEIGHT 240
/* Text is drawn here and left out of the pixel runs: its glyphs are the
 * platform's, not Windows XP's (docs/userland/WIN32.md, "Text"). */
#define TEXT_LEFT 0
#define TEXT_TOP 170
#define TEXT_RIGHT 200
#define TEXT_BOTTOM 240

static int g_all;
static HWND g_main;
static HWND g_child;
static HBRUSH g_green;
static HBRUSH g_magenta;
static HBRUSH g_gray;
static HBRUSH g_blue;

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

/* ---- The message log --------------------------------------------------------------- */

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
    {WM_PAINT, "WM_PAINT"},
    {WM_CLOSE, "WM_CLOSE"},
    {WM_ERASEBKGND, "WM_ERASEBKGND"},
    {WM_SHOWWINDOW, "WM_SHOWWINDOW"},
    {WM_ACTIVATEAPP, "WM_ACTIVATEAPP"},
    {WM_SETCURSOR, "WM_SETCURSOR"},
    {WM_MOUSEACTIVATE, "WM_MOUSEACTIVATE"},
    {WM_GETMINMAXINFO, "WM_GETMINMAXINFO"},
    {WM_WINDOWPOSCHANGING, "WM_WINDOWPOSCHANGING"},
    {WM_WINDOWPOSCHANGED, "WM_WINDOWPOSCHANGED"},
    {WM_GETICON, "WM_GETICON"},
    {WM_NCCREATE, "WM_NCCREATE"},
    {WM_NCDESTROY, "WM_NCDESTROY"},
    {WM_NCCALCSIZE, "WM_NCCALCSIZE"},
    {WM_NCHITTEST, "WM_NCHITTEST"},
    {WM_NCPAINT, "WM_NCPAINT"},
    {WM_NCACTIVATE, "WM_NCACTIVATE"},
    {WM_KEYDOWN, "WM_KEYDOWN"},
    {WM_KEYUP, "WM_KEYUP"},
    {WM_CHAR, "WM_CHAR"},
    {WM_SYSKEYDOWN, "WM_SYSKEYDOWN"},
    {WM_SYSKEYUP, "WM_SYSKEYUP"},
    {WM_SYSCOMMAND, "WM_SYSCOMMAND"},
    {WM_TIMER, "WM_TIMER"},
    {WM_MOUSEMOVE, "WM_MOUSEMOVE"},
    {WM_LBUTTONDOWN, "WM_LBUTTONDOWN"},
    {WM_LBUTTONUP, "WM_LBUTTONUP"},
    {WM_LBUTTONDBLCLK, "WM_LBUTTONDBLCLK"},
    {WM_RBUTTONDOWN, "WM_RBUTTONDOWN"},
    {WM_RBUTTONUP, "WM_RBUTTONUP"},
    {WM_PARENTNOTIFY, "WM_PARENTNOTIFY"},
    {WM_CAPTURECHANGED, "WM_CAPTURECHANGED"},
};

/* What is written by default: the messages a program acts on and that come
 * from what the program and the user do, with arguments that do not depend
 * on the screen. Left out, with what "all" showed on Windows XP: the hit
 * tests and cursor queries every pointer move brings, pointer moves
 * themselves (how many a drag makes depends on the device), the non-client
 * messages of the frame the window manager draws (WM_NCPAINT, WM_GETICON,
 * WM_NCACTIVATE, the input method's), WM_MOVE (the window's position is the
 * window manager's), and the keys that close the window, which differ by
 * environment while the WM_SYSCOMMAND they produce does not. */
static int logged(UINT message) {
    switch (message) {
    case WM_CREATE:
    case WM_DESTROY:
    case WM_SIZE:
    case WM_ACTIVATE:
    case WM_SETFOCUS:
    case WM_KILLFOCUS:
    case WM_PAINT:
    case WM_CLOSE:
    case WM_ERASEBKGND:
    case WM_SHOWWINDOW:
    case WM_NCCREATE:
    case WM_NCDESTROY:
    case WM_KEYDOWN:
    case WM_KEYUP:
    case WM_CHAR:
    case WM_SYSCOMMAND:
    case WM_TIMER:
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
    case WM_LBUTTONDBLCLK:
    case WM_RBUTTONDOWN:
    case WM_RBUTTONUP:
    case WM_PARENTNOTIFY:
    case WM_CAPTURECHANGED:
        return 1;
    default:
        return 0;
    }
}

/* who is the procedure's name, padded to six columns. */
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
    case WM_SIZE:
        say(" ");
        say_int((int)wParam);
        say(" ");
        say_int(LOWORD(lParam));
        say("x");
        say_int(HIWORD(lParam));
        break;
    case WM_ACTIVATE:
        say(" ");
        say_int(LOWORD(wParam));
        break;
    case WM_SHOWWINDOW:
        say(" ");
        say_int((int)wParam);
        say(" ");
        say_int((int)lParam);
        break;
    case WM_KEYDOWN:
    case WM_KEYUP:
    case WM_SYSKEYDOWN:
    case WM_SYSKEYUP:
        say(" 0x");
        say_hex(wParam, 2);
        break;
    case WM_CHAR:
        say(" 0x");
        say_hex(wParam, 4);
        break;
    case WM_SYSCOMMAND:
        say(" 0x");
        say_hex(wParam & 0xfff0u, 4);
        break;
    case WM_TIMER:
        say(" ");
        say_int((int)wParam);
        break;
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
    case WM_LBUTTONDBLCLK:
    case WM_RBUTTONDOWN:
    case WM_RBUTTONUP:
        say(" ");
        say_int(GET_X_LPARAM(lParam));
        say(",");
        say_int(GET_Y_LPARAM(lParam));
        say(" mk 0x");
        say_hex(wParam, 2);
        break;
    case WM_PARENTNOTIFY:
        say(" 0x");
        say_hex(LOWORD(wParam), 4);
        if (LOWORD(wParam) != WM_CREATE && LOWORD(wParam) != WM_DESTROY) {
            say(" ");
            say_int(GET_X_LPARAM(lParam));
            say(",");
            say_int(GET_Y_LPARAM(lParam));
        }
        break;
    case WM_CAPTURECHANGED:
        say(lParam == 0 ? " to none" : (HWND)lParam == g_main ? " to main" : " to another window");
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

/* ---- Painting ----------------------------------------------------------------------- */

static void fill(HDC dc, HBRUSH brush, int left, int top, int width, int height) {
    HGDIOBJ old = SelectObject(dc, brush);
    PatBlt(dc, left, top, width, height, PATCOPY);
    SelectObject(dc, old);
}

static void paint_main(HDC dc) {
    fill(dc, g_magenta, 0, 0, 4, 4);
    fill(dc, (HBRUSH)GetStockObject(WHITE_BRUSH), 10, 10, 60, 40);
    PatBlt(dc, 80, 10, 30, 30, BLACKNESS);
    RECT frame = {120, 10, 180, 50};
    FrameRect(dc, &frame, (HBRUSH)GetStockObject(BLACK_BRUSH));
    RECT inverted = {130, 20, 170, 40};
    InvertRect(dc, &inverted);
    fill(dc, g_blue, 200, 10, 100, 60);
    PatBlt(dc, 220, 20, 60, 40, DSTINVERT);

    /* Lines in the default pen: every octant, and the end point left out. */
    static const POINT k_path[] = {{10, 60}, {110, 95}, {60, 160}, {10, 60}, {100, 60},
                                   {100, 150}, {190, 60}, {130, 150}, {190, 110}};
    MoveToEx(dc, k_path[0].x, k_path[0].y, NULL);
    for (unsigned i = 1; i < sizeof(k_path) / sizeof(k_path[0]); ++i) {
        LineTo(dc, k_path[i].x, k_path[i].y);
    }
    /* R2_NOT across white, green and black. */
    SetROP2(dc, R2_NOT);
    MoveToEx(dc, 5, 30, NULL);
    LineTo(dc, 115, 22);
    MoveToEx(dc, 300, 80, NULL);
    LineTo(dc, 210, 160);
    SetROP2(dc, R2_COPYPEN);
    for (int i = 0; i < 8; ++i) {
        SetPixel(dc, 220 + 3 * i, 150 + i, RGB(255, 255, 0));
    }

    /* The text box, left out of the pixel runs. */
    fill(dc, (HBRUSH)GetStockObject(WHITE_BRUSH), TEXT_LEFT + 2, TEXT_TOP + 2, 196, 66);
    SetTextColor(dc, RGB(0, 0, 128));
    static const WCHAR k_hello[] = {'H', 'o', 'l', 'a', ',', ' ', 'v', 'e', 'n', 't', 'a', 'n', 'a', 0};
    TextOutW(dc, TEXT_LEFT + 6, TEXT_TOP + 6, k_hello, 13);
    HFONT bold = CreateFontW(-12, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, 0, 0,
                             (const WCHAR[]){'M', 'S', ' ', 'S', 'h', 'e', 'l', 'l', ' ', 'D', 'l', 'g', 0});
    HGDIOBJ old = SelectObject(dc, bold);
    SetTextColor(dc, RGB(128, 0, 0));
    SetBkColor(dc, RGB(255, 255, 192));
    static const WCHAR k_bold[] = {'P', 'u', 'n', 't', 'o', 's', ':', ' ', '5', '2', 0};
    TextOutW(dc, TEXT_LEFT + 6, TEXT_TOP + 26, k_bold, 10);
    RECT box = {TEXT_LEFT + 6, TEXT_TOP + 44, TEXT_LEFT + 190, TEXT_TOP + 62};
    static const WCHAR k_centered[] = {'C', 'e', 'n', 't', 'r', 'o', 0};
    DrawTextW(dc, k_centered, -1, &box, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    SelectObject(dc, old);
    DeleteObject(bold);
}

static void paint_child(HDC dc) {
    RECT r = {0, 0, 100, 40};
    FrameRect(dc, &r, (HBRUSH)GetStockObject(BLACK_BRUSH));
    fill(dc, g_blue, 10, 10, 20, 20);
}

/* The client area outside the text box, row by row as runs "RRGGBB*count";
 * a row like the one above it is written "=". */
static void write_pixels(HWND hwnd) {
    HDC dc = GetDC(hwnd);
    static COLORREF row[CLIENT_WIDTH];
    static COLORREF above[CLIENT_WIDTH];
    say("pixels\n");
    for (int y = 0; y < CLIENT_HEIGHT; ++y) {
        int same = y != 0;
        for (int x = 0; x < CLIENT_WIDTH; ++x) {
            int text = x >= TEXT_LEFT && x < TEXT_RIGHT && y >= TEXT_TOP && y < TEXT_BOTTOM;
            row[x] = text ? 0xff000000u : GetPixel(dc, x, y);
            same = same && row[x] == above[x];
        }
        say_int(y);
        if (same) {
            say(" =\n");
            continue;
        }
        for (int x = 0; x < CLIENT_WIDTH;) {
            int run = 1;
            while (x + run < CLIENT_WIDTH && row[x + run] == row[x]) {
                ++run;
            }
            say(" ");
            if (row[x] == 0xff000000u) {
                say("text");
            } else {
                say_hex(GetRValue(row[x]) << 16 | GetGValue(row[x]) << 8 | GetBValue(row[x]), 6);
            }
            say("*");
            say_int(run);
            x += run;
        }
        say("\n");
        for (int x = 0; x < CLIENT_WIDTH; ++x) {
            above[x] = row[x];
        }
    }
    ReleaseDC(hwnd, dc);
}

/* ---- Window procedures -------------------------------------------------------------- */

static void WINAPI first_timer(HWND hwnd, UINT message, UINT_PTR id, DWORD time) {
    (void)time;
    say("main  timer procedure ");
    say_int((int)id);
    say(message == WM_TIMER ? " WM_TIMER\n" : " ?\n");
    KillTimer(hwnd, id);
    SetTimer(hwnd, 7, 30, NULL);
}

static LRESULT WINAPI main_proc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    log_message("main  ", message, wParam, lParam);
    switch (message) {
    case WM_CREATE:
        SetTimer(hwnd, 666, 30, first_timer);
        return 0;
    case WM_TIMER:
        KillTimer(hwnd, wParam);
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        say("main  paint ");
        say_int(ps.rcPaint.left);
        say(",");
        say_int(ps.rcPaint.top);
        say(" ");
        say_int(ps.rcPaint.right);
        say(",");
        say_int(ps.rcPaint.bottom);
        say(ps.fErase ? " erase\n" : "\n");
        paint_main(dc);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_LBUTTONDOWN:
        SetCapture(hwnd);
        say("main  capture ");
        say(GetCapture() == hwnd ? "main" : "other");
        say(" left ");
        say((GetKeyState(VK_LBUTTON) & 0x8000) != 0 ? "down\n" : "up\n");
        return 0;
    case WM_LBUTTONUP:
        ReleaseCapture();
        return 0;
    case WM_KEYDOWN:
        if (wParam == 'R') {
            RECT r = {100, 50, 140, 90};
            InvalidateRect(hwnd, &r, TRUE);
            UpdateWindow(hwnd);
        } else if (wParam == 'P') {
            flush();
            write_pixels(hwnd);
        }
        say("main  shift ");
        say((GetKeyState(VK_SHIFT) & 0x8000) != 0 ? "down\n" : "up\n");
        return 0;
    case WM_DESTROY:
        PostQuitMessage(3);
        return 0;
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

static LRESULT WINAPI child_proc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    log_message("child ", message, wParam, lParam);
    if (message == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        paint_child(dc);
        EndPaint(hwnd, &ps);
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

    HINSTANCE instance = GetModuleHandleA(NULL);
    g_green = CreateSolidBrush(RGB(0, 128, 0));
    g_magenta = CreateSolidBrush(RGB(255, 0, 255));
    g_gray = CreateSolidBrush(RGB(192, 192, 192));
    g_blue = CreateSolidBrush(RGB(0, 0, 255));

    static const WCHAR k_main_class[] = {'S', 'x', 'w', 'M', 'a', 'i', 'n', 0};
    static const WCHAR k_child_class[] = {'S', 'x', 'w', 'C', 'h', 'i', 'l', 'd', 0};
    static const WCHAR k_title[] = {'W', 'i', 'n', 'd', 'o', 'w', ' ', 't', 'e', 's', 't', 0};
    WNDCLASSEXW wc = {0};
    wc.cbSize = sizeof(wc);
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = main_proc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.hbrBackground = g_green;
    wc.lpszClassName = k_main_class;
    if (RegisterClassExW(&wc) == 0) {
        say("RegisterClassExW failed\n");
        return 1;
    }
    WNDCLASSW child = {0};
    child.lpfnWndProc = child_proc;
    child.hInstance = instance;
    child.hCursor = LoadCursorW(NULL, IDC_ARROW);
    child.hbrBackground = g_gray;
    child.lpszClassName = k_child_class;
    if (RegisterClassW(&child) == 0) {
        say("RegisterClassW failed\n");
        return 1;
    }
    RECT r = {0, 0, CLIENT_WIDTH, CLIENT_HEIGHT};
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    g_main = CreateWindowExW(0, k_main_class, k_title, WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, 100, 100,
                             r.right - r.left, r.bottom - r.top, NULL, NULL, instance, NULL);
    if (g_main == NULL) {
        say("CreateWindowExW failed\n");
        return 1;
    }
    say("created\n");
    g_child = CreateWindowExW(0, k_child_class, NULL, WS_CHILD | WS_VISIBLE, 210, 180, 100, 40, g_main, NULL,
                              instance, NULL);
    say(g_child != NULL ? "child created\n" : "child failed\n");
    RECT client;
    GetClientRect(g_main, &client);
    say("client ");
    say_int(client.right);
    say("x");
    say_int(client.bottom);
    say("\n");
    ShowWindow(g_main, SW_SHOWNORMAL);
    say("shown\n");
    UpdateWindow(g_main);
    say("updated\n");

    MSG msg;
    int got;
    while ((got = GetMessageW(&msg, NULL, 0, 0)) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    say("quit ");
    say_int((int)msg.wParam);
    say("\n");
    flush();
    return (int)msg.wParam;
}

void start(void) {
    ExitProcess((unsigned)run());
}
