#include "sxgfx/gfx2d.h"

#include <string.h>

/* For sx_bitmap_create/destroy: the only part of gfx2d that allocates. */
#include <stdlib.h>

static uint32_t sx_blend_bgra8888_over_rgb(uint32_t destination, uint32_t source)
{
    uint32_t alpha = (source >> 24) & 0xffu;
    uint32_t inverse_alpha = 255u - alpha;
    uint32_t source_blue = source & 0xffu;
    uint32_t source_green = (source >> 8) & 0xffu;
    uint32_t source_red = (source >> 16) & 0xffu;
    uint32_t destination_blue = destination & 0xffu;
    uint32_t destination_green = (destination >> 8) & 0xffu;
    uint32_t destination_red = (destination >> 16) & 0xffu;
    uint32_t blended_blue = 0;
    uint32_t blended_green = 0;
    uint32_t blended_red = 0;

    if (alpha == 0u)
    {
        return destination;
    }
    if (alpha == 255u)
    {
        return source & 0x00ffffffu;
    }

    blended_blue = ((source_blue * alpha) + (destination_blue * inverse_alpha) + 127u) / 255u;
    blended_green = ((source_green * alpha) + (destination_green * inverse_alpha) + 127u) / 255u;
    blended_red = ((source_red * alpha) + (destination_red * inverse_alpha) + 127u) / 255u;
    return (blended_red << 16) | (blended_green << 8) | blended_blue;
}

static int sx_clip_rect_to_bitmap(const struct sx_bitmap* bitmap, struct sx_rect* rect)
{
    int right = 0;
    int bottom = 0;

    if (bitmap == 0 || rect == 0 || bitmap->pixels == 0 || rect->width <= 0 || rect->height <= 0)
    {
        return 0;
    }

    if (rect->x < 0)
    {
        rect->width += rect->x;
        rect->x = 0;
    }
    if (rect->y < 0)
    {
        rect->height += rect->y;
        rect->y = 0;
    }
    if (rect->width <= 0 || rect->height <= 0 ||
        rect->x >= (int)bitmap->info.width || rect->y >= (int)bitmap->info.height)
    {
        return 0;
    }

    right = rect->x + rect->width;
    bottom = rect->y + rect->height;
    if (right > (int)bitmap->info.width)
    {
        right = (int)bitmap->info.width;
    }
    if (bottom > (int)bitmap->info.height)
    {
        bottom = (int)bitmap->info.height;
    }
    rect->width = right - rect->x;
    rect->height = bottom - rect->y;
    return rect->width > 0 && rect->height > 0;
}

static struct sx_rect sx_apply_painter_clip(const struct sx_painter* painter, struct sx_rect rect)
{
    if (painter == 0 || !painter->has_clip)
    {
        return rect;
    }
    return sx_rect_intersect(rect, painter->clip_rect);
}

/* Walks the device pieces that survive the clip: the requested rect,
 * intersected with clip_rect and with the region if there is one, and cut to
 * the bitmap.
 *
 * Without a region it yields a single piece, the usual case. With a region it
 * yields one per span, and the spans are DISJOINT -- that is what makes it
 * safe to draw each primitive in pieces: no pixel is touched twice, so
 * antialiased text is not blended again and borders are not doubled. */
struct sx_clip_iter {
    const struct sx_painter* painter;
    struct sx_rect rect;
    int band;
    int span;
    int emitted_plain;
};

static void sx_clip_iter_init(struct sx_clip_iter* it, const struct sx_painter* painter, struct sx_rect device_rect)
{
    it->painter = painter;
    it->rect = sx_apply_painter_clip(painter, device_rect);
    it->band = 0;
    it->span = 0;
    it->emitted_plain = 0;
}

static int sx_clip_iter_next(struct sx_clip_iter* it, struct sx_rect* out)
{
    const struct sx_painter* painter = it->painter;
    const struct sx_region* region;

    if (sx_rect_is_empty(it->rect))
    {
        return 0;
    }

    region = painter->clip_region;
    if (region == 0 || region->band_count <= 0)
    {
        if (it->emitted_plain)
        {
            return 0;
        }
        it->emitted_plain = 1;
        *out = it->rect;
        return sx_clip_rect_to_bitmap(painter->target, out);
    }

    while (it->band < region->band_count)
    {
        const struct sx_region_band* band = &region->bands[it->band];
        const int y0 = band->y0 + painter->clip_region_origin.y;
        const int y1 = band->y1 + painter->clip_region_origin.y;

        if (it->span >= band->span_count)
        {
            it->band += 1;
            it->span = 0;
            continue;
        }
        {
            const struct sx_region_span* span = &band->spans[it->span];
            struct sx_rect piece = sx_rect_make(
                span->x0 + painter->clip_region_origin.x,
                y0,
                span->x1 - span->x0,
                y1 - y0);

            it->span += 1;
            piece = sx_rect_intersect(piece, it->rect);
            if (sx_rect_is_empty(piece))
            {
                continue;
            }
            *out = piece;
            if (!sx_clip_rect_to_bitmap(painter->target, out))
            {
                continue;
            }
            return 1;
        }
    }
    return 0;
}

/* Caller coordinates -> device coordinates. Every public function that takes
 * positions goes through here exactly once; from there inwards the code works
 * in device coordinates, where the clip, the bitmap and the pattern anchoring
 * live. */
static struct sx_rect sx_to_device_rect(const struct sx_painter* painter, struct sx_rect rect)
{
    if (painter == 0)
    {
        return rect;
    }
    return sx_rect_translate(rect, painter->origin.x, painter->origin.y);
}

const uint8_t sx_pattern_checker_50[8] = {
    0xaau, 0x55u, 0xaau, 0x55u, 0xaau, 0x55u, 0xaau, 0x55u,
};

struct sx_brush sx_brush_solid(uint32_t colour)
{
    struct sx_brush brush;
    memset(&brush, 0, sizeof(brush));
    brush.colour = colour;
    brush.back_colour = colour;
    return brush;
}

struct sx_brush sx_brush_pattern(const uint8_t pattern[8], uint32_t colour, uint32_t back_colour)
{
    struct sx_brush brush;
    int index;

    memset(&brush, 0, sizeof(brush));
    brush.colour = colour;
    brush.back_colour = back_colour;
    if (pattern == 0)
    {
        brush.back_colour = colour;
        return brush;
    }
    for (index = 0; index < 8; ++index)
    {
        brush.pattern[index] = pattern[index];
    }
    brush.has_pattern = 1;
    return brush;
}

struct sx_brush sx_brush_pattern_transparent(const uint8_t pattern[8], uint32_t colour)
{
    struct sx_brush brush = sx_brush_pattern(pattern, colour, colour);
    brush.transparent = brush.has_pattern;
    return brush;
}

struct sx_brush sx_brush_with_rop(struct sx_brush brush, int rop)
{
    if (rop >= SX_ROP_COPY && rop <= SX_ROP_INVERT)
    {
        brush.rop = rop;
    }
    return brush;
}

/* Combines source and destination by raster operation. Only the 24 colour bits
 * matter: the destination's high byte is not alpha in BGRX, and letting it into
 * an XOR would paint garbage. */
static uint32_t sx_apply_rop(int rop, uint32_t destination, uint32_t source)
{
    switch (rop)
    {
    case SX_ROP_XOR:
        return (destination ^ source) & 0x00ffffffu;
    case SX_ROP_AND:
        return (destination & source) & 0x00ffffffu;
    case SX_ROP_OR:
        return (destination | source) & 0x00ffffffu;
    case SX_ROP_INVERT:
        return (~destination) & 0x00ffffffu;
    default:
        return source;
    }
}

/* 1 if the pattern paints that cell of the device grid. The AND with 7 keeps
 * the anchoring absolute, and positions arrive already clipped to the bitmap,
 * so they are never negative. */
static int sx_brush_covers(const struct sx_brush* brush, int device_x, int device_y)
{
    return (brush->pattern[device_y & 7] >> (7 - (device_x & 7))) & 1u;
}

struct sx_rect sx_rect_make(int x, int y, int width, int height)
{
    struct sx_rect rect = {x, y, width, height};
    return rect;
}

int sx_rect_is_empty(struct sx_rect rect)
{
    return rect.width <= 0 || rect.height <= 0;
}

int sx_rect_right(struct sx_rect rect)
{
    return rect.x + rect.width;
}

int sx_rect_bottom(struct sx_rect rect)
{
    return rect.y + rect.height;
}

struct sx_rect sx_rect_translate(struct sx_rect rect, int dx, int dy)
{
    rect.x += dx;
    rect.y += dy;
    return rect;
}

