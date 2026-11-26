/*
 * text.exe -- The WP6 fixture (WIN32_WORDPAD_ROADMAP.md, WP6): text and
 * pictures in gdi32, as WordPad, MFC and the RichEdit control use them.
 *
 * Fonts: Arial, Times New Roman and Courier New in four styles at twelve
 * heights (em heights, cell heights and 0), and faces that do not exist with
 * each family. For each: the face GetTextFaceW names, TEXTMETRICW,
 * GetCharWidthW over ASCII, a sample's extent, OUTLINETEXTMETRICW and the
 * charset. Then weights, underline and strike-out, the ANSI calls over
 * Windows-1252, and EnumFontFamiliesExW. SavanXP draws these faces with
 * metric-compatible ones, so the font lines are compared with a tolerance
 * (tests/host/win32_text_compare.py), not byte for byte.
 *
 * Text drawn: ExtTextOutW's opaque box, clipping, per-character advances,
 * every alignment, the current position, underline and strike-out lines,
 * and a transparent background, on a 320x240 bitmap. What is written is
 * where the ink and the boxes are, not the glyphs' pixels.
 *
 * Pictures: StretchDIBits with 4-, 8- and 24-bit DIBs, top-down and
 * bottom-up, at their size, grown and shrunk; lines under R2_XORPEN;
 * Rectangle with a pen and a brush; a monochrome CreateBitmap blitted with
 * SRCCOPY, SRCAND and SRCINVERT; PatBlt. These have no text: a CRC-32 of each
 * area is compared exactly. With the argument "pixels" the areas' pixels are
 * written too.
 */
#include <windows.h>

#include "../kernel/print.h"

/* A wide literal as the WCHAR the headers take. */
#define W(s) ((const WCHAR *)L##s)

static HDC g_dc;    /* a memory DC holding a bitmap of the screen's format */
static int g_dump;  /* write pixels as well as CRCs */

/* ---- Small helpers -------------------------------------------------------------------- */

static void say_pad(long value) {
    say(" ");
    say_int(value);
}

static void say_face(const WCHAR *face) {
    say_w(face);
}

static void copy_w(WCHAR *to, const WCHAR *from, int capacity) {
    int i = 0;
    for (; from[i] != 0 && i < capacity - 1; ++i) {
        to[i] = from[i];
    }
    to[i] = 0;
}

static int same_w(const WCHAR *a, const WCHAR *b) {
    int i = 0;
    for (; a[i] != 0 && a[i] == b[i]; ++i) {
    }
    return a[i] == b[i];
}

static HFONT make_font(const WCHAR *face, int height, int weight, int italic, int underline, int strike,
                       int pitch_family) {
    LOGFONTW lf;
    unsigned char *p = (unsigned char *)&lf;
    for (unsigned i = 0; i < sizeof lf; ++i) {
        p[i] = 0;
    }
    lf.lfHeight = height;
    lf.lfWeight = weight;
    lf.lfItalic = (BYTE)italic;
    lf.lfUnderline = (BYTE)underline;
    lf.lfStrikeOut = (BYTE)strike;
    lf.lfCharSet = DEFAULT_CHARSET;
    lf.lfPitchAndFamily = (BYTE)pitch_family;
    copy_w(lf.lfFaceName, face, 32);
    return CreateFontIndirectW(&lf);
}

static void say_metrics(const TEXTMETRICW *tm) {
    say_int(tm->tmHeight);
    say_pad(tm->tmAscent);
    say_pad(tm->tmDescent);
    say_pad(tm->tmInternalLeading);
    say_pad(tm->tmExternalLeading);
    say_pad(tm->tmAveCharWidth);
    say_pad(tm->tmMaxCharWidth);
    say_pad(tm->tmWeight);
    say_pad(tm->tmOverhang);
    say_pad(tm->tmDigitizedAspectX);
    say_pad(tm->tmDigitizedAspectY);
    say_pad(tm->tmFirstChar);
    say_pad(tm->tmLastChar);
    say_pad(tm->tmDefaultChar);
    say_pad(tm->tmBreakChar);
    say_pad(tm->tmItalic);
    say_pad(tm->tmUnderlined);
    say_pad(tm->tmStruckOut);
    say_pad(tm->tmPitchAndFamily);
    say_pad(tm->tmCharSet);
}

static const WCHAR *const k_sample = W("The quick brown fox jumps over the lazy dog, 0123456789.");
#define SAMPLE_LENGTH 56

