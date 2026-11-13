/*
 * gdi32.dll -- The drawing calls of the W0 matrix that a window needs
 * (docs/userland/WIN32.md, "user32 and gdi32"): device contexts on a window's
 * surface, solid brushes, the stock pens, one font, PatBlt, lines, pixels and
 * text. Bitmaps and memory device contexts are W4's.
 *
 * Objects live in fixed tables; a handle is (type << 16) | slot, the type
 * being Windows' own object type number. Text is the platform's 13 px UI
 * strike (MS Sans Serif, docs/desktop/UI_FONTS.md) for every face, made bold
 * the way Windows emboldens a bitmap font: drawn twice, one pixel apart.
 */
#define SXW_BUILDING_GDI32
#include <windows.h>

#include "raster.h"
#include "sxw_abi.h"
#include "sxw_gdi.h"

#include "../../../../system/gfx/gfx_font_noto.inc"

typedef unsigned int u32;

__declspec(dllimport) u32 WINAPI SxwService(u32 service, const u32 *arguments);

#define OBJ_DC 1u
#define OBJ_PEN 2u
#define OBJ_BRUSH 3u
#define OBJ_FONT 6u

#define MAX_DCS 32
#define MAX_BRUSHES 256
#define MAX_FONTS 32
#define CLR_INVALID 0xffffffffu

typedef struct brush {
    int used;
    int stock;
    int null;
    u32 color; /* 0x00RRGGBB */
} brush;

typedef struct pen {
    int null;
    u32 color;
} pen;

typedef struct font {
    int used;
    int stock;
    int bold;
} font;

typedef struct dc {
    int used;
    sxw_surface *surface;
    int x;
    int y;
    sxw_region visible;
    HBRUSH brush;
    HPEN pen;
    HFONT font;
    u32 text_color;
    u32 back_color;
    int back_mode;
    int rop2;
    int cx; /* the current position, logical */
    int cy;
    POINT brush_origin;
} dc;

static dc g_dcs[MAX_DCS];
static brush g_brushes[MAX_BRUSHES];
static font g_fonts[MAX_FONTS];
/* WHITE_PEN, BLACK_PEN, NULL_PEN */
static const pen k_pens[3] = {{0, 0xffffffu}, {0, 0}, {1, 0}};

static HGDIOBJ make_handle(u32 type, u32 slot) {
    return (HGDIOBJ)((type << 16) | (slot + 1u));
}

static u32 handle_type(HGDIOBJ h) {
    return (u32)h >> 16;
}

static u32 handle_slot(HGDIOBJ h) {
    return ((u32)h & 0xffffu) - 1u;
}

static dc *dc_of(HDC h) {
    u32 slot = handle_slot(h);
    return handle_type(h) == OBJ_DC && slot < MAX_DCS && g_dcs[slot].used ? &g_dcs[slot] : 0;
}

static brush *brush_of(HGDIOBJ h) {
    u32 slot = handle_slot(h);
    return handle_type(h) == OBJ_BRUSH && slot < MAX_BRUSHES && g_brushes[slot].used ? &g_brushes[slot] : 0;
}

static const pen *pen_of(HGDIOBJ h) {
    u32 slot = handle_slot(h);
    return handle_type(h) == OBJ_PEN && slot < 3u ? &k_pens[slot] : 0;
}

static font *font_of(HGDIOBJ h) {
    u32 slot = handle_slot(h);
    return handle_type(h) == OBJ_FONT && slot < MAX_FONTS && g_fonts[slot].used ? &g_fonts[slot] : 0;
}

static u32 to_pixel(COLORREF c) {
    return ((c & 0xffu) << 16) | (c & 0xff00u) | ((c >> 16) & 0xffu);
}

static COLORREF to_colorref(u32 p) {
    return ((p & 0xffu) << 16) | (p & 0xff00u) | ((p >> 16) & 0xffu);
}

/* ---- Stock objects ----------------------------------------------------------- */

