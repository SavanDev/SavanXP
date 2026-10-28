#pragma once

#include <stddef.h>
#include <stdint.h>

#include "abi/savanxp_abi.h"
#include "sxgfx/pixels.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SX_RECT_SET_CAPACITY 64

enum sx_pixel_format {
    SX_PIXEL_FORMAT_INVALID = 0,
    SX_PIXEL_FORMAT_BGRX8888 = 1,
    SX_PIXEL_FORMAT_BGRA8888 = 2,
};

struct sx_point {
    int x;
    int y;
};

struct sx_size {
    int width;
    int height;
};

struct sx_rect {
    int x;
    int y;
    int width;
    int height;
};

struct sx_bitmap {
    uint32_t* pixels;
    struct savanxp_fb_info info;
    uint32_t format;
    /* 1 if sx_bitmap_create allocated the pixels and sx_bitmap_destroy frees
     * them. sx_bitmap_wrap leaves it 0: the buffer is then the caller's and
     * destroying the bitmap does not touch it. */
    int owns_pixels;
};


/* Raster operations, in the style of GDI's ROP2: how the source colour combines
 * with what is already in the destination. COPY is the usual one.
 *
 * XOR is the one worth having: applied twice it restores the exact
 * destination, which is what makes drag frames and rubber bands cheap -- they
 * are drawn and erased without saving what was underneath. */
enum sx_rop {
    SX_ROP_COPY = 0,   /* dst = src   (SRCCOPY) */
    SX_ROP_XOR = 1,    /* dst ^= src  (SRCINVERT) */
    SX_ROP_AND = 2,    /* dst &= src  (SRCAND) */
    SX_ROP_OR = 3,     /* dst |= src  (SRCPAINT) */
    SX_ROP_INVERT = 4, /* dst = ~dst  (DSTINVERT); ignores the colour */
};

/* Sampling filter of the scaled blit. NEAREST is the usual one; BILINEAR
 * interpolates the four neighbouring texels and shows above all when
 * SHRINKING, which is what icons and the wallpaper do. */
enum sx_scale_filter {
    SX_SCALE_NEAREST = 0,
    SX_SCALE_BILINEAR = 1,
};

/* Maximum vertices of a polygon. Fixed, and failing safely: going over drops
 * the drawing instead of writing past the array. */
#define SX_POLYGON_MAX_POINTS 32

#define SX_PAINTER_CLIP_STACK_DEPTH 16
#define SX_PAINTER_ORIGIN_STACK_DEPTH 16

/* A GDI-style brush: a solid colour, or a 1-bit 8x8 pattern. The pattern is
 * anchored to DEVICE coordinates (not to the rect being painted), so two
 * neighbouring controls fall on the same grid instead of shifting with where
 * each one starts -- the rule sxgui's hand-drawn patterns already followed.
 * Bit 7 of each byte is the left column; row i = pattern[i & 7]. */
struct sx_brush {
    uint32_t colour;
    uint32_t back_colour;
    uint8_t pattern[8];
    int has_pattern;
    /* With a pattern: 0 bits are not painted (the background shows through)
     * instead of being filled with back_colour. */
    int transparent;
    /* sx_rop. The constructors leave it at SX_ROP_COPY. */
    int rop;
};

/* 50% pattern: paints where (x + y) is even. It is the dotted focus rect's and
 * the scroll bar trough's. */
extern const uint8_t sx_pattern_checker_50[8];

/* The three baked fonts (tools/font/genfont.py). The choice used to be nailed
 * into WHICH FUNCTION was called -- gfx_blit_text against gfx_blit_text_mono;
 * now it is a value the painter selects, the equivalent of SelectObject(hFont).
 * All three are 1-bit, indexed by codepoint, and text is decoded as UTF-8.
 * See docs/desktop/UI_FONTS.md. */
enum sx_font_id {
    /* MS Sans Serif (Wine's), the 13px bitmap strike. The UI face. */
    SX_FONT_UI = 0,
    /* UniFont 8x16, monospace and 1-bit. The console's face; it also has
     * box drawing and blocks. */
    SX_FONT_MONO = 1,
    /* RetroSans at 17 px, its native pixel grid; the caption variant. */
    SX_FONT_UI_TITLE = 2,
};