struct sx_rect sx_rect_intersect(struct sx_rect left, struct sx_rect right)
{
    struct sx_rect result;
    int x = left.x > right.x ? left.x : right.x;
    int y = left.y > right.y ? left.y : right.y;
    int r = sx_rect_right(left) < sx_rect_right(right) ? sx_rect_right(left) : sx_rect_right(right);
    int b = sx_rect_bottom(left) < sx_rect_bottom(right) ? sx_rect_bottom(left) : sx_rect_bottom(right);

    result.x = x;
    result.y = y;
    result.width = r - x;
    result.height = b - y;
    if (result.width < 0)
    {
        result.width = 0;
    }
    if (result.height < 0)
    {
        result.height = 0;
    }
    return result;
}

struct sx_rect sx_rect_union(struct sx_rect left, struct sx_rect right)
{
    struct sx_rect result;
    int x = left.x < right.x ? left.x : right.x;
    int y = left.y < right.y ? left.y : right.y;
    int r = sx_rect_right(left) > sx_rect_right(right) ? sx_rect_right(left) : sx_rect_right(right);
    int b = sx_rect_bottom(left) > sx_rect_bottom(right) ? sx_rect_bottom(left) : sx_rect_bottom(right);

    if (sx_rect_is_empty(left))
    {
        return right;
    }
    if (sx_rect_is_empty(right))
    {
        return left;
    }

    result.x = x;
    result.y = y;
    result.width = r - x;
    result.height = b - y;
    return result;
}

int sx_rect_contains_point(struct sx_rect rect, int x, int y)
{
    return !sx_rect_is_empty(rect) &&
        x >= rect.x && y >= rect.y &&
        x < sx_rect_right(rect) &&
        y < sx_rect_bottom(rect);
}

void sx_bitmap_wrap(struct sx_bitmap* bitmap, uint32_t* pixels, const struct savanxp_fb_info* info, uint32_t format)
{
    if (bitmap == 0)
    {
        return;
    }

    memset(bitmap, 0, sizeof(*bitmap));
    if (info != 0)
    {
        bitmap->info = *info;
    }
    bitmap->pixels = pixels;
    bitmap->format = format;
}

int sx_bitmap_create(struct sx_bitmap* bitmap, int width, int height, uint32_t format)
{
    struct savanxp_fb_info info;
    uint32_t* pixels;
    size_t bytes;

    if (bitmap == 0 || width <= 0 || height <= 0)
    {
        return 0;
    }
    /* Defensive cap: the product has to fit in size_t without wrapping. 16384
     * per side keeps the area far below any overflow and limits nothing real
     * (the largest screen the system supports is orders of magnitude
     * smaller). */
    if (width > 16384 || height > 16384)
    {
        return 0;
    }

    bytes = (size_t)width * (size_t)height * sizeof(uint32_t);
    pixels = (uint32_t*)malloc(bytes);
    if (pixels == 0)
    {
        return 0;
    }
    memset(pixels, 0, bytes);

    memset(&info, 0, sizeof(info));
    info.width = (uint32_t)width;
    info.height = (uint32_t)height;
    info.pitch = (uint32_t)width * (uint32_t)sizeof(uint32_t);
    info.bpp = 32u;
    info.buffer_size = info.pitch * info.height;

    sx_bitmap_wrap(bitmap, pixels, &info, format);
    bitmap->owns_pixels = 1;
    return 1;
}

void sx_bitmap_destroy(struct sx_bitmap* bitmap)
{
    if (bitmap == 0)
    {
        return;
    }
    /* On a wrapped bitmap this does nothing: the pixels are the caller's. Being
     * able to call it anyway is what lets code not tell the two apart. */
    if (bitmap->owns_pixels && bitmap->pixels != 0)
    {
        free(bitmap->pixels);
    }
    memset(bitmap, 0, sizeof(*bitmap));
}

void sx_painter_init(struct sx_painter* painter, struct sx_bitmap* bitmap)
{
    if (painter == 0)
    {
        return;
    }
    painter->target = bitmap;
    painter->clip_rect = sx_rect_make(0, 0, 0, 0);
    painter->has_clip = 0;
    painter->clip_depth = 0;
    painter->origin.x = 0;
    painter->origin.y = 0;
    painter->origin_depth = 0;
    painter->font = SX_FONT_UI;
    painter->clip_region = 0;
    painter->clip_region_origin.x = 0;
    painter->clip_region_origin.y = 0;
}

int sx_painter_push_origin(struct sx_painter* painter, int dx, int dy)
{
    if (painter == 0 || painter->origin_depth >= SX_PAINTER_ORIGIN_STACK_DEPTH)
    {
        return 0;
    }

    painter->saved_origin[painter->origin_depth] = painter->origin;
    painter->origin_depth += 1;
    painter->origin.x += dx;
    painter->origin.y += dy;
    return 1;
}

void sx_painter_pop_origin(struct sx_painter* painter)
{
    if (painter == 0 || painter->origin_depth <= 0)
    {
        return;
    }
    painter->origin_depth -= 1;
    painter->origin = painter->saved_origin[painter->origin_depth];
}

struct sx_point sx_painter_origin(const struct sx_painter* painter)
{
    struct sx_point origin = {0, 0};
    if (painter != 0)
    {
        origin = painter->origin;
    }
    return origin;
}

struct sx_rect sx_painter_clip_bounds(const struct sx_painter* painter)
{
    struct sx_rect bounds;

    if (painter == 0)
    {
        return sx_rect_make(0, 0, 0, 0);
    }
    if (painter->has_clip)
    {
        bounds = painter->clip_rect;
    }
    else if (painter->target != 0)
    {
        bounds = sx_rect_make(0, 0, (int)painter->target->info.width, (int)painter->target->info.height);
    }
    else
    {
        return sx_rect_make(0, 0, 0, 0);
    }
    return sx_rect_translate(bounds, -painter->origin.x, -painter->origin.y);
}

void sx_painter_clear_clip(struct sx_painter* painter)
{
    if (painter != 0)
    {
        painter->has_clip = 0;
        painter->clip_region = 0;
    }
}

void sx_painter_add_clip_rect(struct sx_painter* painter, struct sx_rect rect)
{
    if (painter == 0)
    {
        return;
    }
    rect = sx_to_device_rect(painter, rect);
    if (!painter->has_clip)
    {
        painter->clip_rect = rect;
        painter->has_clip = 1;
        return;
    }
    painter->clip_rect = sx_rect_intersect(painter->clip_rect, rect);
}

/* Save the current clip and narrow it to the intersection with rect. Returns 0
 * if the stack is full (clip unchanged). Pair every successful push with a pop;
 * widget trees use this to confine each node's drawing to its parent. */
int sx_painter_push_clip(struct sx_painter* painter, struct sx_rect rect)
{
    if (painter == 0 || painter->clip_depth >= SX_PAINTER_CLIP_STACK_DEPTH)
    {
        return 0;
    }

    painter->saved_clip[painter->clip_depth] = painter->clip_rect;
    painter->saved_has_clip[painter->clip_depth] = painter->has_clip;
    painter->saved_clip_region[painter->clip_depth] = painter->clip_region;
    painter->saved_clip_region_origin[painter->clip_depth] = painter->clip_region_origin;
    painter->clip_depth += 1;

    rect = sx_to_device_rect(painter, rect);
    if (painter->has_clip)
    {
        painter->clip_rect = sx_rect_intersect(painter->clip_rect, rect);
    }
    else
    {
        painter->clip_rect = rect;
    }
    painter->has_clip = 1;
    return 1;
}

int sx_painter_push_clip_region(struct sx_painter* painter, const struct sx_region* region)
{
    struct sx_rect bounds;

    if (painter == 0 || painter->clip_depth >= SX_PAINTER_CLIP_STACK_DEPTH)
    {
        return 0;
    }
    if (region == 0 || sx_region_is_empty(region))
    {
        /* Empty region = nothing is drawn. A degenerate clip says so without a
         * separate flag. */
        return sx_painter_push_clip(painter, sx_rect_make(0, 0, 0, 0));
    }

    /* The bounding box goes to clip_rect: that way each primitive's fast reject
     * is still a rect intersection, and bands are walked only when the
     * requested rect really falls inside. */
    bounds = sx_region_bounds(region);
    if (!sx_painter_push_clip(painter, bounds))
    {
        return 0;
    }
    painter->clip_region = region;
    painter->clip_region_origin = painter->origin;
    return 1;
}

void sx_painter_pop_clip(struct sx_painter* painter)
{
    if (painter == 0 || painter->clip_depth <= 0)
    {
        return;
    }
    painter->clip_depth -= 1;
    painter->clip_rect = painter->saved_clip[painter->clip_depth];
    painter->has_clip = painter->saved_has_clip[painter->clip_depth];
    painter->clip_region = painter->saved_clip_region[painter->clip_depth];
    painter->clip_region_origin = painter->saved_clip_region_origin[painter->clip_depth];
}

/* Fills a rect ALREADY in device coordinates. Every solid fill goes through
 * here: the public functions translate by the origin and delegate here, so the
 * origin is applied exactly once. */