/* Brush slots 0..5 are the stock brushes; font slot 0 is the one stock font. */
static void init_stock(void) {
    static const u32 k_grays[5] = {0xffffffu, 0xc0c0c0u, 0x808080u, 0x404040u, 0};
    for (u32 i = 0; i < 6u; ++i) {
        g_brushes[i].used = 1;
        g_brushes[i].stock = 1;
        g_brushes[i].null = i == 5u;
        g_brushes[i].color = i < 5u ? k_grays[i] : 0;
    }
    /* Windows' System font, the stock font a device context starts with,
     * is a bold face. */
    g_fonts[0].used = 1;
    g_fonts[0].stock = 1;
    g_fonts[0].bold = 1;
}

HGDIOBJ WINAPI GetStockObject(int i) {
    if (!g_brushes[0].used) {
        init_stock();
    }
    if (i >= WHITE_BRUSH && i <= NULL_BRUSH) {
        return make_handle(OBJ_BRUSH, (u32)i);
    }
    if (i >= WHITE_PEN && i <= NULL_PEN) {
        return make_handle(OBJ_PEN, (u32)(i - WHITE_PEN));
    }
    if (i == SYSTEM_FONT || i == DEFAULT_GUI_FONT || (i >= 10 && i <= 17 && i != 15)) {
        return make_handle(OBJ_FONT, 0);
    }
    return NULL;
}

/* ---- Objects ------------------------------------------------------------------- */

HBRUSH WINAPI CreateSolidBrush(COLORREF color) {
    if (!g_brushes[0].used) {
        init_stock();
    }
    for (u32 i = 6; i < MAX_BRUSHES; ++i) {
        if (!g_brushes[i].used) {
            g_brushes[i].used = 1;
            g_brushes[i].stock = 0;
            g_brushes[i].null = 0;
            g_brushes[i].color = to_pixel(color);
            return (HBRUSH)make_handle(OBJ_BRUSH, i);
        }
    }
    return NULL;
}

/* Any face, size, width, angle and quality: the platform has one strike.
 * A weight of 600 or more is bold. */
HFONT WINAPI CreateFontW(int cHeight, int cWidth, int cEscapement, int cOrientation, int cWeight, DWORD bItalic,
                         DWORD bUnderline, DWORD bStrikeOut, DWORD iCharSet, DWORD iOutPrecision,
                         DWORD iClipPrecision, DWORD iQuality, DWORD iPitchAndFamily, LPCWSTR pszFaceName) {
    (void)cHeight;
    (void)cWidth;
    (void)cEscapement;
    (void)cOrientation;
    (void)bItalic;
    (void)bUnderline;
    (void)bStrikeOut;
    (void)iCharSet;
    (void)iOutPrecision;
    (void)iClipPrecision;
    (void)iQuality;
    (void)iPitchAndFamily;
    (void)pszFaceName;
    if (!g_brushes[0].used) {
        init_stock();
    }
    for (u32 i = 1; i < MAX_FONTS; ++i) {
        if (!g_fonts[i].used) {
            g_fonts[i].used = 1;
            g_fonts[i].stock = 0;
            g_fonts[i].bold = cWeight >= 600;
            return (HFONT)make_handle(OBJ_FONT, i);
        }
    }
    return NULL;
}

BOOL WINAPI DeleteObject(HGDIOBJ ho) {
    brush *b = brush_of(ho);
    if (b != 0) {
        if (!b->stock) {
            b->used = 0;
        }
        return TRUE;
    }
    font *f = font_of(ho);
    if (f != 0) {
        if (!f->stock) {
            f->used = 0;
        }
        return TRUE;
    }
    if (pen_of(ho) != 0) {
        return TRUE;
    }
    dc *d = dc_of((HDC)ho);
    if (d != 0) {
        d->used = 0;
        return TRUE;
    }
    return FALSE;
}

HGDIOBJ WINAPI SelectObject(HDC hdc, HGDIOBJ h) {
    dc *d = dc_of(hdc);
    if (d == 0) {
        return NULL;
    }
    HGDIOBJ old = NULL;
    if (brush_of(h) != 0) {
        old = d->brush;
        d->brush = (HBRUSH)h;
    } else if (pen_of(h) != 0) {
        old = d->pen;
        d->pen = (HPEN)h;
    } else if (font_of(h) != 0) {
        old = d->font;
        d->font = (HFONT)h;
    }
    return old;
}

