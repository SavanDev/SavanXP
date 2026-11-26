/*
 * gdi32.dll -- The drawing calls of the W0 matrix that a window needs
 * (docs/userland/WIN32.md, "user32 and gdi32"): device contexts on a window's
 * surface and on bitmaps, device-dependent bitmaps made from resources'
 * device-independent ones, solid brushes, the stock pens, one font, PatBlt,
 * BitBlt, StretchBlt, lines, pixels and text.
 *
 * A bitmap is either monochrome or the screen's format; both keep 0x00RRGGBB
 * pixels, a monochrome one only black and white, and colours drawn on it are
 * first made black or white, the nearer one. Between the two formats BitBlt
 * converts as Windows does: a monochrome source's black becomes the
 * destination's text colour and its white the background colour; a colour
 * source becomes white where it equals its own background colour.
 *
 * Objects live in fixed tables; a handle is (type << 16) | slot, the type
 * being Windows' own object type number. Text comes two ways (WP6,
 * docs/userland/WIN32.md, "Text"): the faces the interface asks for (the
 * stock fonts, MS Shell Dlg, MS Sans Serif, Tahoma) are the platform's 13 px
 * UI strike (docs/desktop/UI_FONTS.md), made bold the way Windows emboldens a
 * bitmap font, drawn twice one pixel apart; every other face is a TrueType
 * face of the host, through the font services (sxw_abi.h, SXW_SVC_FONT_*).
 */
#define SXW_BUILDING_GDI32
#include <windows.h>

#include "codepage.h"
#include "dib.h"
#include "raster.h"
#include "sxw_abi.h"
#include "sxw_gdi.h"

#include "../../../../system/gfx/gfx_font_noto.inc"

typedef unsigned int u32;

__declspec(dllimport) u32 WINAPI SxwService(u32 service, const u32 *arguments);
__declspec(dllimport) void *WINAPI SxwHeapAllocate(u32 size, u32 zero);
__declspec(dllimport) int WINAPI SxwHeapFree(void *pointer);

#define OBJ_DC 1u
#define OBJ_PEN 2u
#define OBJ_BRUSH 3u
#define OBJ_FONT 6u
#define OBJ_BITMAP 7u

#define MAX_DCS 32
#define MAX_BRUSHES 256
#define MAX_FONTS 64
#define MAX_PENS 64
#define MAX_BITMAPS 256
#define WHITE 0x00ffffffu
#define CLR_INVALID 0xffffffffu

typedef struct brush {
    int used;
    int stock;
    int null;
    u32 color; /* 0x00RRGGBB */
} brush;

typedef struct pen {
    int used;
    int null;
    u32 color;
} pen;

/* The vertical metrics a font reports and draws its baseline with; see
 * font_metrics(). A font of the host (host != 0) has its own. */
typedef struct font {
    int used;
    int stock;
    int bold;
    int height;
    int ascent;
    int leading;
    /* What it was made from, for GetObjectW. */
    LOGFONTW logical;
    u32 host;                 /* the host's font id, 0 for the strike */
    sxw_font_metrics metrics; /* the host font's */
    int advances_ready;
    short advances[256];      /* the host font's, U+0000 to U+00FF */
} font;

typedef struct bitmap {
    int used;
    int stock;
    int mono;
    int selected; /* into a memory device context */
    int deleted;  /* while selected: freed when it is deselected */
    sxw_surface surface;
} bitmap;

typedef struct dc {
    int used;
    int memory;
    HBITMAP bitmap; /* a memory device context's */
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
    UINT text_align; /* TA_* */
} dc;

static dc g_dcs[MAX_DCS];
static brush g_brushes[MAX_BRUSHES];
static font g_fonts[MAX_FONTS];
/* Slots 0..2 are the stock pens, WHITE_PEN, BLACK_PEN and NULL_PEN. */
static pen g_pens[MAX_PENS] = {{1, 0, 0xffffffu}, {1, 0, 0}, {1, 1, 0}};
/* Slot 0 is the stock 1x1 monochrome bitmap every memory DC starts with. */
static bitmap g_bitmaps[MAX_BITMAPS];
static unsigned int g_stock_bitmap_pixel;

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

static pen *pen_of(HGDIOBJ h) {
    u32 slot = handle_slot(h);
    return handle_type(h) == OBJ_PEN && slot < MAX_PENS && g_pens[slot].used ? &g_pens[slot] : 0;
}

/* A bitmap a device context may still hold after DeleteObject. */
static bitmap *bitmap_held(HGDIOBJ h) {
    u32 slot = handle_slot(h);
    return handle_type(h) == OBJ_BITMAP && slot < MAX_BITMAPS && g_bitmaps[slot].used ? &g_bitmaps[slot] : 0;
}

/* A bitmap a program may name. */
static bitmap *bitmap_of(HGDIOBJ h) {
    bitmap *b = bitmap_held(h);
    return b != 0 && !b->deleted ? b : 0;
}

