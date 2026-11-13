/*
 * raster.c -- See raster.h.
 */
#include "raster.h"

#define RGB_MASK 0x00ffffffu

unsigned int sxw_rop3(unsigned int index, unsigned int pattern, unsigned int source, unsigned int destination) {
    /* Bit i of the index is the result where pattern, source and
     * destination have the bits of i: 4 for the pattern, 2 for the source,
     * 1 for the destination (0xF0, 0xCC and 0xAA as the three operands). */
    unsigned int result = 0;
    for (unsigned int i = 0; i < 8u; ++i) {
        if ((index >> i) & 1u) {
            result |= ((i & 4u) ? pattern : ~pattern) & ((i & 2u) ? source : ~source) &
                      ((i & 1u) ? destination : ~destination);
        }
    }
    return result & RGB_MASK;
}

unsigned int sxw_rop2(int rop2, unsigned int pen, unsigned int destination) {
    /* R2_x - 1 is a truth table: bit (2 * pen + destination). */
    const unsigned int table = (unsigned int)(rop2 - 1) & 15u;
    unsigned int result = 0;
    for (unsigned int i = 0; i < 4u; ++i) {
        if ((table >> i) & 1u) {
            result |= ((i & 2u) ? pen : ~pen) & ((i & 1u) ? destination : ~destination);
        }
    }
    return result & RGB_MASK;
}

static int clip_bounds(const sxw_target *t, sxw_rect *r) {
    const sxw_rect whole = {0, 0, t->width, t->height};
    return sxw_rect_intersect(r, r, &whole);
}

void sxw_raster_fill(const sxw_target *t, const sxw_region *clip, const sxw_rect *rect, unsigned int rop_index,
                     unsigned int pattern) {
    for (int i = 0; i < clip->count; ++i) {
        sxw_rect r;
        if (!sxw_rect_intersect(&r, rect, &clip->rects[i]) || !clip_bounds(t, &r)) {
            continue;
        }
        for (int y = r.top; y < r.bottom; ++y) {
            unsigned int *row = t->pixels + y * t->stride;
            switch (rop_index) {
            case 0xf0u: /* PATCOPY */
                for (int x = r.left; x < r.right; ++x) {
                    row[x] = pattern & RGB_MASK;
                }
                break;
            case 0x00u: /* BLACKNESS */
                for (int x = r.left; x < r.right; ++x) {
                    row[x] = 0;
                }
                break;
            default:
                for (int x = r.left; x < r.right; ++x) {
                    row[x] = sxw_rop3(rop_index, pattern, 0, row[x]);
                }
                break;
            }
        }
    }
}

int sxw_line_points(int x0, int y0, int x1, int y1, sxw_point_fn point, void *context) {
    const int dx = x1 > x0 ? x1 - x0 : x0 - x1;
    const int dy = y1 > y0 ? y1 - y0 : y0 - y1;
    const int sx = x1 > x0 ? 1 : -1;
    const int sy = y1 > y0 ? 1 : -1;
    const int major = dx >= dy ? dx : dy;
    const int minor = dx >= dy ? dy : dx;
    /* The minor coordinate is the nearest one to the true line; a point
     * exactly half-way goes to the smaller coordinate, whichever way the
     * line runs. Checked against Windows XP's own lines in every octant
     * (fixtures/window). */
    const int minor_sign = dx >= dy ? sy : sx;
    for (int i = 0; i < major; ++i) {
        const int step = (2 * i * minor + major - (minor_sign > 0 ? 1 : 0)) / (2 * major);
        if (dx >= dy) {
            point(x0 + sx * i, y0 + sy * step, context);
        } else {
            point(x0 + sx * step, y0 + sy * i, context);
        }
    }
    return major;
}

typedef struct line_context {
    const sxw_target *t;
    const sxw_region *clip;
    int rop2;
    unsigned int color;
} line_context;

static void line_point(int x, int y, void *context) {
    line_context *c = context;
    if (sxw_raster_visible(c->t, c->clip, x, y)) {
        unsigned int *p = c->t->pixels + y * c->t->stride + x;
        *p = sxw_rop2(c->rop2, c->color, *p);
    }
}

void sxw_raster_line(const sxw_target *t, const sxw_region *clip, int x0, int y0, int x1, int y1, int rop2,
                     unsigned int color) {
    line_context c = {t, clip, rop2, color};
    sxw_line_points(x0, y0, x1, y1, line_point, &c);
}

int sxw_raster_visible(const sxw_target *t, const sxw_region *clip, int x, int y) {
    return x >= 0 && y >= 0 && x < t->width && y < t->height && sxw_region_contains(clip, x, y);
}

void sxw_raster_glyph(const sxw_target *t, const sxw_region *clip, int x, int y, const unsigned char *coverage,
                      int width, int rows, unsigned int color) {
    for (int row = 0; row < rows; ++row) {
        for (int column = 0; column < width; ++column) {
            if (coverage[row * width + column] != 0 && sxw_raster_visible(t, clip, x + column, y + row)) {
                t->pixels[(y + row) * t->stride + x + column] = color & RGB_MASK;
            }
        }
    }
}

