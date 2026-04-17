#include "savanxp/libc.h"

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

void sx_painter_init(struct sx_painter* painter, struct sx_bitmap* bitmap)
{
    if (painter == 0)
    {
        return;
    }
    painter->target = bitmap;
    painter->clip_rect = sx_rect_make(0, 0, 0, 0);
    painter->has_clip = 0;
}

void sx_painter_clear_clip(struct sx_painter* painter)
{
    if (painter != 0)
    {
        painter->has_clip = 0;
    }
}

void sx_painter_add_clip_rect(struct sx_painter* painter, struct sx_rect rect)
{
    if (painter == 0)
    {
        return;
    }
    if (!painter->has_clip)
    {
        painter->clip_rect = rect;
        painter->has_clip = 1;
        return;
    }
    painter->clip_rect = sx_rect_intersect(painter->clip_rect, rect);
}

void sx_painter_fill(struct sx_painter* painter, uint32_t colour)
{
    struct sx_rect rect;

    if (painter == 0 || painter->target == 0)
    {
        return;
    }
    rect = sx_rect_make(0, 0, (int)painter->target->info.width, (int)painter->target->info.height);
    sx_painter_fill_rect(painter, rect, colour);
}

void sx_painter_fill_rect(struct sx_painter* painter, struct sx_rect rect, uint32_t colour)
{
    if (painter == 0 || painter->target == 0)
    {
        return;
    }

    rect = sx_apply_painter_clip(painter, rect);
    if (!sx_clip_rect_to_bitmap(painter->target, &rect))
    {
        return;
    }

    gfx_rect(painter->target->pixels, &painter->target->info, rect.x, rect.y, rect.width, rect.height, colour);
}

void sx_painter_draw_frame(struct sx_painter* painter, struct sx_rect rect, uint32_t colour)
{
    if (painter == 0 || painter->target == 0)
    {
        return;
    }

    rect = sx_apply_painter_clip(painter, rect);
    if (!sx_clip_rect_to_bitmap(painter->target, &rect))
    {
        return;
    }

    gfx_frame(painter->target->pixels, &painter->target->info, rect.x, rect.y, rect.width, rect.height, colour);
}

void sx_painter_blit_bitmap(struct sx_painter* painter, const struct sx_bitmap* source, int dst_x, int dst_y)
{
    struct sx_rect src_rect;
    struct sx_rect dst_rect;
    size_t row_bytes;
    uint32_t src_stride;
    uint32_t dst_stride;
    int row;

    if (painter == 0 || painter->target == 0 || source == 0 || source->pixels == 0)
    {
        return;
    }

    src_rect = sx_rect_make(0, 0, (int)source->info.width, (int)source->info.height);
    dst_rect = sx_rect_make(dst_x, dst_y, src_rect.width, src_rect.height);
    dst_rect = sx_apply_painter_clip(painter, dst_rect);
    if (!sx_clip_rect_to_bitmap(painter->target, &dst_rect))
    {
        return;
    }

    src_rect.x += dst_rect.x - dst_x;
    src_rect.y += dst_rect.y - dst_y;
    src_rect.width = dst_rect.width;
    src_rect.height = dst_rect.height;

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
    for (row = 0; row < dst_rect.height; ++row)
    {
        memcpy(
            painter->target->pixels + ((size_t)(dst_rect.y + row) * dst_stride) + (size_t)dst_rect.x,
            source->pixels + ((size_t)(src_rect.y + row) * src_stride) + (size_t)src_rect.x,
            row_bytes);
    }
}

void sx_painter_draw_scaled_bitmap_nearest(
    struct sx_painter* painter,
    const struct sx_bitmap* source,
    struct sx_rect destination,
    struct sx_rect source_rect)
{
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

    target_rect = sx_apply_painter_clip(painter, destination);
    if (!sx_clip_rect_to_bitmap(painter->target, &target_rect))
    {
        return;
    }

    src_stride = gfx_stride_pixels(&source->info);
    dst_stride = gfx_stride_pixels(&painter->target->info);
    for (y = target_rect.y; y < sx_rect_bottom(target_rect); ++y)
    {
        int source_y = source_rect.y +
            (((y - destination.y) * source_rect.height) / destination.height);
        for (x = target_rect.x; x < sx_rect_right(target_rect); ++x)
        {
            int source_x = source_rect.x +
                (((x - destination.x) * source_rect.width) / destination.width);
            uint32_t source_pixel = source->pixels[((size_t)source_y * src_stride) + (size_t)source_x];
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

void sx_painter_draw_text(struct sx_painter* painter, int x, int y, const char* text, uint32_t colour)
{
    if (painter == 0 || painter->target == 0 || text == 0)
    {
        return;
    }
    if (painter->has_clip)
    {
        struct sx_rect text_rect = sx_rect_make(x, y, gfx_text_width(text), gfx_text_height());
        text_rect = sx_rect_intersect(text_rect, painter->clip_rect);
        if (sx_rect_is_empty(text_rect))
        {
            return;
        }
    }
    gfx_blit_text(painter->target->pixels, &painter->target->info, x, y, text, colour);
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

        if (!sx_rect_is_empty(sx_rect_intersect(existing, rect)) ||
            sx_rect_right(existing) == rect.x ||
            sx_rect_right(rect) == existing.x ||
            sx_rect_bottom(existing) == rect.y ||
            sx_rect_bottom(rect) == existing.y)
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