struct sx_painter {
    struct sx_bitmap* target;
    /* In device coordinates, like origin: the public functions translate what
     * the caller passes before touching these fields. */
    struct sx_rect clip_rect;
    int has_clip;
    struct sx_rect saved_clip[SX_PAINTER_CLIP_STACK_DEPTH];
    int saved_has_clip[SX_PAINTER_CLIP_STACK_DEPTH];
    int clip_depth;
    /* Offset added to every coordinate coming in through the public API
     * (analogous to GDI's SetViewportOrgEx). Starts at (0,0). */
    struct sx_point origin;
    struct sx_point saved_origin[SX_PAINTER_ORIGIN_STACK_DEPTH];
    int origin_depth;
    /* Clip by region (analogous to SelectClipRgn). BORROWED: the caller owns it
     * and has to keep it alive while it is pushed. It is read in the painter's
     * coordinates at the time of the push; clip_region_origin keeps that origin
     * so it can be translated later. clip_rect is always the clip's bounding
     * box, region included, so the fast reject never needs to look at the
     * region. */
    /* Active font (sx_font_id). Starts at SX_FONT_UI. */
    int font;
    const struct sx_region* clip_region;
    struct sx_point clip_region_origin;
    const struct sx_region* saved_clip_region[SX_PAINTER_CLIP_STACK_DEPTH];
    struct sx_point saved_clip_region_origin[SX_PAINTER_CLIP_STACK_DEPTH];
};

struct sx_rect_set {
    size_t count;
    struct sx_rect rects[SX_RECT_SET_CAPACITY];
};

/* --- Regions -------------------------------------------------------------
 *
 * A region in Y BANDS, the model of GDI's HRGN: sorted, disjoint bands, each
 * with its X spans sorted, disjoint and non-adjacent. Unlike sx_rect_set, the
 * shape is canonical and the operations are EXACT: the union of two rects in
 * an L is the L, not the rectangle that contains them.
 *
 * Why, when sx_rect_set exists: sx_rect_set_add merges by bounding box on any
 * overlap, so it over-covers. For accumulating damage that does not matter
 * (painting too much breaks nothing), but for clipping a layer against what
 * remains visible, every extra pixel is repaint work.
 *
 * OVERFLOW: going over the band or span budget collapses the region to its
 * bounding box and marks it (sx_region_overflowed). It is a SUPERSET -- it
 * never loses area -- like sx_rect_set's overflow. For compositing that is
 * safe, because layers are painted back to front and the layer in front covers
 * what was painted too much; for an arbitrary clip it means painting too much,
 * so whoever needs the exact clip has to check the mark. */
#define SX_REGION_MAX_BANDS 32
#define SX_REGION_MAX_SPANS_PER_BAND 8

struct sx_region_span {
    int x0;
    int x1;
};

struct sx_region_band {
    int y0;
    int y1;
    int span_count;
    struct sx_region_span spans[SX_REGION_MAX_SPANS_PER_BAND];
};

struct sx_region {
    int band_count;
    int overflowed;
    struct sx_region_band bands[SX_REGION_MAX_BANDS];
};

struct sx_rect sx_rect_make(int x, int y, int width, int height);
int sx_rect_is_empty(struct sx_rect rect);
int sx_rect_right(struct sx_rect rect);
int sx_rect_bottom(struct sx_rect rect);
struct sx_rect sx_rect_translate(struct sx_rect rect, int dx, int dy);
struct sx_rect sx_rect_intersect(struct sx_rect left, struct sx_rect right);
struct sx_rect sx_rect_union(struct sx_rect left, struct sx_rect right);
int sx_rect_contains_point(struct sx_rect rect, int x, int y);

void sx_bitmap_wrap(struct sx_bitmap* bitmap, uint32_t* pixels, const struct savanxp_fb_info* info, uint32_t format);
/* Off-screen surface with its own pixels, GDI's CreateCompatibleBitmap: the
 * recipe for compositing without flicker. Returns 0 if there is no memory or
 * the dimensions are unusable. ALWAYS pair with sx_bitmap_destroy; on a bitmap
 * from sx_bitmap_wrap, destroy is a deliberate no-op. */