static void sx_fill_rect_device(struct sx_painter* painter, struct sx_rect rect, uint32_t colour)
{
    struct sx_clip_iter iter;
    struct sx_rect piece;

    for (sx_clip_iter_init(&iter, painter, rect); sx_clip_iter_next(&iter, &piece); )
    {
        gfx_rect(painter->target->pixels, &painter->target->info, piece.x, piece.y, piece.width, piece.height, colour);
    }
}

/* Like the above but with a brush. The per-pixel walk is paid only when the
 * brush has a pattern; the solid case goes straight to the rect fill. */
static void sx_fill_rect_device_brush(struct sx_painter* painter, struct sx_rect rect, const struct sx_brush* brush)
{
    struct sx_clip_iter iter;
    struct sx_rect piece;
    uint32_t stride;

    /* Solid and COPY: the usual path, a rect fill without touching pixel by
     * pixel. Anything else needs to read the destination. */
    if (!brush->has_pattern && brush->rop == SX_ROP_COPY)
    {
        sx_fill_rect_device(painter, rect, brush->colour);
        return;
    }

    stride = gfx_stride_pixels(&painter->target->info);
    for (sx_clip_iter_init(&iter, painter, rect); sx_clip_iter_next(&iter, &piece); )
    {
        int x;
        int y;

        for (y = piece.y; y < sx_rect_bottom(piece); ++y)
        {
            uint32_t* row = painter->target->pixels + ((size_t)y * stride);
            for (x = piece.x; x < sx_rect_right(piece); ++x)
            {
                uint32_t source;

                if (!brush->has_pattern)
                {
                    source = brush->colour;
                }
                else if (sx_brush_covers(brush, x, y))
                {
                    source = brush->colour;
                }
                else if (brush->transparent)
                {
                    continue;
                }
                else
                {
                    source = brush->back_colour;
                }
                row[x] = sx_apply_rop(brush->rop, row[x], source);
            }
        }
    }
}

/* The four edge strips of the ORIGINAL rect, each clipped on its own. Clipping
 * the rect first and framing the result would trace a spurious border around
 * every fragment of a partial repaint (the cursor's damage footprint over a
 * dialog, for example), which is exactly the leftovers bug. Painting the true
 * edges only touches edge pixels that really exist. */
static void sx_draw_frame_device(struct sx_painter* painter, struct sx_rect rect, const struct sx_brush* brush)
{
    sx_fill_rect_device_brush(painter, sx_rect_make(rect.x, rect.y, rect.width, 1), brush);
    if (rect.height > 1)
    {
        sx_fill_rect_device_brush(painter, sx_rect_make(rect.x, rect.y + rect.height - 1, rect.width, 1), brush);
    }
    if (rect.height > 2)
    {
        sx_fill_rect_device_brush(painter, sx_rect_make(rect.x, rect.y + 1, 1, rect.height - 2), brush);
        if (rect.width > 1)
        {
            sx_fill_rect_device_brush(painter, sx_rect_make(rect.x + rect.width - 1, rect.y + 1, 1, rect.height - 2), brush);
        }
    }
}

void sx_painter_fill(struct sx_painter* painter, uint32_t colour)
{
    if (painter == 0 || painter->target == 0)
    {
        return;
    }
    /* The whole target, not the area at the current origin: it is a clear. */
    sx_fill_rect_device(
        painter,
        sx_rect_make(0, 0, (int)painter->target->info.width, (int)painter->target->info.height),
        colour);
}

void sx_painter_fill_rect(struct sx_painter* painter, struct sx_rect rect, uint32_t colour)
{
    if (painter == 0 || painter->target == 0)
    {
        return;
    }
    sx_fill_rect_device(painter, sx_to_device_rect(painter, rect), colour);
}

void sx_painter_fill_rect_brush(struct sx_painter* painter, struct sx_rect rect, const struct sx_brush* brush)
{
    if (painter == 0 || painter->target == 0 || brush == 0)
    {
        return;
    }
    sx_fill_rect_device_brush(painter, sx_to_device_rect(painter, rect), brush);
}

void sx_painter_set_pixel(struct sx_painter* painter, int x, int y, uint32_t colour)
{
    if (painter == 0 || painter->target == 0)
    {
        return;
    }
    sx_fill_rect_device(painter, sx_rect_make(x + painter->origin.x, y + painter->origin.y, 1, 1), colour);
}

void sx_painter_hline(struct sx_painter* painter, int x, int y, int width, uint32_t colour)
{
    if (painter == 0 || painter->target == 0)
    {
        return;
    }
    sx_fill_rect_device(painter, sx_rect_make(x + painter->origin.x, y + painter->origin.y, width, 1), colour);
}

void sx_painter_vline(struct sx_painter* painter, int x, int y, int height, uint32_t colour)
{
    if (painter == 0 || painter->target == 0)
    {
        return;
    }
    sx_fill_rect_device(painter, sx_rect_make(x + painter->origin.x, y + painter->origin.y, 1, height), colour);
}

void sx_painter_draw_frame(struct sx_painter* painter, struct sx_rect rect, uint32_t colour)
{
    struct sx_brush brush = sx_brush_solid(colour);
    sx_painter_draw_frame_brush(painter, rect, &brush);
}

void sx_painter_draw_frame_brush(struct sx_painter* painter, struct sx_rect rect, const struct sx_brush* brush)
{
    if (painter == 0 || painter->target == 0 || brush == 0 || rect.width <= 0 || rect.height <= 0)
    {
        return;
    }
    sx_draw_frame_device(painter, sx_to_device_rect(painter, rect), brush);
}

/* Copies the piece `dst_rect` (device, already clipped) from the matching
 * area of the source. It is separate so the region clip can call it once per
 * span without duplicating the copy logic. */
static void sx_blit_bitmap_piece(
    struct sx_painter* painter,
    const struct sx_bitmap* source,
    int dst_x,
    int dst_y,
    struct sx_rect dst_rect)
{
    struct sx_rect src_rect;
    size_t row_bytes;
    uint32_t src_stride;
    uint32_t dst_stride;
    int row;

    src_rect = sx_rect_make(dst_rect.x - dst_x, dst_rect.y - dst_y, dst_rect.width, dst_rect.height);
    src_stride = gfx_stride_pixels(&source->info);
    dst_stride = gfx_stride_pixels(&painter->target->info);

    if (source->format == SX_PIXEL_FORMAT_BGRA8888)
    {
        for (row = 0; row < dst_rect.height; ++row)
        {
            uint32_t* destination = painter->target->pixels + ((size_t)(dst_rect.y + row) * dst_stride) + (size_t)dst_rect.x;
            const uint32_t* source_pixels = source->pixels + ((size_t)(src_rect.y + row) * src_stride) + (size_t)src_rect.x;
            int column;

            for (column = 0; column < dst_rect.width; ++column)
            {
                destination[column] = sx_blend_bgra8888_over_rgb(destination[column], source_pixels[column]);
            }
        }
        return;
    }

    row_bytes = (size_t)dst_rect.width * sizeof(uint32_t);

    /* Rows contiguous on both sides (the blit covers the full width of source
     * and destination): the rectangle is one linear run and goes in a single
     * copy instead of one call per row. It pays off when blitting a whole
     * surface -- the background, a full-screen client --, not small dirty
     * rects. */
    if (dst_rect.x == 0 && src_rect.x == 0 &&
        (uint32_t)dst_rect.width == dst_stride && (uint32_t)dst_rect.width == src_stride)
    {
        memcpy(
            painter->target->pixels + ((size_t)dst_rect.y * dst_stride),
            source->pixels + ((size_t)src_rect.y * src_stride),
            row_bytes * (size_t)dst_rect.height);
        return;
    }

    for (row = 0; row < dst_rect.height; ++row)
    {
        memcpy(
            painter->target->pixels + ((size_t)(dst_rect.y + row) * dst_stride) + (size_t)dst_rect.x,
            source->pixels + ((size_t)(src_rect.y + row) * src_stride) + (size_t)src_rect.x,
            row_bytes);
    }
}

void sx_painter_blit_bitmap(struct sx_painter* painter, const struct sx_bitmap* source, int dst_x, int dst_y)
{
    struct sx_clip_iter iter;
    struct sx_rect piece;

    if (painter == 0 || painter->target == 0 || source == 0 || source->pixels == 0)
    {
        return;
    }

    dst_x += painter->origin.x;
    dst_y += painter->origin.y;
    sx_clip_iter_init(
        &iter, painter,
        sx_rect_make(dst_x, dst_y, (int)source->info.width, (int)source->info.height));
    while (sx_clip_iter_next(&iter, &piece))
    {
        sx_blit_bitmap_piece(painter, source, dst_x, dst_y, piece);
    }
}