/* ---- Device contexts ------------------------------------------------------------ */

__declspec(dllexport) void *WINAPI SxwCreateSurfaceDC(sxw_surface *surface, int x, int y,
                                                      const sxw_region *visible) {
    if (!g_brushes[0].used) {
        init_stock();
    }
    for (u32 i = 0; i < MAX_DCS; ++i) {
        dc *d = &g_dcs[i];
        if (!d->used) {
            d->used = 1;
            d->surface = surface;
            d->x = x;
            d->y = y;
            d->visible = *visible;
            d->brush = (HBRUSH)make_handle(OBJ_BRUSH, WHITE_BRUSH);
            d->pen = (HPEN)make_handle(OBJ_PEN, BLACK_PEN - WHITE_PEN);
            d->font = (HFONT)make_handle(OBJ_FONT, 0);
            d->text_color = 0;
            d->back_color = 0xffffffu;
            d->back_mode = OPAQUE;
            d->rop2 = R2_COPYPEN;
            d->cx = 0;
            d->cy = 0;
            d->brush_origin.x = 0;
            d->brush_origin.y = 0;
            return make_handle(OBJ_DC, i);
        }
    }
    return 0;
}

__declspec(dllexport) sxw_surface *WINAPI SxwSurfaceOfDC(void *hdc) {
    dc *d = dc_of((HDC)hdc);
    return d != 0 ? d->surface : 0;
}

static sxw_target target_of(const dc *d) {
    sxw_target t = {d->surface->pixels, d->surface->stride, d->surface->width, d->surface->height};
    return t;
}

/* Marks what a call touched, already in surface coordinates. */
static void touched(dc *d, const sxw_rect *r) {
    sxw_rect clipped;
    const sxw_rect bounds = sxw_region_bounds(&d->visible);
    if (sxw_rect_intersect(&clipped, r, &bounds)) {
        sxw_region_union_rect(&d->surface->dirty, &clipped);
    }
}

int WINAPI GetDeviceCaps(HDC hdc, int index) {
    (void)hdc;
    u32 size[2] = {0, 0};
    u32 arguments[1] = {(u32)size};
    switch (index) {
    case HORZRES:
    case VERTRES:
        SxwService(SXW_SVC_DISPLAY, arguments);
        return (int)(index == HORZRES ? size[0] : size[1]);
    case LOGPIXELSX:
    case LOGPIXELSY:
        return 96;
    case BITSPIXEL:
        return 32;
    case PLANES:
        return 1;
    case NUMCOLORS:
        return -1; /* more than 256 colours */
    default:
        return 0;
    }
}

BOOL WINAPI SetBrushOrgEx(HDC hdc, int x, int y, LPPOINT lppt) {
    dc *d = dc_of(hdc);
    if (d == 0) {
        return FALSE;
    }
    if (lppt != NULL) {
        *lppt = d->brush_origin;
    }
    d->brush_origin.x = x;
    d->brush_origin.y = y;
    return TRUE;
}

int WINAPI SetROP2(HDC hdc, int rop2) {
    dc *d = dc_of(hdc);
    if (d == 0 || rop2 < R2_BLACK || rop2 > R2_WHITE) {
        return 0;
    }
    int old = d->rop2;
    d->rop2 = rop2;
    return old;
}

COLORREF WINAPI SetTextColor(HDC hdc, COLORREF color) {
    dc *d = dc_of(hdc);
    if (d == 0) {
        return CLR_INVALID;
    }
    COLORREF old = to_colorref(d->text_color);
    d->text_color = to_pixel(color);
    return old;
}

COLORREF WINAPI SetBkColor(HDC hdc, COLORREF color) {
    dc *d = dc_of(hdc);
    if (d == 0) {
        return CLR_INVALID;
    }
    COLORREF old = to_colorref(d->back_color);
    d->back_color = to_pixel(color);
    return old;
}

/* ---- Fills, lines and pixels --------------------------------------------------- */