int sx_bitmap_create(struct sx_bitmap* bitmap, int width, int height, uint32_t format);
void sx_bitmap_destroy(struct sx_bitmap* bitmap);

struct sx_brush sx_brush_solid(uint32_t colour);
struct sx_brush sx_brush_pattern(const uint8_t pattern[8], uint32_t colour, uint32_t back_colour);
struct sx_brush sx_brush_pattern_transparent(const uint8_t pattern[8], uint32_t colour);
/* Returns the same brush with another raster operation. It chains with the
 * constructors: sx_brush_with_rop(sx_brush_solid(c), SX_ROP_XOR). */
struct sx_brush sx_brush_with_rop(struct sx_brush brush, int rop);

void sx_painter_init(struct sx_painter* painter, struct sx_bitmap* bitmap);
void sx_painter_clear_clip(struct sx_painter* painter);
void sx_painter_add_clip_rect(struct sx_painter* painter, struct sx_rect rect);
int sx_painter_push_clip(struct sx_painter* painter, struct sx_rect rect);
/* Like push_clip but against a region. The region is BORROWED: it has to stay
 * alive and unchanged until the pop. It shares the stack with push_clip, so
 * both pair with the same sx_painter_pop_clip. An overflowed region clips by
 * its bounding box (paints too much, never too little). */
int sx_painter_push_clip_region(struct sx_painter* painter, const struct sx_region* region);
void sx_painter_pop_clip(struct sx_painter* painter);

/* Moves the origin by dx/dy relative to the current one. Returns 0 if the
 * stack is full (origin unchanged); pair every successful push with a pop. */
int sx_painter_push_origin(struct sx_painter* painter, int dx, int dy);
void sx_painter_pop_origin(struct sx_painter* painter);
struct sx_point sx_painter_origin(const struct sx_painter* painter);
/* The active clip in LOCAL coordinates (the ones the drawing functions take).
 * Without a clip it returns the whole target. It is the supported way to ask:
 * reading painter->clip_rect by hand gives device coordinates. */
struct sx_rect sx_painter_clip_bounds(const struct sx_painter* painter);

void sx_painter_fill(struct sx_painter* painter, uint32_t colour);
void sx_painter_fill_rect(struct sx_painter* painter, struct sx_rect rect, uint32_t colour);
void sx_painter_draw_frame(struct sx_painter* painter, struct sx_rect rect, uint32_t colour);
void sx_painter_set_pixel(struct sx_painter* painter, int x, int y, uint32_t colour);
void sx_painter_hline(struct sx_painter* painter, int x, int y, int width, uint32_t colour);
void sx_painter_vline(struct sx_painter* painter, int x, int y, int height, uint32_t colour);
void sx_painter_fill_rect_brush(struct sx_painter* painter, struct sx_rect rect, const struct sx_brush* brush);
void sx_painter_draw_frame_brush(struct sx_painter* painter, struct sx_rect rect, const struct sx_brush* brush);
void sx_painter_blit_bitmap(struct sx_painter* painter, const struct sx_bitmap* source, int dst_x, int dst_y);
void sx_painter_draw_scaled_bitmap_nearest(
    struct sx_painter* painter,
    const struct sx_bitmap* source,
    struct sx_rect destination,
    struct sx_rect source_rect);
/* The same, with a choice of filter (sx_scale_filter). The _nearest variant
 * above stays as a shortcut for the NEAREST case. */
void sx_painter_draw_scaled_bitmap(
    struct sx_painter* painter,
    const struct sx_bitmap* source,
    struct sx_rect destination,
    struct sx_rect source_rect,
    int filter);


/* --- Geometria ---------------------------------------------------------- */

/* One-pixel line between two points, Bresenham. Horizontal and vertical ones
 * take the hline/vline path, which is a rect fill. */
void sx_painter_draw_line(struct sx_painter* painter, int x0, int y0, int x1, int y1, uint32_t colour);
/* Ellipse inscribed in the rect (midpoint). draw_ is the one-pixel outline,
 * fill_ the interior. */