/* Bilinear sampling: the four neighbouring texels weighted by the fractional
 * part of the position. It works in 8-bit fixed point because this layer is
 * linked by EVERY binary, and it was written when userland had no floating
 * point unless the app was built with -Sse.
 *
 * Neighbours are clamped to source_rect (not to the bitmap): at the edge that
 * repeats the last texel instead of bleeding the next one, which in an icon
 * atlas would pull in pixels from another icon. */
static uint32_t sx_sample_bilinear(
    const struct sx_bitmap* source,
    uint32_t src_stride,
    struct sx_rect source_rect,
    int fx,
    int fy)
{
    const int x0 = fx >> 8;
    const int y0 = fy >> 8;
    const unsigned int wx = (unsigned int)(fx & 0xff);
    const unsigned int wy = (unsigned int)(fy & 0xff);
    const int right = sx_rect_right(source_rect) - 1;
    const int bottom = sx_rect_bottom(source_rect) - 1;
    const int x1 = x0 < right ? x0 + 1 : right;
    const int y1 = y0 < bottom ? y0 + 1 : bottom;
    const uint32_t* row0 = source->pixels + ((size_t)y0 * src_stride);
    const uint32_t* row1 = source->pixels + ((size_t)y1 * src_stride);
    const uint32_t p00 = row0[x0];
    const uint32_t p10 = row0[x1];
    const uint32_t p01 = row1[x0];
    const uint32_t p11 = row1[x1];
    uint32_t result = 0;
    int shift;

    /* Channel by channel, the high byte included: in BGRA that is the alpha,
     * and it has to interpolate like the colour, or an icon's edges get a
     * halo. */
    for (shift = 0; shift <= 24; shift += 8)
    {
        const unsigned int c00 = (p00 >> shift) & 0xffu;
        const unsigned int c10 = (p10 >> shift) & 0xffu;
        const unsigned int c01 = (p01 >> shift) & 0xffu;
        const unsigned int c11 = (p11 >> shift) & 0xffu;
        const unsigned int top = c00 * (256u - wx) + c10 * wx;
        const unsigned int low = c01 * (256u - wx) + c11 * wx;
        const unsigned int value = ((top * (256u - wy)) + (low * wy) + 32768u) >> 16;
        result |= (value > 255u ? 255u : value) << shift;
    }
    return result;
}

void sx_painter_draw_scaled_bitmap(
    struct sx_painter* painter,
    const struct sx_bitmap* source,
    struct sx_rect destination,
    struct sx_rect source_rect,
    int filter)
{
    struct sx_clip_iter iter;
    struct sx_rect target_rect;
    uint32_t src_stride;
    uint32_t dst_stride;
    int x;
    int y;

    if (painter == 0 || painter->target == 0 || source == 0 || source->pixels == 0 ||
        sx_rect_is_empty(destination) || sx_rect_is_empty(source_rect))
    {
        return;
    }

    /* The caller provides the sampling source: clamping it to the source bitmap
     * is the only thing that keeps an out-of-range source_rect from reading
     * past the buffer, because the sample coordinates derive from it and are
     * not validated again in the loop. */
    source_rect = sx_rect_intersect(
        source_rect,
        sx_rect_make(0, 0, (int)source->info.width, (int)source->info.height));
    if (sx_rect_is_empty(source_rect))
    {
        return;
    }

    destination = sx_to_device_rect(painter, destination);
    src_stride = gfx_stride_pixels(&source->info);
    dst_stride = gfx_stride_pixels(&painter->target->info);

    /* Sampling is always computed against the whole `destination`, not the
     * piece: that way each pixel takes exactly the same texel whether it is
     * painted in one go or split into spans by the region. */
    for (sx_clip_iter_init(&iter, painter, destination); sx_clip_iter_next(&iter, &target_rect); )
    {
        for (y = target_rect.y; y < sx_rect_bottom(target_rect); ++y)
        {
            for (x = target_rect.x; x < sx_rect_right(target_rect); ++x)
            {
                uint32_t source_pixel;

                if (filter == SX_SCALE_BILINEAR)
                {
                    /* Texel centre: the half-pixel correction is what keeps
                     * the image from shifting half a pixel when scaled. */
                    const int fx = (int)((((long)(x - destination.x) * 2 + 1) * source_rect.width * 128)
                                          / destination.width) + (source_rect.x << 8) - 128;
                    const int fy = (int)((((long)(y - destination.y) * 2 + 1) * source_rect.height * 128)
                                          / destination.height) + (source_rect.y << 8) - 128;
                    const int min_x = source_rect.x << 8;
                    const int min_y = source_rect.y << 8;
                    const int max_x = (sx_rect_right(source_rect) - 1) << 8;
                    const int max_y = (sx_rect_bottom(source_rect) - 1) << 8;
                    source_pixel = sx_sample_bilinear(
                        source, src_stride, source_rect,
                        fx < min_x ? min_x : (fx > max_x ? max_x : fx),
                        fy < min_y ? min_y : (fy > max_y ? max_y : fy));
                }
                else
                {
                    const int source_y = source_rect.y +
                        (((y - destination.y) * source_rect.height) / destination.height);
                    const int source_x = source_rect.x +
                        (((x - destination.x) * source_rect.width) / destination.width);
                    source_pixel = source->pixels[((size_t)source_y * src_stride) + (size_t)source_x];
                }

                if (source->format == SX_PIXEL_FORMAT_BGRA8888)
                {
                    uint32_t* target = &painter->target->pixels[((size_t)y * dst_stride) + (size_t)x];
                    *target = sx_blend_bgra8888_over_rgb(*target, source_pixel);
                }
                else
                {
                    painter->target->pixels[((size_t)y * dst_stride) + (size_t)x] = source_pixel;
                }
            }
        }
    }
}

void sx_painter_draw_scaled_bitmap_nearest(
    struct sx_painter* painter,
    const struct sx_bitmap* source,
    struct sx_rect destination,
    struct sx_rect source_rect)
{
    sx_painter_draw_scaled_bitmap(painter, source, destination, source_rect, SX_SCALE_NEAREST);
}

int sx_painter_set_font(struct sx_painter* painter, int font)
{
    int previous;

    if (painter == 0)
    {
        return SX_FONT_UI;
    }
    previous = painter->font;
    if (font == SX_FONT_UI || font == SX_FONT_MONO || font == SX_FONT_UI_TITLE)
    {
        painter->font = font;
    }
    return previous;
}

int sx_painter_font(const struct sx_painter* painter)
{
    return painter == 0 ? SX_FONT_UI : painter->font;
}

int sx_painter_text_width(const struct sx_painter* painter, const char* text)
{
    if (painter != 0 && painter->font == SX_FONT_MONO)
    {
        return gfx_text_width_mono(text);
    }
    if (painter != 0 && painter->font == SX_FONT_UI_TITLE)
    {
        return gfx_text_width_title(text);
    }
    return gfx_text_width(text);
}

int sx_painter_text_height(const struct sx_painter* painter)
{
    if (painter != 0 && painter->font == SX_FONT_MONO)
    {
        return gfx_cell_height();
    }
    if (painter != 0 && painter->font == SX_FONT_UI_TITLE)
    {
        return gfx_text_height_title();
    }
    return gfx_text_height();
}

/* A text blit with an explicit clip, already in device coordinates. It is the
 * only place where the font is chosen: above it, draw_text deals only with the
 * clip and the pieces. */
static void sx_blit_text_clipped(
    struct sx_painter* painter, int x, int y, const char* text, uint32_t colour, struct sx_rect clip)
{
    if (painter->font == SX_FONT_MONO)
    {
        gfx_blit_text_mono_clip(painter->target->pixels, &painter->target->info, x, y, text, colour,
                                 clip.x, clip.y, sx_rect_right(clip), sx_rect_bottom(clip));
        return;
    }
    if (painter->font == SX_FONT_UI_TITLE)
    {
        gfx_blit_text_title_clip(painter->target->pixels, &painter->target->info, x, y, text, colour,
                                 clip.x, clip.y, sx_rect_right(clip), sx_rect_bottom(clip));
        return;
    }
    gfx_blit_text_clip(painter->target->pixels, &painter->target->info, x, y, text, colour,
                        clip.x, clip.y, sx_rect_right(clip), sx_rect_bottom(clip));
}

/* ---- Geometry -----------------------------------------------------------
 *
 * Everything rests on the painter's hline/vline/set_pixel, so the clip, the
 * region and the origin come free and correct: none of these functions touches
 * the bitmap directly. */