/* A ROP3 that reads the destination or the pattern only. */
static int pattern_rop(DWORD rop) {
    const u32 index = (rop >> 16) & 0xffu;
    /* Not reading the source: the result is the same whatever S is. */
    return ((index >> 2) & 0x33u) == (index & 0x33u);
}

BOOL WINAPI PatBlt(HDC hdc, int x, int y, int w, int h, DWORD rop) {
    dc *d = dc_of(hdc);
    if (d == 0 || !pattern_rop(rop)) {
        return FALSE;
    }
    if (w < 0) {
        x += w;
        w = -w;
    }
    if (h < 0) {
        y += h;
        h = -h;
    }
    const brush *b = brush_of(d->brush);
    const u32 index = (rop >> 16) & 0xffu;
    /* A null brush draws nothing where the pattern matters. */
    if ((b == 0 || b->null) && ((index >> 4) & 0x0fu) != (index & 0x0fu)) {
        return TRUE;
    }
    sxw_rect r = {d->x + x, d->y + y, d->x + x + w, d->y + y + h};
    sxw_target t = target_of(d);
    sxw_raster_fill(&t, &d->visible, &r, index, b != 0 ? b->color : 0);
    touched(d, &r);
    return TRUE;
}

BOOL WINAPI MoveToEx(HDC hdc, int x, int y, LPPOINT lppt) {
    dc *d = dc_of(hdc);
    if (d == 0) {
        return FALSE;
    }
    if (lppt != NULL) {
        lppt->x = d->cx;
        lppt->y = d->cy;
    }
    d->cx = x;
    d->cy = y;
    return TRUE;
}

BOOL WINAPI LineTo(HDC hdc, int x, int y) {
    dc *d = dc_of(hdc);
    if (d == 0) {
        return FALSE;
    }
    const pen *p = pen_of(d->pen);
    if (p != 0 && !p->null) {
        sxw_target t = target_of(d);
        sxw_raster_line(&t, &d->visible, d->x + d->cx, d->y + d->cy, d->x + x, d->y + y, d->rop2, p->color);
        sxw_rect r = {d->x + (d->cx < x ? d->cx : x), d->y + (d->cy < y ? d->cy : y),
                      d->x + (d->cx > x ? d->cx : x) + 1, d->y + (d->cy > y ? d->cy : y) + 1};
        touched(d, &r);
    }
    d->cx = x;
    d->cy = y;
    return TRUE;
}

typedef struct dda_context {
    LINEDDAPROC proc;
    LPARAM data;
} dda_context;

static void dda_point(int x, int y, void *context) {
    dda_context *c = context;
    c->proc(x, y, c->data);
}

/* The points LineTo would draw, each passed to the program's procedure. */
BOOL WINAPI LineDDA(int xStart, int yStart, int xEnd, int yEnd, LINEDDAPROC lpProc, LPARAM data) {
    if (lpProc == NULL) {
        return FALSE;
    }
    dda_context c = {lpProc, data};
    sxw_line_points(xStart, yStart, xEnd, yEnd, dda_point, &c);
    return TRUE;
}

COLORREF WINAPI SetPixel(HDC hdc, int x, int y, COLORREF color) {
    dc *d = dc_of(hdc);
    if (d == 0) {
        return CLR_INVALID;
    }
    sxw_target t = target_of(d);
    const int sx = d->x + x;
    const int sy = d->y + y;
    if (!sxw_raster_visible(&t, &d->visible, sx, sy)) {
        return CLR_INVALID;
    }
    t.pixels[sy * t.stride + sx] = to_pixel(color);
    sxw_rect r = {sx, sy, sx + 1, sy + 1};
    touched(d, &r);
    return color & 0x00ffffffu;
}

COLORREF WINAPI GetPixel(HDC hdc, int x, int y) {
    dc *d = dc_of(hdc);
    if (d == 0) {
        return CLR_INVALID;
    }
    sxw_target t = target_of(d);
    const int sx = d->x + x;
    const int sy = d->y + y;
    if (!sxw_raster_visible(&t, &d->visible, sx, sy)) {
        return CLR_INVALID;
    }
    return to_colorref(t.pixels[sy * t.stride + sx]);
}