void sx_painter_draw_ellipse(struct sx_painter* painter, struct sx_rect rect, uint32_t colour);
void sx_painter_fill_ellipse(struct sx_painter* painter, struct sx_rect rect, uint32_t colour);
/* Rect with rounded corners. A radius <= 0 makes it a plain rect; it is capped
 * at half the shorter side. */
void sx_painter_draw_round_rect(struct sx_painter* painter, struct sx_rect rect, int radius, uint32_t colour);
void sx_painter_fill_round_rect(struct sx_painter* painter, struct sx_rect rect, int radius, uint32_t colour);
/* Scanline-filled polygon, even-odd rule. Up to SX_POLYGON_MAX_POINTS
 * vertices; more than that draws nothing. */
void sx_painter_fill_polygon(struct sx_painter* painter, const struct sx_point* points, int count, uint32_t colour);
/* Outline. closed != 0 closes from the last point back to the first. */
void sx_painter_draw_polyline(struct sx_painter* painter, const struct sx_point* points, int count, int closed, uint32_t colour);
void sx_painter_draw_text(struct sx_painter* painter, int x, int y, const char* text, uint32_t colour);
/* Selects the font and returns the previous one, so it can be restored. It
 * affects draw_text and the metrics of sx_painter_text_width/height. */
int sx_painter_set_font(struct sx_painter* painter, int font);
int sx_painter_font(const struct sx_painter* painter);
/* Metrics of the painter's ACTIVE font. gfx_text_width/height remain those of
 * the plain UI face, for code without a painter at hand. */
int sx_painter_text_width(const struct sx_painter* painter, const char* text);
int sx_painter_text_height(const struct sx_painter* painter);

void sx_region_clear(struct sx_region* region);
void sx_region_set_rect(struct sx_region* region, struct sx_rect rect);
void sx_region_copy(struct sx_region* destination, const struct sx_region* source);
void sx_region_union_rect(struct sx_region* region, struct sx_rect rect);
void sx_region_subtract_rect(struct sx_region* region, struct sx_rect rect);
void sx_region_intersect_rect(struct sx_region* region, struct sx_rect rect);
int sx_region_is_empty(const struct sx_region* region);
int sx_region_overflowed(const struct sx_region* region);
struct sx_rect sx_region_bounds(const struct sx_region* region);
int sx_region_contains_point(const struct sx_region* region, int x, int y);
/* Region from a polygon, GDI's PathToRegion. It uses the same scanline fill
 * as sx_painter_fill_polygon, so the shape matches pixel for pixel what that
 * would draw.
 *
 * MIND the band budget: a slanted edge adds one band per row, so a tall,
 * oblique shape overflows and collapses to its bounding box (a superset, it
 * never loses area -- check sx_region_overflowed). The shapes this is really
 * for -- a window with rounded corners, a cut-out with straight sides -- fit
 * easily, because equal adjacent bands merge. */
void sx_region_from_polygon(struct sx_region* region, const struct sx_point* points, int count);
/* Number of rectangles needed to cover the region. Useful to compare against
 * the count of an equivalent sx_rect_set. */
size_t sx_region_rect_count(const struct sx_region* region);
/* Enumerates the region as disjoint rects, in band order (top to bottom, and
 * left to right within each band). Returns 0 once the index passes
 * sx_region_rect_count. It is the counterpart of sx_rect_set.rects for code
 * that needs a LIST of rects rather than a clip: the compositor's present,
 * which sends the dirty rects to the kernel one at a time. */
int sx_region_rect_at(const struct sx_region* region, size_t index, struct sx_rect* out);

void sx_rect_set_clear(struct sx_rect_set* set);
int sx_rect_set_add(struct sx_rect_set* set, struct sx_rect rect);
int sx_rect_set_subtract_rect(struct sx_rect_set* set, struct sx_rect hole);
int sx_rect_set_add_translated(struct sx_rect_set* set, struct sx_rect rect, int dx, int dy);
struct sx_rect sx_rect_set_bounds(const struct sx_rect_set* set);
int sx_rect_set_valid(const struct sx_rect_set* set);

#ifdef __cplusplus
}
#endif
