/*
 * resource.exe -- The W4 fixture for resources and bitmaps, with no file of
 * anyone else's: strings, bitmaps in both header formats at 1, 4 and 24 bits
 * per pixel, an icon, a cursor and an accelerator table of its own
 * (resource.rc, images from images.py), loaded the ways sol.exe loads them.
 * The bitmaps are then copied, converted between monochrome and colour, and
 * stretched both ways, and what lands is written as a CRC-32 of the pixels.
 * Everything it writes is the same on Windows XP.
 */
#include <windows.h>

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

/* UTF-16 as ASCII, anything else as \uXXXX. */
static void say_wide(const WCHAR *text, int length) {
    for (int i = 0; i < length; ++i) {
        if (text[i] >= 0x20 && text[i] < 0x7f) {
            char c[2] = {(char)text[i], 0};
            say(c);
        } else {
            say("\\u");
            say_hex(text[i], 4);
        }
    }
}

static unsigned crc32(unsigned crc, unsigned char byte) {
    crc ^= byte;
    for (int i = 0; i < 8; ++i) {
        crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
    }
    return crc;
}

/* The pixels, row by row, as runs of colour. */
static void say_rows(HDC dc, int width, int height) {
    for (int y = 0; y < height; ++y) {
        say("  ");
        for (int x = 0; x < width;) {
            COLORREF c = GetPixel(dc, x, y);
            int run = 1;
            while (x + run < width && GetPixel(dc, x + run, y) == c) {
                ++run;
            }
            say(" ");
            say_hex(GetRValue(c) << 16 | GetGValue(c) << 8 | GetBValue(c), 6);
            say("*");
            say_int(run);
            x += run;
        }
        say("\n");
    }
}

static void say_crc(HDC dc, int width, int height) {
    unsigned crc = 0xffffffffu;
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            COLORREF c = GetPixel(dc, x, y);
            crc = crc32(crc, GetRValue(c));
            crc = crc32(crc, GetGValue(c));
            crc = crc32(crc, GetBValue(c));
        }
    }
    say(" crc ");
    say_hex(~crc, 8);
    say("\n");
}

/* ---- Strings ------------------------------------------------------------------------ */

static void strings(HINSTANCE instance) {
    static const UINT k_ids[] = {1, 2, 15, 17, 100, 3, 16, 200};
    WCHAR buffer[64];
    for (unsigned i = 0; i < sizeof(k_ids) / sizeof(k_ids[0]); ++i) {
        buffer[0] = 'x';
        int n = LoadStringW(instance, k_ids[i], buffer, 64);
        say("string ");
        say_int((int)k_ids[i]);
        say(" ");
        say_int(n);
        say(" \"");
        say_wide(buffer, n);
        say(buffer[n] == 0 ? "\"\n" : "\" unterminated\n");
    }
    int n = LoadStringW(instance, 2, buffer, 5);
    say("string 2 in 5: ");
    say_int(n);
    say(" \"");
    say_wide(buffer, n);
    say("\"\n");
    const WCHAR *pointer = NULL;
    n = LoadStringW(instance, 1, (LPWSTR)&pointer, 0);
    say("string 1 by pointer: ");
    say_int(n);
    say(" \"");
    if (pointer != NULL) {
        say_wide(pointer, n);
    }
    say("\"\n");
    char ansi[64];
    n = LoadStringA(instance, 2, ansi, 64);
    say("string 2 ANSI: ");
    say_int(n);
    for (int k = 0; k <= n; ++k) {
        say(" ");
        say_hex((unsigned char)ansi[k], 2);
    }
    say("\n");
    n = LoadStringA(instance, 17, ansi, 6);
    say("string 17 ANSI in 6: ");
    say_int(n);
    say(" \"");
    say(ansi);
    say("\"\n");
}

/* ---- Bitmaps ------------------------------------------------------------------------- */

#define CANVAS_WIDTH 32
#define CANVAS_HEIGHT 16

static HDC g_canvas;
static HBRUSH g_fill;

static void clear(void) {
    HGDIOBJ old = SelectObject(g_canvas, g_fill);
    PatBlt(g_canvas, 0, 0, CANVAS_WIDTH, CANVAS_HEIGHT, PATCOPY);
    SelectObject(g_canvas, old);
}