BOOL WINAPI PtVisible(HDC hdc, int x, int y) {
    dc *d = dc_of(hdc);
    if (d == 0) {
        return FALSE;
    }
    sxw_target t = target_of(d);
    return sxw_raster_visible(&t, &d->visible, d->x + x, d->y + y);
}

/* ---- Text ------------------------------------------------------------------------- */

static int is_bold(const dc *d) {
    const font *f = font_of(d->font);
    return f != 0 && f->bold;
}

static int advance_of(unsigned int c, int bold) {
    return sx_noto_glyph(c)->advance + (bold ? 1 : 0);
}

BOOL WINAPI GetTextExtentPoint32W(HDC hdc, LPCWSTR lpString, int c, LPSIZE psizl) {
    dc *d = dc_of(hdc);
    if (d == 0 || psizl == NULL || c < 0) {
        return FALSE;
    }
    const int bold = is_bold(d);
    int width = 0;
    for (int i = 0; i < c; ++i) {
        width += advance_of(lpString[i], bold);
    }
    psizl->cx = width;
    psizl->cy = SX_NOTO_LINE_HEIGHT;
    return TRUE;
}

BOOL WINAPI GetTextMetricsW(HDC hdc, LPTEXTMETRICW lptm) {
    dc *d = dc_of(hdc);
    if (d == 0 || lptm == NULL) {
        return FALSE;
    }
    const int bold = is_bold(d);
    int widest = 0;
    for (unsigned int c = 0x20; c <= 0xff; ++c) {
        const int a = advance_of(c, bold);
        widest = a > widest ? a : widest;
    }
    unsigned char *p = (unsigned char *)lptm;
    for (unsigned i = 0; i < sizeof(*lptm); ++i) {
        p[i] = 0;
    }
    lptm->tmHeight = SX_NOTO_LINE_HEIGHT;
    lptm->tmAscent = SX_NOTO_ASCENT;
    lptm->tmDescent = SX_NOTO_DESCENT;
    lptm->tmInternalLeading = 2;
    lptm->tmAveCharWidth = advance_of('x', bold);
    lptm->tmMaxCharWidth = widest;
    lptm->tmWeight = bold ? FW_BOLD : FW_NORMAL;
    lptm->tmDigitizedAspectX = 96;
    lptm->tmDigitizedAspectY = 96;
    lptm->tmFirstChar = 0x20;
    lptm->tmLastChar = 0xff;
    lptm->tmDefaultChar = 0x80;
    lptm->tmBreakChar = 0x20;
    lptm->tmPitchAndFamily = 0x21; /* variable pitch, swiss */
    lptm->tmCharSet = ANSI_CHARSET;
    return TRUE;
}

/* Left and top alignment, the reference point the top-left of the text. */
BOOL WINAPI TextOutW(HDC hdc, int x, int y, LPCWSTR lpString, int c) {
    dc *d = dc_of(hdc);
    if (d == 0 || c < 0 || (c > 0 && lpString == NULL)) {
        return FALSE;
    }
    SIZE extent;
    GetTextExtentPoint32W(hdc, lpString, c, &extent);
    sxw_target t = target_of(d);
    sxw_rect box = {d->x + x, d->y + y, d->x + x + extent.cx, d->y + y + extent.cy};
    if (d->back_mode == OPAQUE) {
        sxw_raster_fill(&t, &d->visible, &box, 0xf0u, d->back_color);
    }
    const int bold = is_bold(d);
    int pen = d->x + x;
    for (int i = 0; i < c; ++i) {
        const struct sx_noto_glyph *g = sx_noto_glyph(lpString[i]);
        const int gx = pen + g->left;
        const int gy = d->y + y + SX_NOTO_ASCENT - g->top;
        for (int pass = 0; pass <= bold; ++pass) {
            sxw_raster_glyph(&t, &d->visible, gx + pass, gy, kNotoCoverage + g->offset, g->width, g->rows,
                             d->text_color);
        }
        pen += g->advance + bold;
    }
    touched(d, &box);
    return TRUE;
}