/* Everything about the font selected in g_dc. */
static void describe_font(void) {
    WCHAR face[64];
    TEXTMETRICW tm;
    int n = GetTextFaceW(g_dc, 64, face);
    line("  face");
    say_int(n);
    say(" ");
    say_face(face);
    line("  tm");
    if (GetTextMetricsW(g_dc, &tm)) {
        say_metrics(&tm);
    } else {
        say("failed");
    }
    INT widths[95];
    line("  widths");
    if (GetCharWidthW(g_dc, 0x20, 0x7e, widths)) {
        for (int i = 0; i < 95; ++i) {
            say_pad(widths[i]);
        }
    } else {
        say(" failed");
    }
    SIZE size;
    line("  extent");
    if (GetTextExtentPoint32W(g_dc, k_sample, SAMPLE_LENGTH, &size)) {
        say_pad(size.cx);
        say_pad(size.cy);
    } else {
        say(" failed");
    }
    static union {
        OUTLINETEXTMETRICW otm;
        unsigned char bytes[1024];
    } u;
    UINT need = GetOutlineTextMetricsW(g_dc, 0, NULL);
    line("  otm");
    if (need != 0 && need <= sizeof u && GetOutlineTextMetricsW(g_dc, sizeof u, &u.otm) != 0) {
        const OUTLINETEXTMETRICW *o = &u.otm;
        say_pad(o->otmEMSquare);
        say_pad(o->otmAscent);
        say_pad(o->otmDescent);
        say_pad((long)o->otmLineGap);
        say_pad((long)o->otmsCapEmHeight);
        say_pad((long)o->otmsXHeight);
        say_pad(o->otmItalicAngle);
        say_pad(o->otmsCharSlopeRise);
        say_pad(o->otmsCharSlopeRun);
        say_pad((long)o->otmsUnderscoreSize);
        say_pad(o->otmsUnderscorePosition);
        say_pad((long)o->otmsStrikeoutSize);
        say_pad(o->otmsStrikeoutPosition);
        say_pad((long)o->otmfsSelection);
        say_pad((long)o->otmfsType);
        say_pad(o->otmMacAscent);
        say_pad(o->otmMacDescent);
        say_pad((long)o->otmMacLineGap);
        say_pad((long)o->otmusMinimumPPEM);
        say_pad(o->otmrcFontBox.left);
        say_pad(o->otmrcFontBox.top);
        say_pad(o->otmrcFontBox.right);
        say_pad(o->otmrcFontBox.bottom);
        say(" panose");
        say_pad(o->otmPanoseNumber.bFamilyType);
        say_pad(o->otmPanoseNumber.bSerifStyle);
        say_pad(o->otmPanoseNumber.bWeight);
        say_pad(o->otmPanoseNumber.bProportion);
        say(" names ");
        say_face((const WCHAR *)(u.bytes + (unsigned)o->otmpFamilyName));
        say(" ");
        say_face((const WCHAR *)(u.bytes + (unsigned)o->otmpFaceName));
        say(" ");
        say_face((const WCHAR *)(u.bytes + (unsigned)o->otmpStyleName));
    } else {
        say(" none");
    }
    FONTSIGNATURE sig;
    int charset = GetTextCharsetInfo(g_dc, &sig, 0);
    line("  charset");
    say_pad(charset);
    say(" ");
    say_hex(sig.fsCsb[0]);
    say(" ");
    say_hex(sig.fsCsb[1]);
}

/* ---- Fonts ------------------------------------------------------------------------------ */

typedef struct face_entry {
    const WCHAR *face;
    int pitch_family;
} face_entry;

static const face_entry k_xp_faces[3] = {
    {W("Arial"), VARIABLE_PITCH | FF_SWISS},
    {W("Times New Roman"), VARIABLE_PITCH | FF_ROMAN},
    {W("Courier New"), FIXED_PITCH | FF_MODERN},
};

/* With the argument "liberation": the faces SavanXP ships, which Windows XP
 * is given from the files next to the program (AddFontResourceW). Then
 * Windows XP's answers for them are the very ones gdi32 must give. */
static const face_entry k_liberation_faces[3] = {
    {W("Liberation Sans"), VARIABLE_PITCH | FF_SWISS},
    {W("Liberation Serif"), VARIABLE_PITCH | FF_ROMAN},
    {W("Liberation Mono"), FIXED_PITCH | FF_MODERN},
};

static const face_entry *k_faces = k_xp_faces;
#define FACE_COUNT 3

static const struct {
    const char *name;
    int weight;
    int italic;
} k_styles[] = {
    {"regular", FW_NORMAL, 0},
    {"bold", FW_BOLD, 0},
    {"italic", FW_NORMAL, 1},
    {"bold italic", FW_BOLD, 1},
};

static const int k_heights[] = {-8, -11, -13, -16, -19, -24, -32, -48, -72, 0, 12, 20};