/* A deselected bitmap is freed if it was deleted while selected. */
static void deselect(bitmap *b) {
    if (b == 0 || b->stock) {
        return;
    }
    b->selected = 0;
    if (b->deleted) {
        SxwHeapFree(b->surface.pixels);
        b->used = 0;
        b->deleted = 0;
    }
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
     * is a bold face 16 pixels high with an ascent of 13 at 96 dpi. */
    g_fonts[0].used = 1;
    g_fonts[0].stock = 1;
    g_fonts[0].bold = 1;
    g_fonts[0].height = 16;
    g_fonts[0].ascent = 13;
    g_fonts[0].leading = 3;
    g_fonts[0].logical.lfHeight = 16;
    g_fonts[0].logical.lfWeight = 700;
    g_fonts[0].logical.lfCharSet = 0; /* ANSI_CHARSET */
    g_fonts[0].logical.lfPitchAndFamily = 0x22; /* VARIABLE_PITCH | FF_SWISS */
    static const char k_system[] = "System";
    for (u32 i = 0; i < sizeof k_system; ++i) {
        g_fonts[0].logical.lfFaceName[i] = (WCHAR)k_system[i];
    }
    g_bitmaps[0].used = 1;
    g_bitmaps[0].stock = 1;
    g_bitmaps[0].mono = 1;
    g_bitmaps[0].surface.pixels = &g_stock_bitmap_pixel;
    g_bitmaps[0].surface.stride = g_bitmaps[0].surface.width = g_bitmaps[0].surface.height = 1;
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

/* The height, ascent and internal leading Windows XP gives Tahoma, the face
 * MS Shell Dlg maps to, at a requested height: a negative height is the em
 * size, a positive one the cell height, 0 the platform's own strike. Ascent
 * and descent are 2049 and 423 units of a 2048-unit em, each rounded on its
 * own as the TrueType rasterizer rounds them. Programs size windows from
 * these numbers (Solitaire's status bar, dialog units), so they follow the
 * requested font even though every font is drawn with the one strike. */
static void font_metrics(int requested, font *f) {
    if (requested == 0) {
        f->height = SX_NOTO_LINE_HEIGHT;
        f->ascent = SX_NOTO_ASCENT;
        f->leading = 2;
        return;
    }
    const int cell = requested > 0;
    const int units = cell ? 2049 + 423 : 2048;
    const int size = cell ? requested : -requested;
    f->ascent = (size * 2049 + units / 2) / units;
    const int descent = (size * 423 + units / 2) / units;
    f->height = f->ascent + descent;
    f->leading = f->height - (cell ? (size * 2048 + units / 2) / units : size);
}

static int lower_w(unsigned int c) {
    return c >= 'A' && c <= 'Z' ? (int)c + 32 : (int)c;
}

static int same_face(const WCHAR *a, const char *b) {
    unsigned i = 0;
    for (; a[i] != 0 && b[i] != 0; ++i) {
        if (lower_w(a[i]) != lower_w((unsigned char)b[i])) {
            return 0;
        }
    }
    return a[i] == 0 && b[i] == 0;
}

/* The faces the interface asks for, which stay on the platform's strike as
 * the desktop's own text does (WIN32_WORDPAD_ROADMAP.md, WP6): dialogs,
 * menus and controls keep Windows XP's metrics and the system's look. */
static int strike_face(LPCWSTR face) {
    static const char *const k_faces[] = {"System", "MS Shell Dlg", "MS Shell Dlg 2", "MS Sans Serif",
                                          "Microsoft Sans Serif", "Tahoma"};
    if (face == NULL) {
        return 0;
    }
    for (unsigned i = 0; i < sizeof k_faces / sizeof k_faces[0]; ++i) {
        if (same_face(face, k_faces[i])) {
            return 1;
        }
    }
    return 0;
}

/* A TrueType face of the host for the request; 0 when the host has none,
 * and then the strike draws it. */
static u32 open_host_font(const LOGFONTW *l, sxw_font_metrics *out) {
    static sxw_font_request request;
    unsigned char *p = (unsigned char *)&request;
    for (unsigned i = 0; i < sizeof request; ++i) {
        p[i] = 0;
    }
    request.height = l->lfHeight;
    request.width = l->lfWidth;
    request.weight = (u32)l->lfWeight;
    request.italic = l->lfItalic;
    request.charset = l->lfCharSet;
    request.pitch_family = l->lfPitchAndFamily;
    request.quality = l->lfQuality;
    for (unsigned i = 0; i < 31u && l->lfFaceName[i] != 0; ++i) {
        request.face[i] = l->lfFaceName[i];
    }
    u32 arguments[2] = {(u32)&request, (u32)out};
    return SxwService(SXW_SVC_FONT_OPEN, arguments);
}

/* The strike for the interface's faces; a TrueType face of the host for
 * every other, at any size, weight and slant. Width, escapement and
 * orientation are kept for GetObjectW but not drawn: text is upright at
 * the face's own width. */
HFONT WINAPI CreateFontW(int cHeight, int cWidth, int cEscapement, int cOrientation, int cWeight, DWORD bItalic,
                         DWORD bUnderline, DWORD bStrikeOut, DWORD iCharSet, DWORD iOutPrecision,
                         DWORD iClipPrecision, DWORD iQuality, DWORD iPitchAndFamily, LPCWSTR pszFaceName) {
    if (!g_brushes[0].used) {
        init_stock();
    }
    for (u32 i = 1; i < MAX_FONTS; ++i) {
        if (!g_fonts[i].used) {
            g_fonts[i].used = 1;
            g_fonts[i].stock = 0;
            g_fonts[i].host = 0;
            g_fonts[i].advances_ready = 0;
            g_fonts[i].bold = cWeight >= 600;
            font_metrics(cHeight, &g_fonts[i]);
            LOGFONTW *l = &g_fonts[i].logical;
            l->lfHeight = cHeight;
            l->lfWidth = cWidth;
            l->lfEscapement = cEscapement;
            l->lfOrientation = cOrientation;
            l->lfWeight = cWeight;
            l->lfItalic = (BYTE)bItalic;
            l->lfUnderline = (BYTE)bUnderline;
            l->lfStrikeOut = (BYTE)bStrikeOut;
            l->lfCharSet = (BYTE)iCharSet;
            l->lfOutPrecision = (BYTE)iOutPrecision;
            l->lfClipPrecision = (BYTE)iClipPrecision;
            l->lfQuality = (BYTE)iQuality;
            l->lfPitchAndFamily = (BYTE)iPitchAndFamily;
            u32 n = 0;
            for (; pszFaceName != NULL && pszFaceName[n] != 0 && n < 31u; ++n) {
                l->lfFaceName[n] = pszFaceName[n];
            }
            l->lfFaceName[n] = 0;
            if (!strike_face(pszFaceName)) {
                g_fonts[i].host = open_host_font(l, &g_fonts[i].metrics);
            }
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
            if (f->host != 0) {
                u32 arguments[1] = {f->host};
                SxwService(SXW_SVC_FONT_CLOSE, arguments);
                f->host = 0;
            }
            f->used = 0;
        }
        return TRUE;
    }
    pen *pe = pen_of(ho);
    if (pe != 0) {
        if (handle_slot(ho) >= 3u) {
            pe->used = 0;
        }
        return TRUE;
    }
    bitmap *bm = bitmap_of(ho);
    if (bm != 0) {
        if (bm->stock) {
            return TRUE;
        }
        if (bm->selected) {
            bm->deleted = 1; /* freed when it is deselected, as Windows does */
            return TRUE;
        }
        SxwHeapFree(bm->surface.pixels);
        bm->used = 0;
        return TRUE;
    }
    dc *d = dc_of((HDC)ho);
    if (d != 0) {
        return DeleteDC((HDC)ho);
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
    } else if (bitmap_of(h) != 0) {
        bitmap *next = bitmap_of(h);
        if (!d->memory || (next->selected && h != d->bitmap)) {
            return NULL;
        }
        deselect(bitmap_held(d->bitmap));
        old = d->bitmap;
        if (!next->stock) {
            next->selected = 1;
        }
        d->bitmap = (HBITMAP)h;
        d->surface = &next->surface;
        const sxw_rect whole = {0, 0, next->surface.width, next->surface.height};
        sxw_region_set(&d->visible, &whole);
    }
    return old;
}

/* Any width draws one pixel wide; the styles but PS_NULL draw solid. */
HPEN WINAPI CreatePen(int iStyle, int cWidth, COLORREF color) {
    (void)cWidth;
    for (u32 i = 3; i < MAX_PENS; ++i) {
        if (!g_pens[i].used) {
            g_pens[i].used = 1;
            g_pens[i].null = iStyle == PS_NULL;
            g_pens[i].color = to_pixel(color);
            return (HPEN)make_handle(OBJ_PEN, i);
        }
    }
    return NULL;
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
            d->memory = 0;
            d->bitmap = NULL;
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
            d->text_align = TA_LEFT | TA_TOP | TA_NOUPDATECP;
            return make_handle(OBJ_DC, i);
        }
    }
    return 0;
}

/* user32's controls and menus draw their text without a background, through
 * this or SetBkMode. The previous mode, or 0. */
__declspec(dllexport) int WINAPI SxwSetBkMode(void *hdc, int mode) {
    dc *d = dc_of((HDC)hdc);
    if (d == 0 || (mode != OPAQUE && mode != TRANSPARENT)) {
        return 0;
    }
    const int old = d->back_mode;
    d->back_mode = mode;
    return old;
}

__declspec(dllexport) sxw_surface *WINAPI SxwSurfaceOfDC(void *hdc) {
    dc *d = dc_of((HDC)hdc);
    return d != 0 ? d->surface : 0;
}

static sxw_target target_of(const dc *d) {
    sxw_target t = {d->surface->pixels, d->surface->stride, d->surface->width, d->surface->height};
    return t;
}

static int is_mono(const dc *d) {
    const bitmap *b = d->memory ? bitmap_held(d->bitmap) : 0;
    return b != 0 && b->mono;
}

/* A colour as drawn on the device context: on a monochrome bitmap, black or
 * white, whichever is nearer. */