static void bitmaps(HINSTANCE instance) {
    static const char *k_names[] = {MAKEINTRESOURCEA(10), MAKEINTRESOURCEA(11), MAKEINTRESOURCEA(12),
                                    MAKEINTRESOURCEA(13), "cards"};
    static const char *k_labels[] = {"10 core 1-bit", "11 info 1-bit red", "12 core 4-bit", "13 info 24-bit",
                                     "\"cards\""};
    HBITMAP loaded[5];
    HDC source = CreateCompatibleDC(g_canvas);
    for (int i = 0; i < 5; ++i) {
        loaded[i] = LoadBitmapA(instance, k_names[i]);
        BITMAP info = {0};
        int got = GetObjectA(loaded[i], sizeof(info), &info);
        say("bitmap ");
        say(k_labels[i]);
        say(": ");
        if (loaded[i] == NULL || got != (int)sizeof(info)) {
            say("not loaded\n");
            continue;
        }
        say_int(info.bmWidth);
        say("x");
        say_int(info.bmHeight);
        say(" ");
        say_int(info.bmBitsPixel);
        say(" bpp, ");
        say_int(info.bmWidthBytes);
        say(" bytes a row");
        /* Copied as it is, a monochrome one taking the canvas's colours. */
        clear();
        HGDIOBJ old = SelectObject(source, loaded[i]);
        BitBlt(g_canvas, 1, 2, info.bmWidth, info.bmHeight, source, 0, 0, SRCCOPY);
        SelectObject(source, old);
        say_crc(g_canvas, CANVAS_WIDTH, CANVAS_HEIGHT);
    }
    say("missing bitmap: ");
    say(LoadBitmapA(instance, MAKEINTRESOURCEA(99)) == NULL ? "NULL\n" : "loaded\n");

    /* Stretched: grown by 2, grown to an odd size, shrunk. */
    static const struct {
        int bitmap;
        int width;
        int height;
        int to_width;
        int to_height;
    } k_stretches[] = {{3, 7, 6, 14, 12}, {2, 9, 5, 20, 11}, {2, 9, 5, 4, 3}, {0, 13, 9, 30, 14}, {0, 13, 9, 6, 4}};
    for (unsigned i = 0; i < sizeof(k_stretches) / sizeof(k_stretches[0]); ++i) {
        clear();
        HGDIOBJ old = SelectObject(source, loaded[k_stretches[i].bitmap]);
        StretchBlt(g_canvas, 0, 0, k_stretches[i].to_width, k_stretches[i].to_height, source, 0, 0,
                   k_stretches[i].width, k_stretches[i].height, SRCCOPY);
        SelectObject(source, old);
        say("stretch ");
        say_int(k_stretches[i].width);
        say("x");
        say_int(k_stretches[i].height);
        say(" to ");
        say_int(k_stretches[i].to_width);
        say("x");
        say_int(k_stretches[i].to_height);
        say_crc(g_canvas, CANVAS_WIDTH, CANVAS_HEIGHT);
        say_rows(g_canvas, k_stretches[i].to_width, k_stretches[i].to_height);
    }

    /* Colour to monochrome: a memory DC's first bitmap is monochrome, and so
     * is the one made compatible with it; the source's background colour
     * becomes white. */
    HDC mono = CreateCompatibleDC(g_canvas);
    HBITMAP mono_bitmap = CreateCompatibleBitmap(mono, 16, 8);
    BITMAP info = {0};
    GetObjectA(mono_bitmap, sizeof(info), &info);
    say("compatible with a memory DC: ");
    say_int(info.bmBitsPixel);
    say(" bpp\n");
    HGDIOBJ old_mono = SelectObject(mono, mono_bitmap);
    PatBlt(mono, 0, 0, 16, 8, WHITENESS);
    HGDIOBJ old = SelectObject(source, loaded[2]);
    SetBkColor(source, RGB(16, 239, 37)); /* entry 1 of core4.bmp's palette */
    BitBlt(mono, 2, 1, 9, 5, source, 0, 0, SRCCOPY);
    SelectObject(source, old);
    say("colour to monochrome");
    say_crc(mono, 16, 8);
    /* Black and white brushes on a monochrome bitmap. (Other colours are
     * dithered there, which nothing in the profile does.) */
    HBRUSH brushes[2] = {CreateSolidBrush(RGB(0, 0, 0)), CreateSolidBrush(RGB(255, 255, 255))};
    for (int i = 0; i < 2; ++i) {
        HGDIOBJ previous = SelectObject(mono, brushes[i]);
        PatBlt(mono, 0, 0, 1, 1, PATCOPY);
        SelectObject(mono, previous);
        say("brush on monochrome: ");
        say_hex(GetPixel(mono, 0, 0), 6);
        say("\n");
        DeleteObject(brushes[i]);
    }
    /* Deleting a selected bitmap succeeds; it goes when it is deselected. */
    say("a selected bitmap deletes: ");
    say(DeleteObject(mono_bitmap) ? "yes\n" : "no\n");
    say("still drawn on: ");
    say_hex(GetPixel(mono, 2, 1), 6);
    say("\n");
    SelectObject(mono, old_mono);
    say("deleted again once deselected: ");
    say(DeleteObject(mono_bitmap) ? "yes\n" : "no\n");
    DeleteDC(mono);

    /* Monochrome to colour takes the destination's text and background
     * colours. */
    clear();
    SetTextColor(g_canvas, RGB(128, 0, 0));
    SetBkColor(g_canvas, RGB(0, 0, 128));
    old = SelectObject(source, loaded[0]);
    BitBlt(g_canvas, 3, 3, 13, 9, source, 0, 0, SRCCOPY);
    BitBlt(g_canvas, 18, 3, 13, 9, source, 0, 0, 0x00440328 /* SRCERASE */);
    SelectObject(source, old);
    say("monochrome to colour");
    say_crc(g_canvas, CANVAS_WIDTH, CANVAS_HEIGHT);
    SetTextColor(g_canvas, RGB(0, 0, 0));
    SetBkColor(g_canvas, RGB(255, 255, 255));

    /* A blit within one bitmap, overlapping, both ways. */
    clear();
    old = SelectObject(source, loaded[3]);
    BitBlt(g_canvas, 0, 0, 7, 6, source, 0, 0, SRCCOPY);
    SelectObject(source, old);
    BitBlt(g_canvas, 3, 2, 7, 6, g_canvas, 0, 0, SRCCOPY);
    BitBlt(g_canvas, 1, 1, 7, 6, g_canvas, 3, 2, SRCCOPY);
    say("overlapping");
    say_crc(g_canvas, CANVAS_WIDTH, CANVAS_HEIGHT);

    for (int i = 0; i < 5; ++i) {
        DeleteObject(loaded[i]);
    }
    DeleteDC(source);
}

