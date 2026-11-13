/*
 * cards.exe -- The W4 fixture: it loads the user's own cards.dll (Windows
 * XP's card library, never part of this repository) from its own folder and
 * draws every card the way Solitaire does, through cdtInit, cdtDrawExt and
 * cdtTerm, into a memory device context. For each drawing it writes the
 * CRC-32 of the pixels it reads back with GetPixel; with the argument
 * "pixels" it writes the pixels themselves, as runs of colour.
 *
 * What is drawn: the 52 faces (mode 0) and the 12 backs (mode 1) at 71x96 on
 * Solitaire's green, the other modes of cdtDrawExt on a few cards, and the
 * backs at 33x62 on the dialog colour, as the Deck dialog draws them (with
 * StretchBlt inside cards.dll). Before each drawing the bitmap is filled
 * with a colour of its own, so what cards.dll leaves alone shows.
 */
#include <windows.h>

typedef BOOL(WINAPI *cdt_init_fn)(int *width, int *height);
typedef BOOL(WINAPI *cdt_draw_ext_fn)(HDC hdc, int x, int y, int dx, int dy, int card, int mode, DWORD color);
typedef void(WINAPI *cdt_term_fn)(void);

static int g_pixels;

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

/* ---- Pixels ------------------------------------------------------------------------- */

static unsigned crc32(unsigned crc, unsigned char byte) {
    crc ^= byte;
    for (int i = 0; i < 8; ++i) {
        crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
    }
    return crc;
}

#define MAX_WIDTH 71
#define MAX_HEIGHT 96

/* The CRC-32 of the drawing's pixels, R, G, B row by row; and, with
 * "pixels", the rows as runs ("=" for a row like the one above). */
static void report(HDC dc, int width, int height) {
    static COLORREF row[MAX_WIDTH];
    static COLORREF above[MAX_WIDTH];
    unsigned crc = 0xffffffffu;
    for (int y = 0; y < height; ++y) {
        int same = y != 0;
        for (int x = 0; x < width; ++x) {
            row[x] = GetPixel(dc, x, y);
            crc = crc32(crc, GetRValue(row[x]));
            crc = crc32(crc, GetGValue(row[x]));
            crc = crc32(crc, GetBValue(row[x]));
            same = same && row[x] == above[x];
        }
        if (!g_pixels) {
            continue;
        }
        say("  ");
        say_int(y);
        if (same) {
            say(" =\n");
            continue;
        }
        for (int x = 0; x < width;) {
            int run = 1;
            while (x + run < width && row[x + run] == row[x]) {
                ++run;
            }
            say(" ");
            say_hex(GetRValue(row[x]) << 16 | GetGValue(row[x]) << 8 | GetBValue(row[x]), 6);
            say("*");
            say_int(run);
            x += run;
        }
        say("\n");
        for (int x = 0; x < width; ++x) {
            above[x] = row[x];
        }
    }
    say(" crc ");
    say_hex(~crc, 8);
    say("\n");
}

static HBRUSH g_fill;

static void draw(cdt_draw_ext_fn draw_ext, HDC dc, int card, int mode, int width, int height, DWORD color) {
    HGDIOBJ old = SelectObject(dc, g_fill);
    PatBlt(dc, 0, 0, MAX_WIDTH, MAX_HEIGHT, PATCOPY);
    SelectObject(dc, old);
    BOOL ok = draw_ext(dc, 0, 0, width, height, card, mode, color);
    say("card ");
    say_int(card);
    say(" mode ");
    say_int(mode);
    say(" ");
    say_int(width);
    say("x");
    say_int(height);
    say(ok ? " ok" : " failed");
    report(dc, width, height);
}

/* ---- Start ---------------------------------------------------------------------------- */

static int same(const WCHAR *a, const char *b) {
    while (*a != 0 && *a == (WCHAR)*b) {
        ++a;
        ++b;
    }
    return *a == 0 && *b == 0;
}

#define GREEN RGB(0, 128, 0)
#define DIALOG RGB(212, 208, 200)

static int run(void) {
    const WCHAR *line = GetCommandLineW();
    int n = lstrlenW(line);
    while (n > 0 && line[n - 1] == ' ') {
        --n;
    }
    g_pixels = n >= 7 && same(line + n - 6, "pixels") && line[n - 7] == ' ';

    HMODULE cards = LoadLibraryA("cards.dll");
    if (cards == NULL) {
        say("no cards.dll\n");
        flush();
        return 1;
    }
    cdt_init_fn init = (cdt_init_fn)GetProcAddress(cards, "cdtInit");
    cdt_draw_ext_fn draw_ext = (cdt_draw_ext_fn)GetProcAddress(cards, "cdtDrawExt");
    cdt_term_fn term = (cdt_term_fn)GetProcAddress(cards, "cdtTerm");
    if (init == NULL || draw_ext == NULL || term == NULL) {
        say("cards.dll lacks cdtInit, cdtDrawExt or cdtTerm\n");
        flush();
        return 1;
    }
    int width = 0;
    int height = 0;
    BOOL ok = init(&width, &height);
    say("cdtInit ");
    say(ok ? "ok " : "failed ");
    say_int(width);
    say("x");
    say_int(height);
    say("\n");

    HDC screen = GetDC(NULL);
    HDC dc = CreateCompatibleDC(screen);
    HBITMAP bitmap = CreateCompatibleBitmap(screen, MAX_WIDTH, MAX_HEIGHT);
    ReleaseDC(NULL, screen);
    HGDIOBJ old_bitmap = SelectObject(dc, bitmap);
    g_fill = CreateSolidBrush(RGB(0x12, 0x34, 0x56));

    for (int card = 0; card < 52; ++card) {
        draw(draw_ext, dc, card, 0, 71, 96, GREEN);
    }
    for (int back = 54; back <= 65; ++back) {
        draw(draw_ext, dc, back, 1, 71, 96, GREEN);
    }
    /* Inverted, the empty pile's marks, the removed card, X and O. */
    static const int k_others[][2] = {{0, 2}, {25, 2}, {51, 2}, {1, 3}, {0, 4}, {1, 5}, {0, 6}, {0, 7}};
    for (unsigned i = 0; i < sizeof(k_others) / sizeof(k_others[0]); ++i) {
        draw(draw_ext, dc, k_others[i][0], k_others[i][1], 71, 96, GREEN);
    }
    for (int back = 54; back <= 65; ++back) {
        draw(draw_ext, dc, back, 1, 33, 62, DIALOG);
    }

    term();
    SelectObject(dc, old_bitmap);
    DeleteObject(bitmap);
    DeleteObject(g_fill);
    DeleteDC(dc);
    say("cdtTerm\n");
    flush();
    return 0;
}

void start(void) {
    ExitProcess((unsigned)run());
}