static u32 device_color(const dc *d, u32 pixel) {
    if (!is_mono(d)) {
        return pixel;
    }
    long dark = 0;
    long light = 0;
    for (int shift = 0; shift < 24; shift += 8) {
        const long c = (long)((pixel >> shift) & 0xffu);
        dark += c * c;
        light += (255 - c) * (255 - c);
    }
    return light <= dark ? WHITE : 0;
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
    sxw_raster_fill(&t, &d->visible, &r, index, device_color(d, b != 0 ? b->color : 0));
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
        sxw_raster_line(&t, &d->visible, d->x + d->cx, d->y + d->cy, d->x + x, d->y + y, d->rop2,
                        device_color(d, p->color));
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
    const u32 drawn = device_color(d, to_pixel(color));
    t.pixels[sy * t.stride + sx] = drawn;
    sxw_rect r = {sx, sy, sx + 1, sy + 1};
    touched(d, &r);
    return to_colorref(drawn);
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

/* The selected font; a device context always has one. */
static font *selected_font(const dc *d) {
    font *f = font_of(d->font);
    return f != 0 ? f : &g_fonts[0];
}

/* A host font's advances for U+0000 to U+00FF, asked for once. */
static void fill_advances(font *f) {
    static unsigned short units[256];
    static int out[256];
    if (f->advances_ready) {
        return;
    }
    for (u32 i = 0; i < 256u; ++i) {
        units[i] = (unsigned short)i;
    }
    u32 arguments[4] = {f->host, (u32)units, 256u, (u32)out};
    if (SxwService(SXW_SVC_FONT_ADVANCES, arguments) == 0u) {
        for (u32 i = 0; i < 256u; ++i) {
            f->advances[i] = (short)out[i];
        }
    }
    f->advances_ready = 1;
}

static int advance_of(font *f, unsigned int c) {
    if (f->host == 0) {
        return sx_noto_glyph(c)->advance + (f->bold ? 1 : 0);
    }
    if (c < 256u) {
        fill_advances(f);
        return f->advances[c];
    }
    unsigned short unit = (unsigned short)c;
    int advance = 0;
    u32 arguments[4] = {f->host, (u32)&unit, 1u, (u32)&advance};
    SxwService(SXW_SVC_FONT_ADVANCES, arguments);
    return advance;
}

static int font_height(const font *f) {
    return f->host != 0 ? f->metrics.height : f->height;
}

static int font_ascent(const font *f) {
    return f->host != 0 ? f->metrics.ascent : f->ascent;
}

static int text_width(font *f, LPCWSTR s, int c) {
    int width = 0;
    for (int i = 0; i < c; ++i) {
        width += advance_of(f, s[i]);
    }
    return width;
}

BOOL WINAPI GetTextExtentPoint32W(HDC hdc, LPCWSTR lpString, int c, LPSIZE psizl) {
    dc *d = dc_of(hdc);
    if (d == 0 || psizl == NULL || c < 0 || (c > 0 && lpString == NULL)) {
        return FALSE;
    }
    font *f = selected_font(d);
    psizl->cx = text_width(f, lpString, c);
    psizl->cy = font_height(f);
    return TRUE;
}

/* ANSI text is Windows-1252, the code page of the locale. */
static int widen(LPCSTR in, int c, WCHAR *out, int capacity) {
    int n = 0;
    for (; n < c && n < capacity; ++n) {
        out[n] = sxw_ansi_to_wide((unsigned char)in[n]);
    }
    return n;
}

#define ANSI_MAX 1024

BOOL WINAPI GetTextExtentPoint32A(HDC hdc, LPCSTR lpString, int c, LPSIZE psizl) {
    static WCHAR wide[ANSI_MAX];
    if (c < 0 || (c > 0 && lpString == NULL)) {
        return FALSE;
    }
    return GetTextExtentPoint32W(hdc, wide, widen(lpString, c, wide, ANSI_MAX), psizl);
}

BOOL WINAPI GetCharWidthW(HDC hdc, UINT iFirst, UINT iLast, LPINT lpBuffer) {
    dc *d = dc_of(hdc);
    if (d == 0 || lpBuffer == NULL || iLast < iFirst || iLast > 0xffffu) {
        return FALSE;
    }
    font *f = selected_font(d);
    for (UINT c = iFirst; c <= iLast; ++c) {
        lpBuffer[c - iFirst] = advance_of(f, c);
    }
    return TRUE;
}

BOOL WINAPI GetCharWidthA(HDC hdc, UINT iFirst, UINT iLast, LPINT lpBuffer) {
    dc *d = dc_of(hdc);
    if (d == 0 || lpBuffer == NULL || iLast < iFirst || iLast > 0xffu) {
        return FALSE;
    }
    font *f = selected_font(d);
    for (UINT c = iFirst; c <= iLast; ++c) {
        lpBuffer[c - iFirst] = advance_of(f, sxw_ansi_to_wide((unsigned char)c));
    }
    return TRUE;
}

static void zero(void *p, unsigned n) {
    unsigned char *b = p;
    for (unsigned i = 0; i < n; ++i) {
        b[i] = 0;
    }
}

static void fill_text_metrics(font *f, LPTEXTMETRICW lptm) {
    zero(lptm, sizeof(*lptm));
    if (f->host != 0) {
        const sxw_font_metrics *m = &f->metrics;
        lptm->tmHeight = m->height;
        lptm->tmAscent = m->ascent;
        lptm->tmDescent = m->descent;
        lptm->tmInternalLeading = m->internal_leading;
        lptm->tmExternalLeading = m->external_leading;
        lptm->tmAveCharWidth = m->average_width;
        lptm->tmMaxCharWidth = m->maximum_width;
        lptm->tmWeight = (LONG)m->weight;
        lptm->tmFirstChar = (WCHAR)m->first_char;
        lptm->tmLastChar = (WCHAR)m->last_char;
        lptm->tmDefaultChar = (WCHAR)m->default_char;
        lptm->tmBreakChar = (WCHAR)m->break_char;
        lptm->tmItalic = (BYTE)m->italic;
        lptm->tmPitchAndFamily = (BYTE)m->pitch_family;
        lptm->tmCharSet = (BYTE)m->charset;
    } else {
        const int bold = f->bold;
        int widest = 0;
        for (unsigned int c = 0x20; c <= 0xff; ++c) {
            const int a = advance_of(f, c);
            widest = a > widest ? a : widest;
        }
        lptm->tmHeight = f->height;
        lptm->tmAscent = f->ascent;
        lptm->tmDescent = f->height - f->ascent;
        lptm->tmInternalLeading = f->leading;
        lptm->tmAveCharWidth = advance_of(f, 'x');
        lptm->tmMaxCharWidth = widest;
        lptm->tmWeight = bold ? FW_BOLD : FW_NORMAL;
        lptm->tmFirstChar = 0x20;
        lptm->tmLastChar = 0xff;
        lptm->tmDefaultChar = 0x80;
        lptm->tmBreakChar = 0x20;
        lptm->tmPitchAndFamily = 0x21; /* variable pitch, swiss */
        lptm->tmCharSet = ANSI_CHARSET;
    }
    lptm->tmDigitizedAspectX = 96;
    lptm->tmDigitizedAspectY = 96;
    lptm->tmUnderlined = f->logical.lfUnderline != 0 ? 255 : 0;
    lptm->tmStruckOut = f->logical.lfStrikeOut != 0 ? 255 : 0;
}

BOOL WINAPI GetTextMetricsW(HDC hdc, LPTEXTMETRICW lptm) {
    dc *d = dc_of(hdc);
    if (d == 0 || lptm == NULL) {
        return FALSE;
    }
    fill_text_metrics(selected_font(d), lptm);
    return TRUE;
}

static int copy_units(LPWSTR out, int capacity, const unsigned short *in) {
    int n = 0;
    while (in[n] != 0 && n + 1 < capacity) {
        out[n] = in[n];
        ++n;
    }
    if (capacity > 0) {
        out[n] = 0;
    }
    return n;
}

/* The face drawn: the host's family, or the name asked for of the strike's. */
int WINAPI GetTextFaceW(HDC hdc, int c, LPWSTR lpName) {
    dc *d = dc_of(hdc);
    if (d == 0) {
        return 0;
    }
    const font *f = selected_font(d);
    static unsigned short name[32];
    for (unsigned i = 0; i < 32u; ++i) {
        name[i] = f->host != 0 ? f->metrics.family[i] : f->logical.lfFaceName[i];
    }
    name[31] = 0;
    int length = 0;
    while (name[length] != 0) {
        ++length;
    }
    if (lpName == NULL) {
        return length + 1;
    }
    return c > 0 ? copy_units(lpName, c, name) + 1 : 0;
}

/* The outline metrics of a host font, in pixels but the em square, names
 * after the structure as Windows lays them out. The strike is a bitmap
 * font, which has none. */
UINT WINAPI GetOutlineTextMetricsW(HDC hdc, UINT cjCopy, LPOUTLINETEXTMETRICW potm) {
    dc *d = dc_of(hdc);
    font *f = d != 0 ? selected_font(d) : 0;
    if (f == 0 || f->host == 0) {
        return 0;
    }
    const sxw_font_metrics *m = &f->metrics;
    unsigned lengths[3];
    const unsigned short *names[3] = {m->family, m->full, m->style};
    unsigned need = sizeof(OUTLINETEXTMETRICW);
    for (unsigned i = 0; i < 3u; ++i) {
        unsigned n = 0;
        while (names[i][n] != 0) {
            ++n;
        }
        lengths[i] = n;
        need += 2u * (n + 1u);
    }
    need += 2u * (lengths[1] + 1u); /* the full name again, as the face name */
    if (potm == NULL) {
        return need;
    }
    if (cjCopy < sizeof(OUTLINETEXTMETRICW)) {
        return 0;
    }
    static OUTLINETEXTMETRICW o;
    zero(&o, sizeof o);
    o.otmSize = need;
    fill_text_metrics(f, &o.otmTextMetrics);
    for (unsigned i = 0; i < 10u; ++i) {
        ((BYTE *)&o.otmPanoseNumber)[i] = m->panose[i];
    }
    o.otmfsSelection = m->fs_selection;
    o.otmfsType = m->fs_type;
    o.otmsCharSlopeRise = m->slope_rise;
    o.otmsCharSlopeRun = m->slope_run;
    o.otmItalicAngle = m->italic_angle;
    o.otmEMSquare = m->em_square;
    o.otmAscent = m->typo_ascent;
    o.otmDescent = m->typo_descent;
    o.otmLineGap = (UINT)m->typo_line_gap;
    o.otmsCapEmHeight = (UINT)m->cap_height;
    o.otmsXHeight = (UINT)m->x_height;
    o.otmrcFontBox.left = m->box[0];
    o.otmrcFontBox.top = m->box[1];
    o.otmrcFontBox.right = m->box[2];
    o.otmrcFontBox.bottom = m->box[3];
    o.otmMacAscent = m->mac_ascent;
    o.otmMacDescent = m->mac_descent;
    o.otmMacLineGap = (UINT)m->mac_line_gap;
    o.otmusMinimumPPEM = m->minimum_ppem;
    o.otmsStrikeoutSize = (UINT)m->strikeout_size;
    o.otmsStrikeoutPosition = m->strikeout_position;
    o.otmsUnderscoreSize = m->underscore_size;
    o.otmsUnderscorePosition = m->underscore_position;
    /* Family, face (the full name), style, full name. */
    unsigned offset = sizeof(OUTLINETEXTMETRICW);
    const unsigned short *order[4] = {m->family, m->full, m->style, m->full};
    const unsigned order_lengths[4] = {lengths[0], lengths[1], lengths[2], lengths[1]};
    char **slots[4] = {&o.otmpFamilyName, &o.otmpFaceName, &o.otmpStyleName, &o.otmpFullName};
    unsigned char *out = (unsigned char *)potm;
    for (unsigned i = 0; i < 4u; ++i) {
        *slots[i] = (char *)offset;
        for (unsigned k = 0; k <= order_lengths[i]; ++k) {
            if (offset + 2u * k + 1u < cjCopy) {
                out[offset + 2u * k] = (unsigned char)order[i][k];
                out[offset + 2u * k + 1u] = (unsigned char)(order[i][k] >> 8);
            }
        }
        offset += 2u * (order_lengths[i] + 1u);
    }
    unsigned char *from = (unsigned char *)&o;
    for (unsigned i = 0; i < sizeof o && i < cjCopy; ++i) {
        out[i] = from[i];
    }
    return cjCopy < need ? cjCopy : need;
}

int WINAPI GetTextCharsetInfo(HDC hdc, LPFONTSIGNATURE lpSig, DWORD dwFlags) {
    (void)dwFlags;
    dc *d = dc_of(hdc);
    if (d == 0) {
        return DEFAULT_CHARSET;
    }
    const font *f = selected_font(d);
    if (lpSig != NULL) {
        zero(lpSig, sizeof(*lpSig));
        lpSig->fsCsb[0] = f->host != 0 ? f->metrics.code_pages[0] : 1u;
        lpSig->fsCsb[1] = f->host != 0 ? f->metrics.code_pages[1] : 0u;
    }
    return f->host != 0 ? (int)f->metrics.charset : ANSI_CHARSET;
}

UINT WINAPI SetTextAlign(HDC hdc, UINT align) {
    dc *d = dc_of(hdc);
    if (d == 0) {
        return GDI_ERROR;
    }
    const UINT old = d->text_align;
    d->text_align = align;
    return old;
}

UINT WINAPI GetTextAlign(HDC hdc) {
    dc *d = dc_of(hdc);
    return d != 0 ? d->text_align : GDI_ERROR;
}

BOOL WINAPI GetCurrentPositionEx(HDC hdc, LPPOINT lppt) {
    dc *d = dc_of(hdc);
    if (d == 0 || lppt == NULL) {
        return FALSE;
    }
    lppt->x = d->cx;
    lppt->y = d->cy;
    return TRUE;
}

int WINAPI SetBkMode(HDC hdc, int mode) {
    return SxwSetBkMode(hdc, mode);
}

int WINAPI GetBkMode(HDC hdc) {
    dc *d = dc_of(hdc);
    return d != 0 ? d->back_mode : 0;
}

COLORREF WINAPI GetBkColor(HDC hdc) {
    dc *d = dc_of(hdc);
    return d != 0 ? to_colorref(d->back_color) : CLR_INVALID;
}

COLORREF WINAPI GetTextColor(HDC hdc) {
    dc *d = dc_of(hdc);
    return d != 0 ? to_colorref(d->text_color) : CLR_INVALID;
}

/* ---- Enumeration ------------------------------------------------------------------ */

/* The charsets a face is listed under, in the order Windows XP lists them,
 * each with the code page bit (OS/2 ulCodePageRange1) that says the face
 * covers it and the script's name in the locale's language. */
static const struct {
    BYTE charset;
    BYTE bit;
    const char *script;
} k_scripts[] = {
    {ANSI_CHARSET, 0, "Western"},        {HEBREW_CHARSET, 5, "Hebrew"},   {ARABIC_CHARSET, 6, "Arabic"},
    {GREEK_CHARSET, 3, "Greek"},         {TURKISH_CHARSET, 4, "Turkish"}, {BALTIC_CHARSET, 7, "Baltic"},
    {EASTEUROPE_CHARSET, 1, "Central European"}, {RUSSIAN_CHARSET, 2, "Cyrillic"},
    {VIETNAMESE_CHARSET, 8, "Vietnamese"}, {THAI_CHARSET, 16, "Thai"},  {SHIFTJIS_CHARSET, 17, "Japanese"},
    {GB2312_CHARSET, 18, "CHINESE_GB2312"}, {HANGEUL_CHARSET, 19, "Hangul"},
    {CHINESEBIG5_CHARSET, 20, "CHINESE_BIG5"}, {SYMBOL_CHARSET, 31, "Symbol"},
};

static int same_units(const unsigned short *a, const WCHAR *b) {
    unsigned i = 0;
    for (; a[i] != 0 && b[i] != 0; ++i) {
        if (lower_w(a[i]) != lower_w(b[i])) {
            return 0;
        }
    }
    return a[i] == 0 && b[i] == 0;
}

/* The host's TrueType faces; the strike's bitmap faces are not listed. With
 * no face name, each family once, by its first (regular) face; with one,
 * every face of that family. Each under every charset it covers, or only
 * the one asked for. */
int WINAPI EnumFontFamiliesExW(HDC hdc, LOGFONTW *lpLogfont, FONTENUMPROCW lpProc, LPARAM lParam, DWORD dwFlags) {
    (void)hdc;
    (void)dwFlags;
    if (lpLogfont == NULL || lpProc == NULL) {
        return 0;
    }
    static sxw_font_face_entry e;
    static unsigned short last_family[SXW_FONT_FACE_UNITS];
    static ENUMLOGFONTEXW elf;
    static NEWTEXTMETRICEXW ntm;
    const int all = lpLogfont->lfFaceName[0] == 0;
    int result = 1;
    last_family[0] = 0;
    for (u32 index = 0;; ++index) {
        u32 arguments[2] = {index, (u32)&e};
        if (SxwService(SXW_SVC_FONT_FACE, arguments) != 0u) {
            break;
        }
        if (all) {
            if (last_family[0] != 0 && same_units(e.family, (const WCHAR *)last_family)) {
                continue;
            }
            for (unsigned i = 0; i < SXW_FONT_FACE_UNITS; ++i) {
                last_family[i] = e.family[i];
            }
        } else if (!same_units(e.family, lpLogfont->lfFaceName)) {
            continue;
        }
        for (unsigned k = 0; k < sizeof k_scripts / sizeof k_scripts[0]; ++k) {
            const u32 page = k_scripts[k].bit < 32u ? e.code_pages[0] >> k_scripts[k].bit : 0u;
            if ((page & 1u) == 0u) {
                continue;
            }
            if (lpLogfont->lfCharSet != DEFAULT_CHARSET && lpLogfont->lfCharSet != k_scripts[k].charset) {
                continue;
            }
            zero(&elf, sizeof elf);
            zero(&ntm, sizeof ntm);
            LOGFONTW *l = &elf.elfLogFont;
            l->lfHeight = e.height;
            l->lfWidth = e.lf_width;
            l->lfWeight = (LONG)e.weight;
            l->lfItalic = (BYTE)e.italic;
            l->lfCharSet = k_scripts[k].charset;
            l->lfOutPrecision = 3;  /* OUT_STROKE_PRECIS */
            l->lfClipPrecision = 2; /* CLIP_STROKE_PRECIS */
            l->lfQuality = 1;       /* DRAFT_QUALITY */
            l->lfPitchAndFamily = (BYTE)e.lf_pitch_family;
            for (unsigned i = 0; i < 31u && e.family[i] != 0; ++i) {
                l->lfFaceName[i] = e.family[i];
            }
            for (unsigned i = 0; i < 63u && e.full[i] != 0; ++i) {
                elf.elfFullName[i] = e.full[i];
            }
            for (unsigned i = 0; i < 31u && e.style[i] != 0; ++i) {
                elf.elfStyle[i] = e.style[i];
            }
            for (unsigned i = 0; i < 31u && k_scripts[k].script[i] != 0; ++i) {
                elf.elfScript[i] = (WCHAR)k_scripts[k].script[i];
            }
            NEWTEXTMETRICW *m = &ntm.ntmTm;
            m->tmHeight = e.height;
            m->tmAscent = e.ascent;
            m->tmDescent = e.descent;
            m->tmInternalLeading = e.internal_leading;
            m->tmExternalLeading = e.external_leading;
            m->tmAveCharWidth = e.average_width;
            m->tmMaxCharWidth = e.maximum_width;
            m->tmWeight = (LONG)e.weight;
            m->tmDigitizedAspectX = 96;
            m->tmDigitizedAspectY = 96;
            m->tmFirstChar = (WCHAR)e.first_char;
            m->tmLastChar = (WCHAR)e.last_char;
            m->tmDefaultChar = (WCHAR)(e.first_char > 0u ? e.first_char - 1u : 0u);
            m->tmBreakChar = 0x20;
            m->tmItalic = (BYTE)e.italic;
            m->tmPitchAndFamily = (BYTE)e.pitch_family;
            m->tmCharSet = k_scripts[k].charset;
            m->ntmFlags = (e.italic != 0u ? NTM_ITALIC : 0u) | (e.weight >= 700u ? NTM_BOLD : 0u);
            if (m->ntmFlags == 0u) {
                m->ntmFlags = NTM_REGULAR;
            }
            m->ntmFlags |= e.ntm_flags;
            m->ntmSizeEM = e.em_square;
            m->ntmCellHeight = e.cell_height;
            m->ntmAvgWidth = e.average_units;
            for (unsigned i = 0; i < 4u; ++i) {
                ntm.ntmFontSig.fsUsb[i] = e.unicode_ranges[i];
            }
            ntm.ntmFontSig.fsCsb[0] = e.code_pages[0];
            ntm.ntmFontSig.fsCsb[1] = e.code_pages[1];
            result = lpProc((const LOGFONTW *)&elf, (const TEXTMETRICW *)&ntm, TRUETYPE_FONTTYPE, lParam);
            if (result == 0) {
                return 0;
            }
        }
    }
    return result;
}

/* Fonts are the host's: one added here is not drawn. */
int WINAPI AddFontResourceW(LPCWSTR name) {
    (void)name;
    return 0;
}

/* Each 8-bit channel level in linear light, 0 to 65535: 65535 * (v/255)^2.3.
 * Windows XP's standard smoothing blends text edges in this space, in
 * sixteenths of coverage, which is why its greys for black on white sit
 * high (text.exe "lines levels": 77 104 124 ... 240, 255 * (k/16)^(1/2.3)
 * within one level). Generated with
 * python3 -c "print([round(65535*(v/255)**2.3) for v in range(256)])". */
static const unsigned short k_linear[256] = {
    0, 0, 1, 2, 5, 8, 12, 17, 23, 30, 38, 47,
    58, 70, 83, 97, 112, 129, 147, 167, 188, 210, 234, 259,
    286, 314, 343, 375, 407, 442, 477, 515, 554, 594, 637, 680,
    726, 773, 822, 873, 925, 979, 1035, 1092, 1152, 1213, 1276, 1340,
    1407, 1475, 1545, 1617, 1691, 1767, 1845, 1924, 2006, 2089, 2174, 2261,
    2351, 2442, 2535, 2630, 2727, 2826, 2927, 3030, 3135, 3242, 3351, 3462,
    3575, 3690, 3808, 3927, 4049, 4172, 4298, 4426, 4556, 4688, 4822, 4958,
    5097, 5237, 5380, 5525, 5672, 5821, 5973, 6127, 6283, 6441, 6601, 6764,
    6929, 7096, 7265, 7437, 7611, 7787, 7965, 8146, 8329, 8515, 8702, 8892,
    9085, 9279, 9476, 9676, 9877, 10081, 10288, 10496, 10707, 10921, 11137, 11355,
    11576, 11799, 12024, 12252, 12482, 12715, 12950, 13188, 13428, 13671, 13916, 14163,
    14413, 14665, 14920, 15177, 15437, 15700, 15964, 16232, 16502, 16774, 17049, 17326,
    17606, 17889, 18174, 18461, 18751, 19044, 19339, 19637, 19938, 20240, 20546, 20854,
    21165, 21478, 21794, 22113, 22434, 22758, 23084, 23413, 23745, 24079, 24416, 24756,
    25098, 25443, 25791, 26141, 26494, 26850, 27208, 27569, 27933, 28299, 28668, 29040,
    29414, 29791, 30171, 30554, 30939, 31328, 31718, 32112, 32508, 32907, 33309, 33714,
    34121, 34531, 34944, 35360, 35778, 36200, 36624, 37050, 37480, 37912, 38348, 38786,
    39227, 39670, 40117, 40566, 41018, 41473, 41931, 42392, 42855, 43322, 43791, 44263,
    44738, 45216, 45696, 46180, 46666, 47156, 47648, 48143, 48641, 49142, 49646, 50152,
    50662, 51174, 51690, 52208, 52729, 53254, 53781, 54311, 54844, 55380, 55919, 56461,
    57005, 57553, 58104, 58658, 59214, 59774, 60337, 60902, 61471, 62043, 62617, 63195,
    63775, 64359, 64945, 65535,
};

/* The level whose linear light is nearest `linear`. */
static u32 from_linear(u32 linear) {
    u32 low = 0;
    u32 high = 255;
    while (low < high) {
        const u32 middle = (low + high) / 2u;
        if (k_linear[middle] < linear) {
            low = middle + 1u;
        } else {
            high = middle;
        }
    }
    if (low > 0u && linear - k_linear[low - 1u] < k_linear[low] - linear) {
        low--;
    }
    return low;
}

/* The text color over the pixel by coverage, 0 to 255: in sixteenths, as
 * XP's standard smoothing has it, mixed in linear light. On a monochrome
 * bitmap, set where the coverage is at least half. */
static void blend(const dc *d, const sxw_target *t, const sxw_region *clip, int x, int y, u32 color,
                  unsigned coverage) {
    if (coverage == 0u || !sxw_raster_visible(t, clip, x, y)) {
        return;
    }
    u32 *p = &t->pixels[y * t->stride + x];
    if (coverage >= 255u || is_mono(d)) {
        if (coverage >= 128u) {
            *p = color;
        }
        return;
    }
    const u32 k = coverage * 16u / 255u; /* rounded down, as GGO_GRAY4_BITMAP */
    if (k == 0u) {
        return;
    }
    if (k >= 16u) {
        *p = color;
        return;
    }
    u32 out = 0;
    for (int shift = 0; shift < 24; shift += 8) {
        const u32 under = k_linear[(*p >> shift) & 0xffu];
        const u32 over = k_linear[(color >> shift) & 0xffu];
        out |= from_linear((under * (16u - k) + over * k + 8u) / 16u) << shift;
    }
    *p = out;
}

#define GLYPH_BUFFER (256u * 1024u)

/* One glyph of a host font at the pen, its baseline at y. */
static void draw_host_glyph(const dc *d, const sxw_target *t, const sxw_region *clip, const font *f, int pen,
                            int baseline, unsigned int c, u32 color) {
    static unsigned char *buffer;
    static sxw_font_glyph g;
    if (buffer == 0) {
        buffer = SxwHeapAllocate(GLYPH_BUFFER, 0);
        if (buffer == 0) {
            return;
        }
    }
    u32 arguments[5] = {f->host, c, (u32)&g, (u32)buffer, GLYPH_BUFFER};
    if (SxwService(SXW_SVC_FONT_GLYPH, arguments) != 0u || g.size == 0u || g.size > GLYPH_BUFFER) {
        return;
    }
    for (u32 row = 0; row < g.height; ++row) {
        for (u32 column = 0; column < g.width; ++column) {
            blend(d, t, clip, pen + g.left + (int)column, baseline - g.top + (int)row, color,
                  buffer[row * g.width + column]);
        }
    }
}

/* A horizontal band of the text color: underline and strike-out. */
static void band(const dc *d, const sxw_target *t, const sxw_region *clip, int x0, int x1, int top, int rows,
                 u32 color) {
    for (int y = top; y < top + rows; ++y) {
        for (int x = x0; x < x1; ++x) {
            blend(d, t, clip, x, y, color, 255u);
        }
    }
}

BOOL WINAPI ExtTextOutW(HDC hdc, int x, int y, UINT options, const RECT *lprect, LPCWSTR lpString, UINT c,
                        const INT *lpDx) {
    dc *d = dc_of(hdc);
    if (d == 0 || (c > 0u && lpString == NULL)) {
        return FALSE;
    }
    font *f = selected_font(d);
    sxw_target t = target_of(d);
    /* The pen's advances: the font's, or the caller's. */
    int width = 0;
    for (UINT i = 0; i < c; ++i) {
        width += lpDx != NULL ? lpDx[i] : advance_of(f, lpString[i]);
    }
    const int height = font_height(f);
    const int ascent = font_ascent(f);
    if ((d->text_align & TA_UPDATECP) != 0u) {
        x = d->cx;
        y = d->cy;
    }
    /* The reference point: left, right or centre; top, baseline or bottom. */
    int left = x;
    const UINT horizontal = d->text_align & 6u;
    if (horizontal == TA_RIGHT) {
        left = x - width;
    } else if (horizontal == TA_CENTER) {
        /* An odd width leaves its spare pixel left of the point, as on XP. */
        left = x - (width + 1) / 2;
    }
    int top = y;
    const UINT vertical = d->text_align & 24u;
    if (vertical == TA_BASELINE) {
        top = y - ascent;
    } else if (vertical == TA_BOTTOM) {
        top = y - height;
    }
    if ((d->text_align & TA_UPDATECP) != 0u) {
        d->cx = horizontal == TA_RIGHT ? left : horizontal == TA_CENTER ? d->cx : left + width;
    }
    /* What may be drawn on: the visible region, inside the rectangle when
     * the text is clipped to it. */
    sxw_region clip = d->visible;
    if (lprect != NULL && (options & ETO_CLIPPED) != 0u) {
        sxw_rect r = {d->x + lprect->left, d->y + lprect->top, d->x + lprect->right, d->y + lprect->bottom};
        sxw_region_intersect_rect(&clip, &r);
    }
    if (lprect != NULL && (options & ETO_OPAQUE) != 0u) {
        sxw_rect r = {d->x + lprect->left, d->y + lprect->top, d->x + lprect->right, d->y + lprect->bottom};
        sxw_raster_fill(&t, &d->visible, &r, 0xf0u, device_color(d, d->back_color));
        touched(d, &r);
    }
    sxw_rect box = {d->x + left, d->y + top, d->x + left + width, d->y + top + height};
    if (d->back_mode == OPAQUE && c > 0u) {
        sxw_raster_fill(&t, &clip, &box, 0xf0u, device_color(d, d->back_color));
    }
    const u32 color = device_color(d, d->text_color);
    int pen = d->x + left;
    const int baseline = d->y + top + ascent;
    for (UINT i = 0; i < c; ++i) {
        if (f->host != 0) {
            draw_host_glyph(d, &t, &clip, f, pen, baseline, lpString[i], color);
        } else {
            const struct sx_noto_glyph *g = sx_noto_glyph(lpString[i]);
            for (int pass = 0; pass <= f->bold; ++pass) {
                sxw_raster_glyph(&t, &clip, pen + g->left + pass, baseline - g->top, kNotoCoverage + g->offset,
                                 g->width, g->rows, color);
            }
        }
        pen += lpDx != NULL ? lpDx[i] : advance_of(f, lpString[i]);
    }
    /* Underline below the baseline, strike-out above it, as wide as the
     * text, from the font's own positions. */
    if (f->host != 0 && c > 0u) {
        const sxw_font_metrics *m = &f->metrics;
        if (f->logical.lfUnderline != 0) {
            band(d, &t, &clip, d->x + left, d->x + left + width, baseline - m->underscore_position,
                 m->underscore_size > 0 ? m->underscore_size : 1, color);
        }
        if (f->logical.lfStrikeOut != 0) {
            band(d, &t, &clip, d->x + left, d->x + left + width, baseline - m->strikeout_position,
                 m->strikeout_size > 0 ? m->strikeout_size : 1, color);
        }
    }
    touched(d, &box);
    return TRUE;
}

BOOL WINAPI ExtTextOutA(HDC hdc, int x, int y, UINT options, const RECT *lprect, LPCSTR lpString, UINT c,
                        const INT *lpDx) {
    static WCHAR wide[ANSI_MAX];
    if (c > 0u && lpString == NULL) {
        return FALSE;
    }
    const int n = widen(lpString, (int)c, wide, ANSI_MAX);
    return ExtTextOutW(hdc, x, y, options, lprect, wide, (UINT)n, lpDx);
}

/* The reference point at the text's top left, as TextOutW has it, under the
 * device context's alignment. */
BOOL WINAPI TextOutW(HDC hdc, int x, int y, LPCWSTR lpString, int c) {
    if (c < 0) {
        return FALSE;
    }
    return ExtTextOutW(hdc, x, y, 0, NULL, lpString, (UINT)c, NULL);
}

/* ---- Bitmaps and memory device contexts --------------------------------------- */

static HBITMAP new_bitmap(int width, int height, int mono) {
    if (!g_brushes[0].used) {
        init_stock();
    }
    if (width <= 0 || height <= 0 || width > 0x4000 || height > 0x4000) {
        return NULL;
    }
    for (u32 i = 1; i < MAX_BITMAPS; ++i) {
        bitmap *b = &g_bitmaps[i];
        if (!b->used) {
            unsigned int *pixels = SxwHeapAllocate((u32)width * (u32)height * 4u, 1);
            if (pixels == 0) {
                return NULL;
            }
            b->used = 1;
            b->stock = 0;
            b->mono = mono;
            b->selected = 0;
            b->deleted = 0;
            b->surface.pixels = pixels;
            b->surface.stride = width;
            b->surface.width = width;
            b->surface.height = height;
            sxw_region_clear(&b->surface.dirty);
            return (HBITMAP)make_handle(OBJ_BITMAP, i);
        }
    }
    return NULL;
}

/* A bitmap from bits: one plane of one bit is monochrome, the rows a
 * multiple of 16 bits, a set bit white; 32 bits a pixel is the screen's
 * format. Any other depth makes a colour bitmap of the size, left black. */
HBITMAP WINAPI CreateBitmap(int nWidth, int nHeight, UINT nPlanes, UINT nBitCount, const void *lpBits) {
    const int mono = nPlanes * nBitCount == 1u;
    HBITMAP h = new_bitmap(nWidth, nHeight, mono);
    bitmap *b = bitmap_of(h);
    if (b == 0 || lpBits == NULL) {
        return h;
    }
    const unsigned char *bits = lpBits;
    if (mono) {
        const int stride = ((nWidth + 15) / 16) * 2;
        for (int y = 0; y < nHeight; ++y) {
            for (int x = 0; x < nWidth; ++x) {
                const int set = (bits[y * stride + x / 8] >> (7 - (x & 7))) & 1;
                b->surface.pixels[y * b->surface.stride + x] = set ? WHITE : 0u;
            }
        }
    } else if (nBitCount == 32u && nPlanes == 1u) {
        for (int y = 0; y < nHeight; ++y) {
            for (int x = 0; x < nWidth; ++x) {
                const unsigned char *p = bits + 4 * (y * nWidth + x);
                b->surface.pixels[y * b->surface.stride + x] = ((u32)p[2] << 16) | ((u32)p[1] << 8) | p[0];
            }
        }
    }
    return h;
}

/* A memory device context starts with the stock 1x1 monochrome bitmap. */
HDC WINAPI CreateCompatibleDC(HDC hdc) {
    (void)hdc;
    if (!g_brushes[0].used) {
        init_stock();
    }
    const sxw_rect one = {0, 0, 1, 1};
    sxw_region visible;
    sxw_region_set(&visible, &one);
    HDC h = (HDC)SxwCreateSurfaceDC(&g_bitmaps[0].surface, 0, 0, &visible);
    dc *d = dc_of(h);
    if (d != 0) {
        d->memory = 1;
        d->bitmap = (HBITMAP)make_handle(OBJ_BITMAP, 0);
    }
    return h;
}

BOOL WINAPI DeleteDC(HDC hdc) {
    dc *d = dc_of(hdc);
    if (d == 0) {
        return FALSE;
    }
    if (d->memory) {
        deselect(bitmap_held(d->bitmap));
    }
    d->used = 0;
    return TRUE;
}

/* Monochrome when made for a memory device context holding a monochrome
 * bitmap (its first one, a fresh one's), else the screen's format; an empty
 * size gives a 1x1 monochrome bitmap. */
HBITMAP WINAPI CreateCompatibleBitmap(HDC hdc, int cx, int cy) {
    dc *d = dc_of(hdc);
    if (d == 0) {
        return NULL;
    }
    if (cx == 0 || cy == 0) {
        return new_bitmap(1, 1, 1);
    }
    return new_bitmap(cx, cy, is_mono(d));
}

/* What LoadBitmap makes of a DIB resource: a monochrome bitmap for a black
 * and white one, else one in the screen's format. */
__declspec(dllexport) HBITMAP WINAPI SxwCreateBitmapFromDIB(const void *data, u32 size) {
    sxw_dib dib;
    if (sxw_dib_parse(data, size, &dib) != 0) {
        return NULL;
    }
    HBITMAP h = new_bitmap(dib.width, dib.height, sxw_dib_monochrome(&dib));
    bitmap *b = bitmap_of(h);
    if (b == 0) {
        return NULL;
    }
    for (int y = 0; y < dib.height; ++y) {
        sxw_dib_row(&dib, y, b->surface.pixels + y * b->surface.stride);
    }
    return h;
}

int WINAPI GetObjectA(HANDLE h, int c, LPVOID pv) {
    const bitmap *b = bitmap_of(h);
    if (b != 0) {
        if (pv == NULL) {
            return (int)sizeof(BITMAP);
        }
        if (c < (int)sizeof(BITMAP)) {
            return 0;
        }
        BITMAP *out = pv;
        out->bmType = 0;
        out->bmWidth = b->surface.width;
        out->bmHeight = b->surface.height;
        out->bmWidthBytes = b->mono ? ((b->surface.width + 15) / 16) * 2 : b->surface.width * 4;
        out->bmPlanes = 1;
        out->bmBitsPixel = b->mono ? 1 : 32;
        out->bmBits = NULL;
        return (int)sizeof(BITMAP);
    }
    const brush *br = brush_of(h);
    if (br != 0) {
        /* LOGBRUSH: style, colour, hatch */
        if (pv == NULL) {
            return 12;
        }
        if (c < 12) {
            return 0;
        }
        DWORD *out = pv;
        out[0] = br->null ? 1u /* BS_NULL */ : 0u /* BS_SOLID */;
        out[1] = to_colorref(br->color);
        out[2] = 0;
        return 12;
    }
    return 0;
}

HFONT WINAPI CreateFontIndirectW(const LOGFONTW *lplf) {
    if (lplf == NULL) {
        return NULL;
    }
    return CreateFontW(lplf->lfHeight, lplf->lfWidth, lplf->lfEscapement, lplf->lfOrientation, lplf->lfWeight,
                       lplf->lfItalic, lplf->lfUnderline, lplf->lfStrikeOut, lplf->lfCharSet, lplf->lfOutPrecision,
                       lplf->lfClipPrecision, lplf->lfQuality, lplf->lfPitchAndFamily, lplf->lfFaceName);
}

/* GetObjectA's answer, and a font's LOGFONTW. Every stock font is the System
 * font here, so DEFAULT_GUI_FONT answers System's LOGFONTW, not Windows XP's
 * MS Shell Dlg at -11. */
int WINAPI GetObjectW(HANDLE h, int c, LPVOID pv) {
    const font *f = font_of(h);
    if (f == 0) {
        return GetObjectA(h, c, pv);
    }
    if (pv == NULL) {
        return (int)sizeof(LOGFONTW);
    }
    int n = c < (int)sizeof(LOGFONTW) ? c : (int)sizeof(LOGFONTW);
    const unsigned char *from = (const unsigned char *)&f->logical;
    for (int i = 0; i < n; ++i) {
        ((unsigned char *)pv)[i] = from[i];
    }
    return n;
}

/* The device context's origin on its surface: (0, 0) for a bitmap, the
 * window's client origin on its top-level window for a window. */
BOOL WINAPI GetDCOrgEx(HDC hdc, LPPOINT lppt) {
    dc *d = dc_of(hdc);
    if (d == 0 || lppt == NULL) {
        return FALSE;
    }
    lppt->x = d->x;
    lppt->y = d->y;
    return TRUE;
}

static sxw_convert conversion(const dc *destination, const dc *source) {
    sxw_convert c = {SXW_CONVERT_NONE, 0, 0, 0};
    const int to_mono = is_mono(destination);
    const int from_mono = is_mono(source);
    if (from_mono && !to_mono) {
        c.kind = SXW_CONVERT_MONO_TO_COLOR;
        c.zero = destination->text_color;
        c.one = destination->back_color;
    } else if (!from_mono && to_mono) {
        c.kind = SXW_CONVERT_COLOR_TO_MONO;
        c.key = source->back_color;
    }
    return c;
}

/* Whether a ROP3 reads the source. */
static int reads_source(u32 index) {
    return ((index >> 2) & 0x33u) != (index & 0x33u);
}

BOOL WINAPI StretchBlt(HDC hdcDest, int xDest, int yDest, int wDest, int hDest, HDC hdcSrc, int xSrc, int ySrc,
                       int wSrc, int hSrc, DWORD rop) {
    dc *d = dc_of(hdcDest);
    const u32 index = (rop >> 16) & 0xffu;
    if (d == 0) {
        return FALSE;
    }
    if (!reads_source(index)) {
        return PatBlt(hdcDest, xDest, yDest, wDest, hDest, rop);
    }
    dc *s = dc_of(hdcSrc);
    if (s == 0 || wDest < 0 || hDest < 0 || wSrc < 0 || hSrc < 0) {
        return FALSE;
    }
    const brush *b = brush_of(d->brush);
    const u32 pattern = device_color(d, b != 0 && !b->null ? b->color : 0);
    const sxw_convert c = conversion(d, s);
    sxw_target dt = target_of(d);
    sxw_target st = target_of(s);
    sxw_raster_blit(&dt, &d->visible, d->x + xDest, d->y + yDest, wDest, hDest, &st, s->x + xSrc, s->y + ySrc, wSrc,
                    hSrc, index, pattern, &c);
    sxw_rect r = {d->x + xDest, d->y + yDest, d->x + xDest + wDest, d->y + yDest + hDest};
    touched(d, &r);
    return TRUE;
}

/* A DIB from the caller's header and bits, stretched as StretchBlt
 * stretches. The source rectangle counts its y from the bottom of a
 * bottom-up DIB, as Windows does. The DIB's height, or 0. */
int WINAPI StretchDIBits(HDC hdc, int xDest, int yDest, int DestWidth, int DestHeight, int xSrc, int ySrc,
                         int SrcWidth, int SrcHeight, const void *lpBits, const BITMAPINFO *lpbmi, UINT iUsage,
                         DWORD rop) {
    dc *d = dc_of(hdc);
    sxw_dib dib;
    if (d == 0 || lpBits == NULL || lpbmi == NULL || iUsage != DIB_RGB_COLORS || DestWidth < 0 || DestHeight < 0 ||
        SrcWidth < 0 || SrcHeight < 0 || sxw_dib_parse(lpbmi, 0x7fffffffu, &dib) != 0) {
        return 0;
    }
    dib.bits = lpBits;
    unsigned int *pixels = SxwHeapAllocate((u32)dib.width * (u32)dib.height * 4u, 0);
    if (pixels == 0) {
        return 0;
    }
    for (int y = 0; y < dib.height; ++y) {
        sxw_dib_row(&dib, y, pixels + y * dib.width);
    }
    const int top = dib.top_down ? ySrc : dib.height - ySrc - SrcHeight;
    const u32 index = (rop >> 16) & 0xffu;
    const brush *b = brush_of(d->brush);
    const u32 pattern = device_color(d, b != 0 && !b->null ? b->color : 0);
    const sxw_convert c = {is_mono(d) ? SXW_CONVERT_COLOR_TO_MONO : SXW_CONVERT_NONE, 0, 0, WHITE};
    sxw_target dt = target_of(d);
    sxw_target st = {pixels, dib.width, dib.width, dib.height};
    sxw_raster_blit(&dt, &d->visible, d->x + xDest, d->y + yDest, DestWidth, DestHeight, &st, xSrc, top, SrcWidth,
                    SrcHeight, index, pattern, &c);
    sxw_rect r = {d->x + xDest, d->y + yDest, d->x + xDest + DestWidth, d->y + yDest + DestHeight};
    touched(d, &r);
    SxwHeapFree(pixels);
    return dib.height;
}

/* The pen's outline on the rectangle's edge, the right and bottom ones
 * inside it, and the brush inside; without a pen the brush fills it to one
 * pixel short of the right and bottom, as Windows does. */
BOOL WINAPI Rectangle(HDC hdc, int left, int top, int right, int bottom) {
    dc *d = dc_of(hdc);
    if (d == 0) {
        return FALSE;
    }
    if (right < left) {
        int t = left;
        left = right;
        right = t;
    }
    if (bottom < top) {
        int t = top;
        top = bottom;
        bottom = t;
    }
    const pen *p = pen_of(d->pen);
    const int outline = p != 0 && !p->null;
    const brush *b = brush_of(d->brush);
    sxw_target t = target_of(d);
    if (b != 0 && !b->null) {
        const int inset = outline ? 1 : 0;
        sxw_rect inside = {d->x + left + inset, d->y + top + inset, d->x + right - 1, d->y + bottom - 1};
        if (inside.right > inside.left && inside.bottom > inside.top) {
            sxw_raster_fill(&t, &d->visible, &inside, 0xf0u, device_color(d, b->color));
        }
    }
    if (outline && right - left >= 2 && bottom - top >= 2) {
        const int x0 = d->x + left;
        const int y0 = d->y + top;
        const int x1 = d->x + right - 1;
        const int y1 = d->y + bottom - 1;
        const u32 color = device_color(d, p->color);
        sxw_raster_line(&t, &d->visible, x0, y0, x1, y0, d->rop2, color);
        sxw_raster_line(&t, &d->visible, x1, y0, x1, y1, d->rop2, color);
        sxw_raster_line(&t, &d->visible, x1, y1, x0, y1, d->rop2, color);
        sxw_raster_line(&t, &d->visible, x0, y1, x0, y0, d->rop2, color);
    }
    sxw_rect r = {d->x + left, d->y + top, d->x + right, d->y + bottom};
    touched(d, &r);
    return TRUE;
}

BOOL WINAPI BitBlt(HDC hdc, int x, int y, int cx, int cy, HDC hdcSrc, int x1, int y1, DWORD rop) {
    if (cx < 0) {
        x += cx;
        x1 += cx;
        cx = -cx;
    }
    if (cy < 0) {
        y += cy;
        y1 += cy;
        cy = -cy;
    }
    return StretchBlt(hdc, x, y, cx, cy, hdcSrc, x1, y1, cx, cy, rop);
}