static unsigned int convert_pixel(const sxw_convert *c, unsigned int p) {
    switch (c->kind) {
    case SXW_CONVERT_MONO_TO_COLOR:
        return (p & RGB_MASK) != 0u ? c->one : c->zero;
    case SXW_CONVERT_COLOR_TO_MONO:
        return (p & RGB_MASK) == (c->key & RGB_MASK) ? RGB_MASK : 0u;
    default:
        return p & RGB_MASK;
    }
}

/* Which source indices make each destination index, [first, end), as
 * Windows XP's StretchBlt spreads them: a Bresenham walk along the longer of
 * the two, starting from an error of 2 * shorter - longer. Checked against
 * Windows XP's 71x96 to 33x62 shrink of every card back and its own
 * shrinks in fixtures/resource. Growing takes the source pixel under each
 * destination pixel's centre, as Windows XP does in that fixture. */
#define STRETCH_MAX 4096

static int g_x_first[STRETCH_MAX];
static int g_x_end[STRETCH_MAX];
static int g_y_first[STRETCH_MAX];
static int g_y_end[STRETCH_MAX];

static void stretch_ranges(int destination, int source, int *first, int *end) {
    if (source > destination) {
        int err = 2 * destination - source;
        int j = 0;
        first[0] = 0;
        for (int s = 0; s < source; ++s) {
            end[j] = s + 1;
            if (err > 0) {
                err += 2 * destination - 2 * source;
                if (j + 1 == destination) {
                    break; /* what is left of the source is dropped */
                }
                first[++j] = s + 1;
            } else {
                err += 2 * destination;
            }
        }
        for (++j; j < destination; ++j) {
            first[j] = end[j] = source - 1;
            ++end[j];
        }
    } else {
        /* Growing: each destination pixel takes the source pixel under its
         * centre. */
        for (int j = 0; j < destination; ++j) {
            first[j] = ((2 * j + 1) * source) / (2 * destination);
            end[j] = first[j] + 1;
        }
    }
}

/* The source pixel for destination offset (i, j), the shrunk ones ANDed. */
static unsigned int source_pixel(const sxw_target *src, int sx, int sy, int i, int j) {
    unsigned int value = RGB_MASK;
    for (int y = sy + g_y_first[j]; y < sy + g_y_end[j]; ++y) {
        for (int x = sx + g_x_first[i]; x < sx + g_x_end[i]; ++x) {
            value &= src->pixels[y * src->stride + x];
        }
    }
    return value;
}

void sxw_raster_blit(const sxw_target *dst, const sxw_region *clip, int dx, int dy, int dw, int dh,
                     const sxw_target *src, int sx, int sy, int sw, int sh, unsigned int rop_index,
                     unsigned int pattern, const sxw_convert *convert) {
    if (dw <= 0 || dh <= 0 || sw <= 0 || sh <= 0) {
        return;
    }
    const int same_size = dw == sw && dh == sh;
    if (same_size) {
        /* The source clipped to its bitmap moves the destination with it. */
        if (sx < 0) {
            dx -= sx;
            dw += sx;
            sx = 0;
        }
        if (sy < 0) {
            dy -= sy;
            dh += sy;
            sy = 0;
        }
        if (sx + dw > src->width) {
            dw = src->width - sx;
        }
        if (sy + dh > src->height) {
            dh = src->height - sy;
        }
        if (dw <= 0 || dh <= 0) {
            return;
        }
        sw = dw;
        sh = dh;
    } else if (sx < 0 || sy < 0 || sx + sw > src->width || sy + sh > src->height || dw > STRETCH_MAX ||
               dh > STRETCH_MAX) {
        return; /* a stretch reads only inside its source */
    } else {
        stretch_ranges(dw, sw, g_x_first, g_x_end);
        stretch_ranges(dh, sh, g_y_first, g_y_end);
    }
    const sxw_rect area = {dx, dy, dx + dw, dy + dh};
    /* Overlap within one bitmap: walk away from the source. */
    const int backwards = src->pixels == dst->pixels && (dy > sy || (dy == sy && dx > sx));
    for (int k = 0; k < clip->count; ++k) {
        const int r = backwards ? clip->count - 1 - k : k;
        sxw_rect part;
        if (!sxw_rect_intersect(&part, &area, &clip->rects[r]) || !clip_bounds(dst, &part)) {
            continue;
        }
        for (int jj = 0; jj < part.bottom - part.top; ++jj) {
            const int y = backwards ? part.bottom - 1 - jj : part.top + jj;
            unsigned int *row = dst->pixels + y * dst->stride;
            for (int ii = 0; ii < part.right - part.left; ++ii) {
                const int x = backwards ? part.right - 1 - ii : part.left + ii;
                const unsigned int s =
                    same_size ? src->pixels[(sy + y - dy) * src->stride + sx + x - dx]
                              : source_pixel(src, sx, sy, x - dx, y - dy);
                const unsigned int converted = convert_pixel(convert, s);
                row[x] = rop_index == 0xccu ? converted : sxw_rop3(rop_index, pattern, converted, row[x]);
            }
        }
    }
}