static void fonts(void) {
    for (unsigned f = 0; f < FACE_COUNT; ++f) {
        for (unsigned s = 0; s < sizeof k_styles / sizeof k_styles[0]; ++s) {
            for (unsigned h = 0; h < sizeof k_heights / sizeof k_heights[0]; ++h) {
                HFONT font = make_font(k_faces[f].face, k_heights[h], k_styles[s].weight, k_styles[s].italic, 0, 0,
                                       k_faces[f].pitch_family);
                line("font");
                say_face(k_faces[f].face);
                say(" ");
                say(k_styles[s].name);
                say_pad(k_heights[h]);
                HGDIOBJ old = SelectObject(g_dc, font);
                describe_font();
                SelectObject(g_dc, old);
                DeleteObject(font);
            }
        }
    }

    /* A face that is not installed, with each family: the mapper's choice. */
    static const struct {
        const char *name;
        int pitch_family;
    } k_families[] = {
        {"dontcare", DEFAULT_PITCH | FF_DONTCARE},
        {"swiss", VARIABLE_PITCH | FF_SWISS},
        {"roman", VARIABLE_PITCH | FF_ROMAN},
        {"modern", FIXED_PITCH | FF_MODERN},
        {"fixed", FIXED_PITCH | FF_DONTCARE},
    };
    for (unsigned i = 0; i < sizeof k_families / sizeof k_families[0]; ++i) {
        HFONT font = make_font(W("No Such Face"), -16, FW_NORMAL, 0, 0, 0, k_families[i].pitch_family);
        line("missing");
        say(k_families[i].name);
        HGDIOBJ old = SelectObject(g_dc, font);
        describe_font();
        SelectObject(g_dc, old);
        DeleteObject(font);
    }

    /* Weights: where synthetic bold starts. */
    line("weights");
    for (int weight = 100; weight <= 900; weight += 100) {
        HFONT font = make_font(k_faces[0].face, -16, weight, 0, 0, 0, k_faces[0].pitch_family);
        HGDIOBJ old = SelectObject(g_dc, font);
        TEXTMETRICW tm;
        SIZE size;
        GetTextMetricsW(g_dc, &tm);
        GetTextExtentPoint32W(g_dc, W("Weight"), 6, &size);
        say(" ");
        say_int(weight);
        say(":");
        say_int(tm.tmWeight);
        say("/");
        say_int(size.cx);
        SelectObject(g_dc, old);
        DeleteObject(font);
    }

    /* Underline and strike-out are the font's; GetObjectW keeps them. */
    {
        HFONT font = make_font(k_faces[1].face, -20, FW_NORMAL, 0, 1, 1, k_faces[1].pitch_family);
        HGDIOBJ old = SelectObject(g_dc, font);
        TEXTMETRICW tm;
        GetTextMetricsW(g_dc, &tm);
        LOGFONTW lf;
        GetObjectW(font, sizeof lf, &lf);
        line("underline and strike-out");
        say_pad(tm.tmUnderlined);
        say_pad(tm.tmStruckOut);
        say_pad(lf.lfUnderline);
        say_pad(lf.lfStrikeOut);
        SelectObject(g_dc, old);
        DeleteObject(font);
    }

    /* The ANSI calls read Windows-1252. */
    {
        HFONT font = make_font(k_faces[0].face, -13, FW_NORMAL, 0, 0, 0, k_faces[0].pitch_family);
        HGDIOBJ old = SelectObject(g_dc, font);
        INT widths[32];
        line("ansi widths 0x80-0x9f");
        if (GetCharWidthA(g_dc, 0x80, 0x9f, widths)) {
            for (int i = 0; i < 32; ++i) {
                say_pad(widths[i]);
            }
        }
        INT wide[3];
        GetCharWidthW(g_dc, 0x20ac, 0x20ac, wide);
        GetCharWidthW(g_dc, 0x2014, 0x2014, wide + 1);
        GetCharWidthW(g_dc, 0x0153, 0x0153, wide + 2);
        line("wide euro, em dash, oe");
        say_pad(wide[0]);
        say_pad(wide[1]);
        say_pad(wide[2]);
        SIZE a;
        SIZE w;
        GetTextExtentPoint32A(g_dc, "Ni\xf1o \x80 \x97", 8, &a);
        GetTextExtentPoint32W(g_dc, W("Ni\x00f1o \x20ac \x2014"), 8, &w);
        line("ansi and wide extents");
        say_pad(a.cx);
        say_pad(a.cy);
        say_pad(w.cx);
        say_pad(w.cy);
        SelectObject(g_dc, old);
        DeleteObject(font);
    }
}

/* ---- Enumeration --------------------------------------------------------------------------- */

static int g_family_count;