void sx_painter_draw_line(struct sx_painter* painter, int x0, int y0, int x1, int y1, uint32_t colour)
{
    int dx;
    int dy;
    int step_x;
    int step_y;
    int error;

    if (painter == 0 || painter->target == 0)
    {
        return;
    }
    /* The straight cases earn their shortcut: they are most of a toolkit's
     * lines, and a rect fill beats going pixel by pixel by far. */
    if (y0 == y1)
    {
        const int left = x0 < x1 ? x0 : x1;
        sx_painter_hline(painter, left, y0, (x0 < x1 ? x1 - x0 : x0 - x1) + 1, colour);
        return;
    }
    if (x0 == x1)
    {
        const int top = y0 < y1 ? y0 : y1;
        sx_painter_vline(painter, x0, top, (y0 < y1 ? y1 - y0 : y0 - y1) + 1, colour);
        return;
    }

    dx = x1 > x0 ? x1 - x0 : x0 - x1;
    dy = y1 > y0 ? y1 - y0 : y0 - y1;
    step_x = x0 < x1 ? 1 : -1;
    step_y = y0 < y1 ? 1 : -1;
    error = dx - dy;

    for (;;)
    {
        int doubled;

        sx_painter_set_pixel(painter, x0, y0, colour);
        if (x0 == x1 && y0 == y1)
        {
            return;
        }
        doubled = error * 2;
        if (doubled > -dy)
        {
            error -= dy;
            x0 += step_x;
        }
        if (doubled < dx)
        {
            error += dx;
            y0 += step_y;
        }
    }
}

void sx_painter_draw_polyline(struct sx_painter* painter, const struct sx_point* points, int count, int closed, uint32_t colour)
{
    int index;

    if (painter == 0 || points == 0 || count < 2)
    {
        return;
    }
    for (index = 0; index + 1 < count; ++index)
    {
        sx_painter_draw_line(painter, points[index].x, points[index].y,
                              points[index + 1].x, points[index + 1].y, colour);
    }
    if (closed)
    {
        sx_painter_draw_line(painter, points[count - 1].x, points[count - 1].y,
                              points[0].x, points[0].y, colour);
    }
}

/* The polygon's crossings with scanline `y`, sorted. Returns how many there
 * were. fill_polygon and sx_region_from_polygon share it: it is the only way to
 * guarantee that the region and the drawing describe exactly the same shape. */
static int sx_polygon_crossings(const struct sx_point* points, int count, int y, int* crossings)
{
    int found = 0;
    int index;
    int sorted;

    for (index = 0; index < count; ++index)
    {
        const struct sx_point* a = &points[index];
        const struct sx_point* b = &points[(index + 1) % count];
        /* The edge is ALWAYS walked top to bottom, however the caller stored
         * it. Otherwise the same diagonal shared by two polygons gives a
         * different x depending on direction, and the two overlap or leave a
         * seam -- exactly what happens when tessellating. */
        const struct sx_point* top_point = a->y < b->y ? a : b;
        const struct sx_point* bottom_point = a->y < b->y ? b : a;

        /* Half-open edge in Y ([top, bottom)) so a shared vertex counts ONCE:
         * counting it twice would open and close the span at the same x and
         * leave holes right at the vertices. */
        if (a->y == b->y || y < top_point->y || y >= bottom_point->y)
        {
            continue;
        }
        /* C division truncates towards zero, which is exactly what is needed:
         * truncation is odd-symmetric (trunc(-u) == -trunc(u)), so a polygon
         * and its mirror cover the same area. Floor and ceiling are NOT -- with
         * either one the mirror comes out with a different area, because the
         * left and right edges round to opposite sides when spans are
         * half-open. */
        crossings[found] = top_point->x +
            ((y - top_point->y) * (bottom_point->x - top_point->x)) /
            (bottom_point->y - top_point->y);
        ++found;
    }

    /* Insertion sort: `found` never exceeds the vertex count, and a toolkit's
     * polygons have a handful. */
    for (sorted = 1; sorted < found; ++sorted)
    {
        const int value = crossings[sorted];
        int slot = sorted - 1;
        while (slot >= 0 && crossings[slot] > value)
        {
            crossings[slot + 1] = crossings[slot];
            --slot;
        }
        crossings[slot + 1] = value;
    }
    return found;
}

/* Vertical extent of the polygon. 0 if the input is unusable. */
static int sx_polygon_y_range(const struct sx_point* points, int count, int* min_y, int* max_y)
{
    int index;

    if (points == 0 || count < 3 || count > SX_POLYGON_MAX_POINTS)
    {
        return 0;
    }
    *min_y = points[0].y;
    *max_y = points[0].y;
    for (index = 1; index < count; ++index)
    {
        if (points[index].y < *min_y) { *min_y = points[index].y; }
        if (points[index].y > *max_y) { *max_y = points[index].y; }
    }
    return 1;
}

void sx_painter_fill_polygon(struct sx_painter* painter, const struct sx_point* points, int count, uint32_t colour)
{
    int crossings[SX_POLYGON_MAX_POINTS];
    int min_y;
    int max_y;
    int y;

    if (painter == 0 || painter->target == 0 ||
        !sx_polygon_y_range(points, count, &min_y, &max_y))
    {
        return;
    }

    for (y = min_y; y <= max_y; ++y)
    {
        const int found = sx_polygon_crossings(points, count, y, crossings);
        int index;

        for (index = 0; index + 1 < found; index += 2)
        {
            const int left = crossings[index];
            const int right = crossings[index + 1];
            if (right > left)
            {
                sx_painter_hline(painter, left, y, right - left, colour);
            }
        }
    }
}

void sx_region_from_polygon(struct sx_region* region, const struct sx_point* points, int count)
{
    int crossings[SX_POLYGON_MAX_POINTS];
    int min_y;
    int max_y;
    int y;

    if (region == 0)
    {
        return;
    }
    sx_region_clear(region);
    if (!sx_polygon_y_range(points, count, &min_y, &max_y))
    {
        return;
    }

    /* Row by row, adding each span as a rect one pixel high. The region itself
     * merges adjacent bands with the same spans, so straight sides do not cost
     * a band per row -- only the slanted parts do. */
    for (y = min_y; y <= max_y; ++y)
    {
        const int found = sx_polygon_crossings(points, count, y, crossings);
        int index;

        for (index = 0; index + 1 < found; index += 2)
        {
            const int left = crossings[index];
            const int right = crossings[index + 1];
            if (right > left)
            {
                sx_region_union_rect(region, sx_rect_make(left, y, right - left, 1));
            }
        }
    }
}

/* Midpoint ellipse, in symmetric quadrants. `fill` chooses between tracing the
 * outline and filling the spans between the mirrored sides. */
static void sx_ellipse_impl(struct sx_painter* painter, struct sx_rect rect, uint32_t colour, int fill)
{
    int a;
    int b;
    int cx;
    int cy;
    int x;
    int y;
    long a2;
    long b2;
    long error;
    int odd_width;
    int odd_height;

    if (painter == 0 || painter->target == 0 || rect.width <= 0 || rect.height <= 0)
    {
        return;
    }
    if (rect.width <= 2 || rect.height <= 2)
    {
        /* Too small for the curve to show: the rect IS the shape. */
        sx_painter_fill_rect(painter, rect, colour);
        return;
    }

    /* With an even side the centre falls between pixels: the radius is rounded
     * down and the opposite side is moved by one pixel (odd_*). */
    a = (rect.width - 1) / 2;
    b = (rect.height - 1) / 2;
    odd_width = (rect.width - 1) % 2;
    odd_height = (rect.height - 1) % 2;
    cx = rect.x + a;
    cy = rect.y + b;
    a2 = (long)a * a;
    b2 = (long)b * b;

    x = 0;
    y = b;
    error = b2 - a2 * b + a2 / 4;
    while (b2 * x <= a2 * y)
    {
        if (fill)
        {
            sx_painter_hline(painter, cx - x, cy - y, 2 * x + 1 + odd_width, colour);
            sx_painter_hline(painter, cx - x, cy + y + odd_height, 2 * x + 1 + odd_width, colour);
        }
        else
        {
            sx_painter_set_pixel(painter, cx - x, cy - y, colour);
            sx_painter_set_pixel(painter, cx + x + odd_width, cy - y, colour);
            sx_painter_set_pixel(painter, cx - x, cy + y + odd_height, colour);
            sx_painter_set_pixel(painter, cx + x + odd_width, cy + y + odd_height, colour);
        }
        if (error < 0)
        {
            error += b2 * (2 * x + 3);
        }
        else
        {
            error += b2 * (2 * x + 3) + a2 * (2 - 2 * y);
            --y;
        }
        ++x;
    }

    x = a;
    y = 0;
    error = a2 - b2 * a + b2 / 4;
    while (a2 * y <= b2 * x)
    {
        if (fill)
        {
            sx_painter_hline(painter, cx - x, cy - y, 2 * x + 1 + odd_width, colour);
            sx_painter_hline(painter, cx - x, cy + y + odd_height, 2 * x + 1 + odd_width, colour);
        }
        else
        {
            sx_painter_set_pixel(painter, cx - x, cy - y, colour);
            sx_painter_set_pixel(painter, cx + x + odd_width, cy - y, colour);
            sx_painter_set_pixel(painter, cx - x, cy + y + odd_height, colour);
            sx_painter_set_pixel(painter, cx + x + odd_width, cy + y + odd_height, colour);
        }
        if (error < 0)
        {
            error += a2 * (2 * y + 3);
        }
        else
        {
            error += a2 * (2 * y + 3) + b2 * (2 - 2 * x);
            --x;
        }
        ++y;
    }
}

