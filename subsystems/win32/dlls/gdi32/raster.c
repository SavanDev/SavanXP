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