static int CALLBACK on_family(const LOGFONTW *lf, const TEXTMETRICW *tm, DWORD type, LPARAM data) {
    const ENUMLOGFONTEXW *e = (const ENUMLOGFONTEXW *)lf;
    const NEWTEXTMETRICEXW *n = (const NEWTEXTMETRICEXW *)tm;
    int all = (int)data;
    g_family_count++;
    int known = 0;
    for (unsigned i = 0; i < FACE_COUNT; ++i) {
        known |= same_w(lf->lfFaceName, k_faces[i].face);
    }
    if (!all && !known) {
        return 1;
    }
    line("  family");
    say_face(lf->lfFaceName);
    say(" ");
    say_face(e->elfFullName);
    say(" ");
    say_face(e->elfStyle);
    say(" ");
    say_face(e->elfScript);
    say(" type");
    say_pad((long)type);
    say(" lf");
    say_pad(lf->lfHeight);
    say_pad(lf->lfWidth);
    say_pad(lf->lfWeight);
    say_pad(lf->lfItalic);
    say_pad(lf->lfCharSet);
    say_pad(lf->lfOutPrecision);
    say_pad(lf->lfClipPrecision);
    say_pad(lf->lfQuality);
    say_pad(lf->lfPitchAndFamily);
    say(" ntm");
    say_pad(n->ntmTm.tmHeight);
    say_pad(n->ntmTm.tmAscent);
    say_pad(n->ntmTm.tmAveCharWidth);
    say_pad(n->ntmTm.tmMaxCharWidth);
    say_pad(n->ntmTm.tmWeight);
    say_pad(n->ntmTm.tmPitchAndFamily);
    say_pad(n->ntmTm.tmCharSet);
    say(" ");
    say_hex(n->ntmTm.ntmFlags);
    say_pad((long)n->ntmTm.ntmSizeEM);
    say_pad((long)n->ntmTm.ntmCellHeight);
    say_pad((long)n->ntmTm.ntmAvgWidth);
    say(" ");
    say_hex(n->ntmFontSig.fsCsb[0]);
    return 1;
}

static void enumerate(const char *label, const WCHAR *face, int charset, int all) {
    LOGFONTW lf;
    unsigned char *p = (unsigned char *)&lf;
    for (unsigned i = 0; i < sizeof lf; ++i) {
        p[i] = 0;
    }
    lf.lfCharSet = (BYTE)charset;
    copy_w(lf.lfFaceName, face, 32);
    g_family_count = 0;
    line("enumerate");
    say(label);
    int result = EnumFontFamiliesExW(g_dc, &lf, on_family, all, 0);
    line("  result");
    say_pad(result);
    if (all) {
        say(" callbacks");
        say_pad(g_family_count);
    }
}

/* ---- Text drawn ---------------------------------------------------------------------------- */

#define AREA_W 320
#define AREA_H 240
#define WHITE_PIXEL 0x00ffffffu

static void fill(int left, int top, int right, int bottom, COLORREF color) {
    HBRUSH brush = CreateSolidBrush(color);
    HGDIOBJ old = SelectObject(g_dc, brush);
    PatBlt(g_dc, left, top, right - left, bottom - top, PATCOPY);
    SelectObject(g_dc, old);
    DeleteObject(brush);
}

/* The box of the pixels in [left, right) x [top, bottom) that are not
 * background, and how many there are. */
static void ink(const char *label, int left, int top, int right, int bottom, COLORREF background) {
    int x0 = right;
    int y0 = bottom;
    int x1 = left - 1;
    int y1 = top - 1;
    long count = 0;
    for (int y = top; y < bottom; ++y) {
        for (int x = left; x < right; ++x) {
            if (GetPixel(g_dc, x, y) != background) {
                count++;
                x0 = x < x0 ? x : x0;
                x1 = x > x1 ? x : x1;
                y0 = y < y0 ? y : y0;
                y1 = y > y1 ? y : y1;
            }
        }
    }
    line(label);
    if (count == 0) {
        say("none");
        return;
    }
    say_int(x0);
    say_pad(y0);
    say_pad(x1 + 1);
    say_pad(y1 + 1);
    say(" pixels");
    say_pad(count);
}

/* The distinct red levels in [left, right) x [top, bottom), for black text
 * on white: how many steps of grey the edges are drawn with. */
static void levels(const char *label, int left, int top, int right, int bottom) {
    static unsigned char seen[256];
    for (int i = 0; i < 256; ++i) {
        seen[i] = 0;
    }
    for (int y = top; y < bottom; ++y) {
        for (int x = left; x < right; ++x) {
            seen[GetPixel(g_dc, x, y) & 0xff] = 1;
        }
    }
    line(label);
    for (int i = 0; i < 256; ++i) {
        if (seen[i]) {
            say_pad(i);
        }
    }
}