void sx_painter_draw_ellipse(struct sx_painter* painter, struct sx_rect rect, uint32_t colour)
{
    sx_ellipse_impl(painter, rect, colour, 0);
}

void sx_painter_fill_ellipse(struct sx_painter* painter, struct sx_rect rect, uint32_t colour)
{
    sx_ellipse_impl(painter, rect, colour, 1);
}

/* Caps the radius at what the rect can hold: more than half the shorter side
 * would make two corners overlap. */
static int sx_round_rect_radius(struct sx_rect rect, int radius)
{
    const int shortest = rect.width < rect.height ? rect.width : rect.height;
    if (radius > shortest / 2)
    {
        radius = shortest / 2;
    }
    return radius < 0 ? 0 : radius;
}

/* How far the corner cuts into row `y` (0 = the topmost row of the arc). */
static int sx_round_rect_inset(int radius, int y)
{
    const int dy = radius - y;
    int dx = 0;
    while ((dx + 1) * (dx + 1) + dy * dy <= radius * radius)
    {
        ++dx;
    }
    return radius - dx;
}

void sx_painter_fill_round_rect(struct sx_painter* painter, struct sx_rect rect, int radius, uint32_t colour)
{
    int y;

    if (painter == 0 || painter->target == 0 || sx_rect_is_empty(rect))
    {
        return;
    }
    radius = sx_round_rect_radius(rect, radius);
    if (radius == 0)
    {
        sx_painter_fill_rect(painter, rect, colour);
        return;
    }

    /* The middle strip is a rect; above and below, each row is shortened by the
     * corner's circle. It is done per row rather than by composing ellipses so
     * no pixel is painted twice, which with a ROP would not be harmless. */
    sx_painter_fill_rect(painter, sx_rect_make(rect.x, rect.y + radius, rect.width, rect.height - 2 * radius), colour);
    for (y = 0; y < radius; ++y)
    {
        const int inset = sx_round_rect_inset(radius, y);
        sx_painter_hline(painter, rect.x + inset, rect.y + y, rect.width - 2 * inset, colour);
        sx_painter_hline(painter, rect.x + inset, rect.y + rect.height - 1 - y, rect.width - 2 * inset, colour);
    }
}

void sx_painter_draw_round_rect(struct sx_painter* painter, struct sx_rect rect, int radius, uint32_t colour)
{
    int y;
    int previous_inset = -1;

    if (painter == 0 || painter->target == 0 || sx_rect_is_empty(rect))
    {
        return;
    }
    radius = sx_round_rect_radius(rect, radius);
    if (radius == 0)
    {
        sx_painter_draw_frame(painter, rect, colour);
        return;
    }

    sx_painter_hline(painter, rect.x + radius, rect.y, rect.width - 2 * radius, colour);
    sx_painter_hline(painter, rect.x + radius, rect.y + rect.height - 1, rect.width - 2 * radius, colour);
    sx_painter_vline(painter, rect.x, rect.y + radius, rect.height - 2 * radius, colour);
    sx_painter_vline(painter, rect.x + rect.width - 1, rect.y + radius, rect.height - 2 * radius, colour);

    /* Corners: each row paints the span from the previous row's inset to this
     * one's. Without that span the outline breaks where the circle moves more
     * than one pixel in x between two rows. */
    for (y = 0; y < radius; ++y)
    {
        const int inset = sx_round_rect_inset(radius, y);
        int span = previous_inset < 0 ? 1 : (previous_inset - inset + 1);

        if (span < 1)
        {
            span = 1;
        }
        sx_painter_hline(painter, rect.x + inset, rect.y + y, span, colour);
        sx_painter_hline(painter, rect.x + rect.width - inset - span, rect.y + y, span, colour);
        sx_painter_hline(painter, rect.x + inset, rect.y + rect.height - 1 - y, span, colour);
        sx_painter_hline(painter, rect.x + rect.width - inset - span, rect.y + rect.height - 1 - y, span, colour);
        previous_inset = inset;
    }
}

void sx_painter_draw_text(struct sx_painter* painter, int x, int y, const char* text, uint32_t colour)
{
    struct sx_rect text_rect;

    if (painter == 0 || painter->target == 0 || text == 0)
    {
        return;
    }
    x += painter->origin.x;
    y += painter->origin.y;
    text_rect = sx_rect_make(x, y, sx_painter_text_width(painter, text), sx_painter_text_height(painter));

    if (painter->clip_region != 0)
    {
        /* One blit per span. The spans are disjoint, so no glyph pixel is
         * blended twice -- exactly what the comment on gfx_blit_text_impl
         * (pixels.c) warns about repainting in fragments. */
        struct sx_clip_iter iter;
        struct sx_rect piece;

        for (sx_clip_iter_init(&iter, painter, text_rect); sx_clip_iter_next(&iter, &piece); )
        {
            sx_blit_text_clipped(painter, x, y, text, colour, piece);
        }
        return;
    }

    if (painter->has_clip)
    {
        if (sx_rect_is_empty(sx_rect_intersect(text_rect, painter->clip_rect)))
        {
            return;
        }
        sx_blit_text_clipped(painter, x, y, text, colour, painter->clip_rect);
        return;
    }

    if (painter->font == SX_FONT_MONO)
    {
        gfx_blit_text_mono(painter->target->pixels, &painter->target->info, x, y, text, colour);
        return;
    }
    if (painter->font == SX_FONT_UI_TITLE)
    {
        gfx_blit_text_title(painter->target->pixels, &painter->target->info, x, y, text, colour);
        return;
    }
    gfx_blit_text(painter->target->pixels, &painter->target->info, x, y, text, colour);
}

/* ---- Banded regions -----------------------------------------------------
 *
 * Canonical invariant, kept by construction in every operation:
 *   - bands sorted by y, disjoint and non-empty;
 *   - within each band, spans sorted, disjoint and NOT adjacent
 *     (two spans that touch are merged);
 *   - consecutive bands that touch and have identical spans are merged.
 * Without it the same shape would have several representations, and comparing
 * or iterating would stop being predictable. */

enum sx_region_op {
    SX_REGION_OP_UNION = 0,
    SX_REGION_OP_SUBTRACT = 1,
    SX_REGION_OP_INTERSECT = 2,
};

void sx_region_clear(struct sx_region* region)
{
    if (region == 0)
    {
        return;
    }
    region->band_count = 0;
    region->overflowed = 0;
}

int sx_region_is_empty(const struct sx_region* region)
{
    return region == 0 || region->band_count <= 0;
}

int sx_region_overflowed(const struct sx_region* region)
{
    return region != 0 && region->overflowed;
}

void sx_region_copy(struct sx_region* destination, const struct sx_region* source)
{
    if (destination == 0)
    {
        return;
    }
    if (source == 0)
    {
        sx_region_clear(destination);
        return;
    }
    *destination = *source;
}

struct sx_rect sx_region_bounds(const struct sx_region* region)
{
    int x0 = 0;
    int x1 = 0;
    int index;

    if (sx_region_is_empty(region))
    {
        return sx_rect_make(0, 0, 0, 0);
    }

    x0 = region->bands[0].spans[0].x0;
    x1 = region->bands[0].spans[region->bands[0].span_count - 1].x1;
    for (index = 1; index < region->band_count; ++index)
    {
        const struct sx_region_band* band = &region->bands[index];
        if (band->spans[0].x0 < x0)
        {
            x0 = band->spans[0].x0;
        }
        if (band->spans[band->span_count - 1].x1 > x1)
        {
            x1 = band->spans[band->span_count - 1].x1;
        }
    }
    return sx_rect_make(
        x0,
        region->bands[0].y0,
        x1 - x0,
        region->bands[region->band_count - 1].y1 - region->bands[0].y0);
}

size_t sx_region_rect_count(const struct sx_region* region)
{
    size_t total = 0;
    int index;

    if (sx_region_is_empty(region))
    {
        return 0;
    }
    for (index = 0; index < region->band_count; ++index)
    {
        total += (size_t)region->bands[index].span_count;
    }
    return total;
}

/* Walks the bands adding up spans until it passes the requested index. Linear
 * in the number of bands, which is all there is: enumerating the whole region
 * costs O(bands x rects), and with 32 bands as the ceiling that is not worth a
 * stateful iterator. */