/* ---- Icons, cursors and accelerators ----------------------------------------------- */

static int g_commands;

static LRESULT WINAPI proc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_COMMAND) {
        say("  WM_COMMAND ");
        say_int(LOWORD(wParam));
        say(" from ");
        say_int(HIWORD(wParam));
        say("\n");
        ++g_commands;
        return 0;
    }
    if (message == WM_CHAR) {
        say("  WM_CHAR 0x");
        say_hex(wParam, 4);
        say("\n");
        return 0;
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

static void images_and_accelerators(HINSTANCE instance) {
    say("icon 20: ");
    say(LoadIconW(instance, MAKEINTRESOURCEW(20)) != NULL ? "loaded\n" : "NULL\n");
    say("icon 20 at 16x16: ");
    say(LoadImageW(instance, MAKEINTRESOURCEW(20), IMAGE_ICON, 16, 16, 0) != NULL ? "loaded\n" : "NULL\n");
    say("cursor 30: ");
    say(LoadCursorW(instance, MAKEINTRESOURCEW(30)) != NULL ? "loaded\n" : "NULL\n");
    say("bitmap 13 as an image: ");
    HANDLE image = LoadImageW(instance, MAKEINTRESOURCEW(13), IMAGE_BITMAP, 0, 0, 0);
    say(image != NULL ? "loaded\n" : "NULL\n");
    DeleteObject(image);
    say("missing icon: ");
    say(LoadIconW(instance, MAKEINTRESOURCEW(99)) == NULL ? "NULL\n" : "loaded\n");
    say("system icon: ");
    say(LoadIconW(NULL, IDI_APPLICATION) != NULL ? "loaded\n" : "NULL\n");

    HACCEL table = LoadAcceleratorsW(instance, MAKEINTRESOURCEW(40));
    say("accelerators: ");
    say(table != NULL ? "loaded\n" : "NULL\n");
    static const WCHAR k_class[] = {'S', 'x', 'w', 'R', 'e', 's', 0};
    WNDCLASSW wc = {0};
    wc.lpfnWndProc = proc;
    wc.hInstance = instance;
    wc.lpszClassName = k_class;
    RegisterClassW(&wc);
    HWND hwnd = CreateWindowExW(0, k_class, k_class, WS_OVERLAPPEDWINDOW, 0, 0, 100, 100, NULL, NULL, instance, NULL);
    /* Keys as the queue would bring them; their modifiers are up. */
    PostMessageW(hwnd, WM_KEYDOWN, 0x71 /* VK_F2 */, 0x003c0001);
    PostMessageW(hwnd, WM_KEYDOWN, 'Z', 0x002c0001);
    PostMessageW(hwnd, WM_CHAR, 'a', 0x001e0001);
    PostMessageW(hwnd, WM_CHAR, 'b', 0x00300001);
    PostMessageW(hwnd, WM_KEYUP, 0x71, 0xc03c0001);
    PostQuitMessage(0);
    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        if (msg.message == WM_KEYDOWN || msg.message == WM_CHAR || msg.message == WM_KEYUP) {
            say("message 0x");
            say_hex(msg.message, 4);
            say(" 0x");
            say_hex(msg.wParam, 2);
            say("\n");
        }
        if (!TranslateAcceleratorW(hwnd, table, &msg)) {
            DispatchMessageW(&msg);
        }
    }
    say("commands ");
    say_int(g_commands);
    say("\n");
    DestroyWindow(hwnd);
}

static int run(void) {
    HINSTANCE instance = GetModuleHandleA(NULL);
    strings(instance);
    HDC screen = GetDC(NULL);
    g_canvas = CreateCompatibleDC(screen);
    HBITMAP canvas = CreateCompatibleBitmap(screen, CANVAS_WIDTH, CANVAS_HEIGHT);
    ReleaseDC(NULL, screen);
    HGDIOBJ old = SelectObject(g_canvas, canvas);
    g_fill = CreateSolidBrush(RGB(0x12, 0x34, 0x56));
    bitmaps(instance);
    SelectObject(g_canvas, old);
    DeleteObject(canvas);
    DeleteDC(g_canvas);
    DeleteObject(g_fill);
    images_and_accelerators(instance);
    flush();
    return 0;
}

void start(void) {
    ExitProcess((unsigned)run());
}