/* Each row of [left, right) x [top, bottom) whose pixels are all `color`. */
static void full_rows(const char *label, int left, int top, int right, int bottom, COLORREF color) {
    line(label);
    for (int y = top; y < bottom; ++y) {
        int all = 1;
        for (int x = left; x < right && all; ++x) {
            all = GetPixel(g_dc, x, y) == color;
        }
        if (all) {
            say_pad(y);
        }
    }
}

static void text_drawn(void) {
    fill(0, 0, AREA_W, AREA_H, RGB(255, 255, 255));
    HFONT arial = make_font(k_faces[0].face, -16, FW_NORMAL, 0, 0, 0, k_faces[0].pitch_family);
    HGDIOBJ old = SelectObject(g_dc, arial);
    SetTextColor(g_dc, RGB(0, 0, 0));
    SetBkColor(g_dc, RGB(255, 255, 0));

    /* An opaque box larger than the text: the box is the rectangle. */
    RECT box = {10, 8, 150, 34};
    ExtTextOutW(g_dc, 14, 12, ETO_OPAQUE, &box, W("Opaque"), 6, NULL);
    ink("opaque box", 0, 0, AREA_W, 40, RGB(255, 255, 255));
    line("opaque box yellow rows");
    {
        int first = -1;
        int last = -1;
        for (int y = 0; y < 40; ++y) {
            if (GetPixel(g_dc, 10, y) == RGB(255, 255, 0)) {
                first = first < 0 ? y : first;
                last = y;
            }
        }
        say_int(first);
        say_pad(last + 1);
        int left = -1;
        int right = -1;
        for (int x = 0; x < AREA_W; ++x) {
            if (GetPixel(g_dc, x, 9) == RGB(255, 255, 0)) {
                left = left < 0 ? x : left;
                right = x;
            }
        }
        say_pad(left);
        say_pad(right + 1);
    }
    fill(0, 0, AREA_W, AREA_H, RGB(255, 255, 255));

    /* Clipped to a box smaller than the text, and opaque inside it. */
    RECT clip = {10, 40, 60, 58};
    ExtTextOutW(g_dc, 10, 40, ETO_OPAQUE | ETO_CLIPPED, &clip, W("Clipped text"), 12, NULL);
    ink("clipped", 0, 36, AREA_W, 64, RGB(255, 255, 255));

    /* Per-character advances of 12. */
    static const INT k_dx[6] = {12, 12, 12, 12, 12, 12};
    SetBkMode(g_dc, TRANSPARENT);
    ExtTextOutW(g_dc, 10, 70, 0, NULL, W("spaced"), 6, k_dx);
    ink("advances", 0, 66, AREA_W, 92, RGB(255, 255, 255));
    for (int i = 0; i < 6; ++i) {
        ink(i == 0 ? "advances cell 0" : "advances cell", 10 + 12 * i, 66, 10 + 12 * i + 12, 92,
            RGB(255, 255, 255));
    }

    /* Alignments. */
    line("align was");
    say_hex(SetTextAlign(g_dc, TA_BASELINE));
    ExtTextOutW(g_dc, 10, 120, 0, NULL, W("Baseline"), 8, NULL);
    ink("baseline", 0, 96, AREA_W, 130, RGB(255, 255, 255));
    SetTextAlign(g_dc, TA_RIGHT | TA_BOTTOM);
    ExtTextOutW(g_dc, 300, 160, 0, NULL, W("Right"), 5, NULL);
    ink("right bottom", 160, 130, AREA_W, 170, RGB(255, 255, 255));
    SetTextAlign(g_dc, TA_CENTER | TA_TOP);
    ExtTextOutW(g_dc, 80, 140, 0, NULL, W("Centre"), 6, NULL);
    ink("centre", 0, 136, 158, 164, RGB(255, 255, 255));
    line("align now");
    say_hex(GetTextAlign(g_dc));

    /* The current position moves with each call. */
    fill(0, 0, AREA_W, AREA_H, RGB(255, 255, 255));
    SetTextAlign(g_dc, TA_UPDATECP);
    MoveToEx(g_dc, 10, 10, NULL);
    ExtTextOutW(g_dc, 0, 0, 0, NULL, W("One"), 3, NULL);
    POINT p;
    GetCurrentPositionEx(g_dc, &p);
    line("position after one");
    say_int(p.x);
    say_pad(p.y);
    ExtTextOutW(g_dc, 0, 0, 0, NULL, W("Two"), 3, NULL);
    GetCurrentPositionEx(g_dc, &p);
    line("position after two");
    say_int(p.x);
    say_pad(p.y);
    ink("current position", 0, 0, AREA_W, 40, RGB(255, 255, 255));
    SetTextAlign(g_dc, TA_LEFT | TA_TOP);

    /* Underline and strike-out lines across the text. */
    HFONT lines = make_font(k_faces[1].face, -20, FW_NORMAL, 0, 1, 1, k_faces[1].pitch_family);
    SelectObject(g_dc, lines);
    SIZE size;
    GetTextExtentPoint32W(g_dc, W("Lines"), 5, &size);
    ExtTextOutW(g_dc, 20, 60, 0, NULL, W("Lines"), 5, NULL);
    full_rows("underline and strike-out rows", 20, 50, 20 + size.cx, 100, RGB(0, 0, 0));
    ink("lines", 0, 50, AREA_W, 100, RGB(255, 255, 255));
    levels("lines levels", 0, 50, AREA_W, 100);
    SelectObject(g_dc, arial);
    DeleteObject(lines);

    /* Transparent text leaves what is under it. */
    fill(10, 120, 200, 150, RGB(0, 0, 255));
    SetTextColor(g_dc, RGB(255, 255, 255));
    ExtTextOutW(g_dc, 14, 124, 0, NULL, W("Transparent"), 11, NULL);
    ink("transparent box", 0, 110, AREA_W, 160, RGB(255, 255, 255));
    ink("transparent text", 10, 120, 200, 150, RGB(0, 0, 255));
    line("colours");
    say_hex(GetTextColor(g_dc));
    say(" ");
    say_hex(GetBkColor(g_dc));

    /* The ANSI call. */
    SetTextColor(g_dc, RGB(0, 0, 0));
    ExtTextOutA(g_dc, 10, 170, 0, NULL, "Ni\xf1o \x80", 6, NULL);
    ink("ansi", 0, 166, AREA_W, 200, RGB(255, 255, 255));

    SelectObject(g_dc, old);
    DeleteObject(arial);
}