int sx_region_rect_at(const struct sx_region* region, size_t index, struct sx_rect* out)
{
    size_t seen = 0;
    int band_index;

    if (out == 0 || sx_region_is_empty(region))
    {
        return 0;
    }

    for (band_index = 0; band_index < region->band_count; ++band_index)
    {
        const struct sx_region_band* band = &region->bands[band_index];
        const size_t span_count = (size_t)band->span_count;

        /* Loop invariant: index >= seen, because the loop only moves on when the
         * index falls beyond this band. The subtraction never goes below
         * zero. */
        if (index < seen + span_count)
        {
            const struct sx_region_span* span = &band->spans[index - seen];
            *out = sx_rect_make(span->x0, band->y0, span->x1 - span->x0, band->y1 - band->y0);
            return 1;
        }
        seen += span_count;
    }
    return 0;
}

int sx_region_contains_point(const struct sx_region* region, int x, int y)
{
    int index;

    if (sx_region_is_empty(region))
    {
        return 0;
    }
    for (index = 0; index < region->band_count; ++index)
    {
        const struct sx_region_band* band = &region->bands[index];
        int span;

        if (y < band->y0)
        {
            return 0; /* bands are sorted: we are past it */
        }
        if (y >= band->y1)
        {
            continue;
        }
        for (span = 0; span < band->span_count; ++span)
        {
            if (x >= band->spans[span].x0 && x < band->spans[span].x1)
            {
                return 1;
            }
        }
        return 0;
    }
    return 0;
}

/* Collapses to the bounding box and marks it. The paths that ran out of bands
 * or spans call it: a marked superset is better than silently losing area. */
static void sx_region_collapse(struct sx_region* region)
{
    struct sx_rect bounds = sx_region_bounds(region);

    region->band_count = 0;
    region->overflowed = 1;
    if (sx_rect_is_empty(bounds))
    {
        return;
    }
    region->band_count = 1;
    region->bands[0].y0 = bounds.y;
    region->bands[0].y1 = sx_rect_bottom(bounds);
    region->bands[0].span_count = 1;
    region->bands[0].spans[0].x0 = bounds.x;
    region->bands[0].spans[0].x1 = sx_rect_right(bounds);
}

/* Adds a span to a SORTED list, merging whatever it touches. Returns 0 if it
 * did not fit. */
static int sx_span_list_add(struct sx_region_span* spans, int* count, int x0, int x1)
{
    int index = 0;
    int insert;

    if (x0 >= x1)
    {
        return 1;
    }

    /* Absorbs everything that overlaps or touches, keeping the hull. */
    while (index < *count)
    {
        if (spans[index].x1 < x0)
        {
            ++index;
            continue;
        }
        if (spans[index].x0 > x1)
        {
            break;
        }
        if (spans[index].x0 < x0)
        {
            x0 = spans[index].x0;
        }
        if (spans[index].x1 > x1)
        {
            x1 = spans[index].x1;
        }
        {
            int shift;
            for (shift = index; shift + 1 < *count; ++shift)
            {
                spans[shift] = spans[shift + 1];
            }
        }
        *count -= 1;
    }

    if (*count >= SX_REGION_MAX_SPANS_PER_BAND)
    {
        return 0;
    }
    for (insert = *count; insert > index; --insert)
    {
        spans[insert] = spans[insert - 1];
    }
    spans[index].x0 = x0;
    spans[index].x1 = x1;
    *count += 1;
    return 1;
}

/* Applies the operation to one row: the band's spans against the rect's span
 * [rx0, rx1). Writes to `out` and returns 0 if it did not fit. */
static int sx_span_list_apply(
    const struct sx_region_span* spans,
    int count,
    int rx0,
    int rx1,
    int op,
    int rect_covers_row,
    struct sx_region_span* out,
    int* out_count)
{
    int index;

    *out_count = 0;

    if (op == SX_REGION_OP_UNION)
    {
        for (index = 0; index < count; ++index)
        {
            if (!sx_span_list_add(out, out_count, spans[index].x0, spans[index].x1))
            {
                return 0;
            }
        }
        if (rect_covers_row && !sx_span_list_add(out, out_count, rx0, rx1))
        {
            return 0;
        }
        return 1;
    }

    if (op == SX_REGION_OP_INTERSECT)
    {
        if (!rect_covers_row)
        {
            return 1; /* nothing is left outside the rect */
        }
        for (index = 0; index < count; ++index)
        {
            int x0 = spans[index].x0 > rx0 ? spans[index].x0 : rx0;
            int x1 = spans[index].x1 < rx1 ? spans[index].x1 : rx1;
            if (x0 < x1 && !sx_span_list_add(out, out_count, x0, x1))
            {
                return 0;
            }
        }
        return 1;
    }

    /* SUBTRACT: each span may survive whole, split in two or vanish. */
    for (index = 0; index < count; ++index)
    {
        const int x0 = spans[index].x0;
        const int x1 = spans[index].x1;

        if (!rect_covers_row || rx1 <= x0 || rx0 >= x1)
        {
            if (!sx_span_list_add(out, out_count, x0, x1))
            {
                return 0;
            }
            continue;
        }
        if (x0 < rx0 && !sx_span_list_add(out, out_count, x0, rx0))
        {
            return 0;
        }
        if (rx1 < x1 && !sx_span_list_add(out, out_count, rx1, x1))
        {
            return 0;
        }
    }
    return 1;
}

static int sx_bands_equal(const struct sx_region_band* left, const struct sx_region_band* right)
{
    int index;

    if (left->span_count != right->span_count)
    {
        return 0;
    }
    for (index = 0; index < left->span_count; ++index)
    {
        if (left->spans[index].x0 != right->spans[index].x0 ||
            left->spans[index].x1 != right->spans[index].x1)
        {
            return 0;
        }
    }
    return 1;
}

/* Appends a band to the result, merging it with the previous one if they are
 * adjacent and have the same spans (that is what keeps the shape canonical). */
static int sx_region_append_band(
    struct sx_region* out,
    int y0,
    int y1,
    const struct sx_region_span* spans,
    int span_count)
{
    struct sx_region_band* band;

    if (y0 >= y1 || span_count <= 0)
    {
        return 1;
    }

    if (out->band_count > 0)
    {
        struct sx_region_band* previous = &out->bands[out->band_count - 1];
        struct sx_region_band candidate;
        int index;

        candidate.span_count = span_count;
        for (index = 0; index < span_count; ++index)
        {
            candidate.spans[index] = spans[index];
        }
        if (previous->y1 == y0 && sx_bands_equal(previous, &candidate))
        {
            previous->y1 = y1;
            return 1;
        }
    }

    if (out->band_count >= SX_REGION_MAX_BANDS)
    {
        return 0;
    }
    band = &out->bands[out->band_count];
    band->y0 = y0;
    band->y1 = y1;
    band->span_count = span_count;
    {
        int index;
        for (index = 0; index < span_count; ++index)
        {
            band->spans[index] = spans[index];
        }
    }
    out->band_count += 1;
    return 1;
}

/* The single engine behind the three operations against a rect.
 *
 * It rebuilds the region by cutting in Y at every edge that matters (each
 * band's and the rect's), and applies the span operation to each resulting
 * strip. Doing it this way -- rather than one routine per operation -- is what
 * keeps the canonical invariant in one place. */