/* ---- Pictures ------------------------------------------------------------------------------ */

static unsigned long crc_table[256];

static unsigned long crc32_pixels(int left, int top, int right, int bottom) {
    if (crc_table[1] == 0) {
        for (unsigned long n = 0; n < 256; ++n) {
            unsigned long c = n;
            for (int k = 0; k < 8; ++k) {
                c = (c & 1u) ? 0xedb88320ul ^ (c >> 1) : c >> 1;
            }
            crc_table[n] = c;
        }
    }
    unsigned long crc = 0xfffffffful;
    for (int y = top; y < bottom; ++y) {
        for (int x = left; x < right; ++x) {
            COLORREF c = GetPixel(g_dc, x, y);
            for (int b = 0; b < 3; ++b) {
                crc = crc_table[(crc ^ (c >> (8 * b))) & 0xffu] ^ (crc >> 8);
            }
        }
    }
    return crc ^ 0xfffffffful;
}

static void area(const char *label, int left, int top, int right, int bottom) {
    line(label);
    say_hex(crc32_pixels(left, top, right, bottom));
    if (!g_dump) {
        return;
    }
    for (int y = top; y < bottom; ++y) {
        line("   ");
        for (int x = left; x < right; ++x) {
            say(" ");
            say_hex(GetPixel(g_dc, x, y));
        }
    }
}

/* A DIB: header, palette, bits, one allocation. */
static struct {
    BITMAPINFOHEADER h;
    RGBQUAD palette[256];
    unsigned char bits[4096];
} g_dib;

static void dib(int width, int height, int bits) {
    unsigned char *p = (unsigned char *)&g_dib;
    for (unsigned i = 0; i < sizeof g_dib; ++i) {
        p[i] = 0;
    }
    g_dib.h.biSize = sizeof g_dib.h;
    g_dib.h.biWidth = width;
    g_dib.h.biHeight = height;
    g_dib.h.biPlanes = 1;
    g_dib.h.biBitCount = (WORD)bits;
    g_dib.h.biCompression = BI_RGB;
    const int colours = bits <= 8 ? 1 << bits : 0;
    for (int i = 0; i < colours; ++i) {
        g_dib.palette[i].rgbRed = (BYTE)(i * 37);
        g_dib.palette[i].rgbGreen = (BYTE)(255 - i * 23);
        g_dib.palette[i].rgbBlue = (BYTE)((i * 91) & 0xff);
    }
    const int rows = height < 0 ? -height : height;
    const int stride = ((width * bits + 31) / 32) * 4;
    unsigned char *b = colours != 0 ? (unsigned char *)&g_dib.palette[colours] : (unsigned char *)g_dib.palette;
    for (int y = 0; y < rows; ++y) {
        for (int x = 0; x < width; ++x) {
            const int v = (x * 3 + y * 5) & 0xff;
            if (bits == 4) {
                const int nibble = (x + y) & 15;
                b[y * stride + x / 2] |= (unsigned char)(x & 1 ? nibble : nibble << 4);
            } else if (bits == 8) {
                b[y * stride + x] = (unsigned char)v;
            } else if (bits == 24) {
                b[y * stride + x * 3] = (unsigned char)(x * 16);
                b[y * stride + x * 3 + 1] = (unsigned char)(y * 16);
                b[y * stride + x * 3 + 2] = (unsigned char)(v);
            }
        }
    }
}

static const void *dib_bits(void) {
    const int colours = g_dib.h.biBitCount <= 8 ? 1 << g_dib.h.biBitCount : 0;
    return colours != 0 ? (const void *)&g_dib.palette[colours] : (const void *)g_dib.palette;
}

static void pictures(void) {
    fill(0, 0, AREA_W, AREA_H, RGB(255, 255, 255));

    dib(16, 8, 4);
    line("stretchdibits 4-bit");
    say_pad(StretchDIBits(g_dc, 0, 0, 16, 8, 0, 0, 16, 8, dib_bits(), (const BITMAPINFO *)&g_dib, DIB_RGB_COLORS,
                          SRCCOPY));
    say_pad(StretchDIBits(g_dc, 20, 0, 32, 16, 0, 0, 16, 8, dib_bits(), (const BITMAPINFO *)&g_dib, DIB_RGB_COLORS,
                          SRCCOPY));
    say_pad(StretchDIBits(g_dc, 60, 0, 8, 4, 0, 0, 16, 8, dib_bits(), (const BITMAPINFO *)&g_dib, DIB_RGB_COLORS,
                          SRCCOPY));
    say_pad(StretchDIBits(g_dc, 72, 0, 6, 6, 4, 2, 6, 6, dib_bits(), (const BITMAPINFO *)&g_dib, DIB_RGB_COLORS,
                          SRCCOPY));
    area("4-bit at size", 0, 0, 16, 8);
    area("4-bit grown", 20, 0, 52, 16);
    area("4-bit shrunk", 60, 0, 68, 4);
    area("4-bit part", 72, 0, 78, 6);

    dib(8, -8, 8);
    line("stretchdibits 8-bit top-down");
    say_pad(StretchDIBits(g_dc, 90, 0, 8, 8, 0, 0, 8, 8, dib_bits(), (const BITMAPINFO *)&g_dib, DIB_RGB_COLORS,
                          SRCCOPY));
    area("8-bit top-down", 90, 0, 98, 8);
    dib(8, 8, 24);
    line("stretchdibits 24-bit");
    say_pad(StretchDIBits(g_dc, 100, 0, 8, 8, 0, 0, 8, 8, dib_bits(), (const BITMAPINFO *)&g_dib, DIB_RGB_COLORS,
                          SRCCOPY));
    say_pad(StretchDIBits(g_dc, 110, 0, 12, 10, 0, 0, 8, 8, dib_bits(), (const BITMAPINFO *)&g_dib,
                          DIB_RGB_COLORS, SRCCOPY));
    area("24-bit", 100, 0, 108, 8);
    area("24-bit grown", 110, 0, 122, 10);

    /* Lines under R2_XORPEN: drawn once on a pattern, and twice, which
     * leaves what was there. */
    fill(0, 20, 60, 60, RGB(0, 128, 255));
    fill(30, 20, 60, 60, RGB(255, 255, 255));
    HPEN red = CreatePen(PS_SOLID, 1, RGB(255, 0, 0));
    HGDIOBJ old_pen = SelectObject(g_dc, red);
    line("rop2 was");
    say_pad(SetROP2(g_dc, R2_XORPEN));
    MoveToEx(g_dc, 2, 22, NULL);
    LineTo(g_dc, 58, 58);
    MoveToEx(g_dc, 58, 22, NULL);
    LineTo(g_dc, 2, 40);
    LineTo(g_dc, 2, 22);
    LineTo(g_dc, 58, 22);
    MoveToEx(g_dc, 10, 58, NULL);
    LineTo(g_dc, 50, 26);
    MoveToEx(g_dc, 10, 58, NULL);
    LineTo(g_dc, 50, 26);
    SetROP2(g_dc, R2_COPYPEN);
    area("xor lines", 0, 20, 60, 60);

    /* Rectangle: the pen's outline, the brush's inside. */
    HPEN blue = CreatePen(PS_SOLID, 1, RGB(0, 0, 255));
    SelectObject(g_dc, blue);
    HBRUSH green = CreateSolidBrush(RGB(0, 160, 0));
    HGDIOBJ old_brush = SelectObject(g_dc, green);
    line("rectangle");
    say_pad(Rectangle(g_dc, 70, 20, 110, 50));
    SelectObject(g_dc, GetStockObject(NULL_BRUSH));
    say_pad(Rectangle(g_dc, 74, 24, 90, 36));
    SelectObject(g_dc, GetStockObject(NULL_PEN));
    SelectObject(g_dc, green);
    say_pad(Rectangle(g_dc, 112, 20, 130, 40));
    area("rectangles", 66, 18, 134, 54);
    SelectObject(g_dc, old_brush);
    SelectObject(g_dc, old_pen);
    DeleteObject(green);
    DeleteObject(blue);
    DeleteObject(red);

    /* A monochrome bitmap from bits, blitted three ways. */
    static const WORD k_bits[8] = {0xff00, 0x8100, 0xbd00, 0xa500, 0xa500, 0xbd00, 0x8100, 0xff00};
    HBITMAP mono = CreateBitmap(8, 8, 1, 1, k_bits);
    HDC source = CreateCompatibleDC(g_dc);
    HGDIOBJ old_bitmap = SelectObject(source, mono);
    fill(140, 20, 200, 40, RGB(200, 100, 50));
    SetTextColor(g_dc, RGB(0, 0, 160));
    SetBkColor(g_dc, RGB(255, 255, 200));
    line("createbitmap blits");
    say_pad(BitBlt(g_dc, 140, 20, 8, 8, source, 0, 0, SRCCOPY));
    say_pad(BitBlt(g_dc, 150, 20, 8, 8, source, 0, 0, SRCAND));
    say_pad(BitBlt(g_dc, 160, 20, 8, 8, source, 0, 0, SRCINVERT));
    say_pad(BitBlt(g_dc, 170, 20, 8, 8, source, 0, 0, SRCPAINT));
    area("mono blits", 140, 20, 180, 28);
    BITMAP info;
    GetObjectA(mono, sizeof info, &info);
    line("createbitmap");
    say_pad(info.bmWidth);
    say_pad(info.bmHeight);
    say_pad(info.bmWidthBytes);
    say_pad(info.bmBitsPixel);
    SelectObject(source, old_bitmap);
    DeleteDC(source);
    DeleteObject(mono);

    /* PATCOPY with a brush. */
    fill(200, 20, 240, 40, RGB(10, 20, 30));
    area("patcopy", 196, 16, 244, 44);
}