static void sx_region_apply_rect(struct sx_region* region, struct sx_rect rect, int op)
{
    /* The result is built apart and copied only at the end: the strips read
     * the original region while the new one is built. */
    static struct sx_region scratch;
    const int ry0 = rect.y;
    const int ry1 = sx_rect_bottom(rect);
    const int rx0 = rect.x;
    const int rx1 = sx_rect_right(rect);
    const int rect_empty = sx_rect_is_empty(rect);
    int cut = 0;
    int band_index;
    int ok = 1;

    if (region == 0)
    {
        return;
    }
    if (rect_empty)
    {
        /* Union and subtraction with an empty rect do nothing; intersection does everything. */
        if (op == SX_REGION_OP_INTERSECT)
        {
            sx_region_clear(region);
        }
        return;
    }
    if (op == SX_REGION_OP_UNION && sx_region_is_empty(region))
    {
        sx_region_set_rect(region, rect);
        return;
    }
    if (op != SX_REGION_OP_UNION && sx_region_is_empty(region))
    {
        return;
    }

    scratch.band_count = 0;
    scratch.overflowed = region->overflowed;

    /* Sweep in Y: on each turn, `cut` is the top of what has been emitted and
     * the next relevant edge is computed. */
    cut = region->bands[0].y0;
    if (op == SX_REGION_OP_UNION && ry0 < cut)
    {
        cut = ry0;
    }

    while (ok)
    {
        int next = 0;
        int have_next = 0;
        const struct sx_region_band* covering = 0;
        int rect_covers_row;
        struct sx_region_span out_spans[SX_REGION_MAX_SPANS_PER_BAND];
        int out_count = 0;

        /* The band covering `cut`, and the next edge ahead. */
        for (band_index = 0; band_index < region->band_count; ++band_index)
        {
            const struct sx_region_band* band = &region->bands[band_index];
            if (band->y0 <= cut && cut < band->y1)
            {
                covering = band;
                if (!have_next || band->y1 < next)
                {
                    next = band->y1;
                    have_next = 1;
                }
            }
            else if (band->y0 > cut && (!have_next || band->y0 < next))
            {
                next = band->y0;
                have_next = 1;
            }
        }
        if (op == SX_REGION_OP_UNION)
        {
            if (ry0 > cut && (!have_next || ry0 < next))
            {
                next = ry0;
                have_next = 1;
            }
            if (ry1 > cut && (!have_next || ry1 < next))
            {
                next = ry1;
                have_next = 1;
            }
        }
        else if (covering != 0)
        {
            if (ry0 > cut && ry0 < next)
            {
                next = ry0;
            }
            if (ry1 > cut && ry1 < next)
            {
                next = ry1;
            }
        }
        if (!have_next)
        {
            break;
        }

        rect_covers_row = (cut >= ry0 && cut < ry1);
        if (covering != 0 || (op == SX_REGION_OP_UNION && rect_covers_row))
        {
            ok = sx_span_list_apply(
                covering != 0 ? covering->spans : 0,
                covering != 0 ? covering->span_count : 0,
                rx0, rx1, op, rect_covers_row,
                out_spans, &out_count);
            if (ok)
            {
                ok = sx_region_append_band(&scratch, cut, next, out_spans, out_count);
            }
        }
        cut = next;
    }

    if (!ok)
    {
        /* It did not fit: keep the superset. For union and subtraction the
         * bounding box of what was there already covers it; for union the rect
         * has to be included too. */
        if (op == SX_REGION_OP_UNION)
        {
            struct sx_rect merged = sx_rect_union(sx_region_bounds(region), rect);
            sx_region_set_rect(region, merged);
        }
        else
        {
            sx_region_collapse(region);
        }
        region->overflowed = 1;
        return;
    }

    scratch.overflowed = region->overflowed;
    *region = scratch;
}

void sx_region_set_rect(struct sx_region* region, struct sx_rect rect)
{
    if (region == 0)
    {
        return;
    }
    sx_region_clear(region);
    if (sx_rect_is_empty(rect))
    {
        return;
    }
    region->band_count = 1;
    region->bands[0].y0 = rect.y;
    region->bands[0].y1 = sx_rect_bottom(rect);
    region->bands[0].span_count = 1;
    region->bands[0].spans[0].x0 = rect.x;
    region->bands[0].spans[0].x1 = sx_rect_right(rect);
}

void sx_region_union_rect(struct sx_region* region, struct sx_rect rect)
{
    sx_region_apply_rect(region, rect, SX_REGION_OP_UNION);
}

void sx_region_subtract_rect(struct sx_region* region, struct sx_rect rect)
{
    sx_region_apply_rect(region, rect, SX_REGION_OP_SUBTRACT);
}

void sx_region_intersect_rect(struct sx_region* region, struct sx_rect rect)
{
    sx_region_apply_rect(region, rect, SX_REGION_OP_INTERSECT);
}

void sx_rect_set_clear(struct sx_rect_set* set)
{
    if (set != 0)
    {
        memset(set, 0, sizeof(*set));
    }
}

static void sx_rect_set_compact(struct sx_rect_set* set)
{
    size_t index = 0;

    if (set == 0)
    {
        return;
    }

    while (index < set->count)
    {
        if (!sx_rect_is_empty(set->rects[index]))
        {
            ++index;
            continue;
        }

        while (index + 1 < set->count)
        {
            set->rects[index] = set->rects[index + 1];
            ++index;
        }
        set->count -= 1;
        if (index < SX_RECT_SET_CAPACITY)
        {
            memset(&set->rects[set->count], 0, sizeof(set->rects[set->count]));
        }
        index = 0;
    }
}

static int sx_ranges_overlap(int a_start, int a_end, int b_start, int b_end)
{
    return a_start < b_end && b_start < a_end;
}

static int sx_rects_should_merge(struct sx_rect left, struct sx_rect right)
{
    if (!sx_rect_is_empty(sx_rect_intersect(left, right)))
    {
        return 1;
    }
    if ((sx_rect_right(left) == right.x || sx_rect_right(right) == left.x) &&
        sx_ranges_overlap(left.y, sx_rect_bottom(left), right.y, sx_rect_bottom(right)))
    {
        return 1;
    }
    if ((sx_rect_bottom(left) == right.y || sx_rect_bottom(right) == left.y) &&
        sx_ranges_overlap(left.x, sx_rect_right(left), right.x, sx_rect_right(right)))
    {
        return 1;
    }
    return 0;
}

int sx_rect_set_add(struct sx_rect_set* set, struct sx_rect rect)
{
    size_t index = 0;

    if (set == 0 || sx_rect_is_empty(rect))
    {
        return 0;
    }

    for (index = 0; index < set->count; ++index)
    {
        struct sx_rect existing = set->rects[index];
        if (sx_rect_is_empty(existing))
        {
            continue;
        }

        if (sx_rects_should_merge(existing, rect))
        {
            set->rects[index] = sx_rect_union(existing, rect);
            sx_rect_set_compact(set);
            return 1;
        }
    }

    if (set->count < SX_RECT_SET_CAPACITY)
    {
        set->rects[set->count++] = rect;
        return 1;
    }

    set->rects[0] = sx_rect_union(set->rects[0], rect);
    for (index = 1; index < set->count; ++index)
    {
        set->rects[0] = sx_rect_union(set->rects[0], set->rects[index]);
        memset(&set->rects[index], 0, sizeof(set->rects[index]));
    }
    set->count = 1;
    return 1;
}

/* Append without merging adjacent rects. Subtraction relies on the resulting
 * sub-rects staying distinct (merging them back would re-cover the hole). On
 * overflow the rect is unioned into slot 0: that only ever grows coverage
 * (superset), which is safe for the visible region because the compositor
 * paints back-to-front and any over-painted area is re-covered by the layer in
 * front. It never under-paints. */
static void sx_rect_set_push_raw(struct sx_rect_set* set, struct sx_rect rect)
{
    if (set == 0 || sx_rect_is_empty(rect))
    {
        return;
    }
    if (set->count < SX_RECT_SET_CAPACITY)
    {
        set->rects[set->count++] = rect;
        return;
    }
    set->rects[0] = sx_rect_union(set->rects[0], rect);
}

int sx_rect_set_subtract_rect(struct sx_rect_set* set, struct sx_rect hole)
{
    struct sx_rect_set result;
    size_t index;

    if (set == 0)
    {
        return 0;
    }
    if (sx_rect_is_empty(hole))
    {
        return 1;
    }

    sx_rect_set_clear(&result);
    for (index = 0; index < set->count; ++index)
    {
        struct sx_rect r = set->rects[index];
        struct sx_rect overlap;

        if (sx_rect_is_empty(r))
        {
            continue;
        }

        overlap = sx_rect_intersect(r, hole);
        if (sx_rect_is_empty(overlap))
        {
            sx_rect_set_push_raw(&result, r);
            continue;
        }

        /* Up to four strips of r that fall outside the hole: above, below, and
         * the left/right slivers within the hole's vertical band. */
        if (overlap.y > r.y)
        {
            sx_rect_set_push_raw(&result, sx_rect_make(r.x, r.y, r.width, overlap.y - r.y));
        }
        if (sx_rect_bottom(overlap) < sx_rect_bottom(r))
        {
            sx_rect_set_push_raw(&result, sx_rect_make(r.x, sx_rect_bottom(overlap), r.width, sx_rect_bottom(r) - sx_rect_bottom(overlap)));
        }
        if (overlap.x > r.x)
        {
            sx_rect_set_push_raw(&result, sx_rect_make(r.x, overlap.y, overlap.x - r.x, overlap.height));
        }
        if (sx_rect_right(overlap) < sx_rect_right(r))
        {
            sx_rect_set_push_raw(&result, sx_rect_make(sx_rect_right(overlap), overlap.y, sx_rect_right(r) - sx_rect_right(overlap), overlap.height));
        }
    }

    *set = result;
    return 1;
}

int sx_rect_set_add_translated(struct sx_rect_set* set, struct sx_rect rect, int dx, int dy)
{
    return sx_rect_set_add(set, sx_rect_translate(rect, dx, dy));
}

struct sx_rect sx_rect_set_bounds(const struct sx_rect_set* set)
{
    struct sx_rect bounds = sx_rect_make(0, 0, 0, 0);
    size_t index;

    if (set == 0 || set->count == 0)
    {
        return bounds;
    }

    bounds = set->rects[0];
    for (index = 1; index < set->count; ++index)
    {
        bounds = sx_rect_union(bounds, set->rects[index]);
    }
    return bounds;
}

int sx_rect_set_valid(const struct sx_rect_set* set)
{
    return set != 0 && set->count != 0;
}