/* ---- Start ----------------------------------------------------------------------------------- */

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

/* The Liberation files next to the program, by their 8.3 names, given to
 * Windows XP for this process's session. Where they are already installed
 * (SavanXP) the call fails and nothing changes. */
static void add_liberation(void) {
    static const char *const k_files[12] = {"LSANS", "LSANSB", "LSANSI", "LSANSZ", "LSERIF", "LSERIFB",
                                            "LSERIFI", "LSERIFZ", "LMONO", "LMONOB", "LMONOI", "LMONOZ"};
    WCHAR path[300];
    DWORD n = GetModuleFileNameW(NULL, path, 260);
    while (n > 0 && path[n - 1] != '\\') {
        --n;
    }
    for (unsigned i = 0; i < 12; ++i) {
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

void start(void) {
    g_dump = argument(W("pixels"));
    if (argument(W("liberation"))) {
        k_faces = k_liberation_faces;
        add_liberation();
    }

    HDC screen = GetDC(NULL);
    g_dc = CreateCompatibleDC(screen);
    HBITMAP bitmap = CreateCompatibleBitmap(screen, AREA_W, AREA_H);
    HGDIOBJ old = SelectObject(g_dc, bitmap);
    say("text.exe: screen ");
    say_int(GetDeviceCaps(screen, BITSPIXEL));
    say(" bits, ");
    say_int(GetDeviceCaps(screen, LOGPIXELSY));
    say(" dpi");
    /* Whether the system smooths screen fonts, and how: what decides grey
     * edges where a face's gasp table allows them. */
    UINT smoothing = 0;
    UINT type = 0;
    line("font smoothing");
    say_yes(SystemParametersInfoW(0x004a /* SPI_GETFONTSMOOTHING */, 0, &smoothing, 0));
    say_pad((long)smoothing);
    say(" ");
    say_yes(SystemParametersInfoW(0x200a /* SPI_GETFONTSMOOTHINGTYPE */, 0, &type, 0));
    say_pad((long)type);

    fonts();
    enumerate("all, the three faces", W(""), DEFAULT_CHARSET, 0);
    enumerate("the sans face, every charset", k_faces[0].face, DEFAULT_CHARSET, 1);
    enumerate("the serif face, ANSI", k_faces[1].face, ANSI_CHARSET, 1);
    text_drawn();
    pictures();
    say("\r\n");

    SelectObject(g_dc, old);
    DeleteObject(bitmap);
    DeleteDC(g_dc);
    ReleaseDC(NULL, screen);
    ExitProcess(0);
}
