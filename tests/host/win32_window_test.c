/*
 * win32_window_test.c -- The pure parts of user32 and gdi32 on the build
 * host, with the sanitizers on: regions against a brute-force pixel set,
 * the raster operations against their truth tables, lines against the rule
 * Windows XP's were checked to follow, clipped fills, DIB decoding in both
 * header formats, and BitBlt and StretchBlt with their format conversions.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dib.h"
#include "raster.h"
#include "region.h"

static int g_failures;

#define CHECK(cond)                                                                  \
    do {                                                                             \
        if (!(cond)) {                                                               \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                                            \
        }                                                                            \
    } while (0)

#define SIDE 48

/* A region as a set of pixels, the truth it is compared with. */
typedef struct pixels {
    unsigned char in[SIDE][SIDE];
} pixels;

static void paint(pixels *p, const sxw_rect *r, int value) {
    for (int y = r->top; y < r->bottom; ++y) {
        for (int x = r->left; x < r->right; ++x) {
            if (x >= 0 && y >= 0 && x < SIDE && y < SIDE) {
                p->in[y][x] = (unsigned char)value;
            }
        }
    }
}

static int region_is(const sxw_region *region, const pixels *truth) {
    /* Disjoint rectangles: no pixel twice. */
    for (int y = 0; y < SIDE; ++y) {
        for (int x = 0; x < SIDE; ++x) {
            int hits = 0;
            for (int i = 0; i < region->count; ++i) {
                const sxw_rect *r = &region->rects[i];
                hits += x >= r->left && x < r->right && y >= r->top && y < r->bottom;
            }
            if (hits > 1 || hits != truth->in[y][x]) {
                return 0;
            }
        }
    }
    return 1;
}

static sxw_rect random_rect(void) {
    int x0 = rand() % SIDE;
    int y0 = rand() % SIDE;
    sxw_rect r = {x0, y0, x0 + rand() % (SIDE - x0 + 1), y0 + rand() % (SIDE - y0 + 1)};
    return r;
}

static void test_regions(void) {
    srand(7);
    int overflowed = 0;
    for (int round = 0; round < 2000; ++round) {
        sxw_region region;
        pixels truth;
        memset(&truth, 0, sizeof(truth));
        sxw_region_clear(&region);
        for (int step = 0; step < 8; ++step) {
            sxw_rect r = random_rect();
            const int op = rand() % 3;
            sxw_region before = region;
            if (op == 0) {
                sxw_region_union_rect(&region, &r);
                if (region.overflowed && !before.overflowed) {
                    /* Collapsed to the bounding box: a superset, flagged. */
                    ++overflowed;
                    break;
                }
                paint(&truth, &r, 1);
            } else if (op == 1) {
                sxw_region_subtract_rect(&region, &r);
                if (region.overflowed && !before.overflowed) {
                    ++overflowed;
                    break;
                }
                paint(&truth, &r, 0);
            } else {
                sxw_region_intersect_rect(&region, &r);
                pixels outside = truth;
                memset(&truth, 0, sizeof(truth));
                for (int y = r.top; y < r.bottom; ++y) {
                    for (int x = r.left; x < r.right; ++x) {
                        truth.in[y][x] = outside.in[y][x];
                    }
                }
            }
            CHECK(region_is(&region, &truth));
            for (int i = 0; i < region.count; ++i) {
                CHECK(!sxw_rect_empty(&region.rects[i]));
            }
        }
    }
    (void)overflowed;

    /* The bound: holes punched into a rectangle until a subtraction does
     * not fit leave the region unchanged and flagged (it draws more, never
     * less); a union that does not fit becomes the bounding box. */
    sxw_region holes;
    const sxw_rect big = {0, 0, SIDE, SIDE};
    sxw_region_set(&holes, &big);
    int punched = 0;
    for (int y = 1; y + 2 < SIDE && !holes.overflowed; y += 3) {
        for (int x = 1; x + 2 < SIDE && !holes.overflowed; x += 3) {
            const sxw_region before = holes;
            const sxw_rect hole = {x, y, x + 1, y + 1};
            sxw_region_subtract_rect(&holes, &hole);
            if (holes.overflowed) {
                CHECK(holes.count == before.count);
                CHECK(memcmp(holes.rects, before.rects, sizeof(before.rects)) == 0);
            } else {
                ++punched;
            }
        }
    }
    CHECK(holes.overflowed && punched > 0);
    sxw_region dots;
    sxw_region_clear(&dots);
    for (int i = 0; i < SXW_REGION_MAX + 1; ++i) {
        const sxw_rect dot = {i, i, i + 1, i + 1};
        sxw_region_union_rect(&dots, &dot);
    }
    CHECK(dots.overflowed && dots.count == 1);
    CHECK(dots.rects[0].left == 0 && dots.rects[0].right == SXW_REGION_MAX + 1);

    /* Intersecting two regions, and the bounds. */
    sxw_region a;
    sxw_region b;
    const sxw_rect whole = {0, 0, 40, 40};
    const sxw_rect hole = {10, 10, 30, 30};
    const sxw_rect band = {0, 15, 40, 25};
    sxw_region_set(&a, &whole);
    sxw_region_subtract_rect(&a, &hole);
    sxw_region_set(&b, &band);
    sxw_region_intersect(&a, &b);
    pixels truth;
    memset(&truth, 0, sizeof(truth));
    paint(&truth, &band, 1);
    paint(&truth, &hole, 0);
    CHECK(region_is(&a, &truth));
    const sxw_rect bounds = sxw_region_bounds(&a);
    CHECK(bounds.left == 0 && bounds.top == 15 && bounds.right == 40 && bounds.bottom == 25);
    CHECK(sxw_region_contains(&a, 5, 20) && !sxw_region_contains(&a, 20, 20));
}

static void test_rops(void) {
    /* Each ROP2 is its truth table over every pen and destination bit. */
    for (int rop2 = 1; rop2 <= 16; ++rop2) {
        for (unsigned p = 0; p < 2; ++p) {
            for (unsigned d = 0; d < 2; ++d) {
                const unsigned expected = ((unsigned)(rop2 - 1) >> (2 * p + d)) & 1u;
                const unsigned got = sxw_rop2(rop2, p ? 0xffffffu : 0, d ? 0xffffffu : 0);
                CHECK(got == (expected ? 0xffffffu : 0));
            }
        }
    }
    /* The named ones on mixed colours. */
    CHECK(sxw_rop2(6 /* R2_NOT */, 0x123456, 0x008000) == 0xff7fff);
    CHECK(sxw_rop2(13 /* R2_COPYPEN */, 0x123456, 0x008000) == 0x123456);
    CHECK(sxw_rop2(7 /* R2_XORPEN */, 0x0000ff, 0x0000f0) == 0x00000f);
    CHECK(sxw_rop2(11 /* R2_NOP */, 0x123456, 0xabcdef) == 0xabcdef);
    /* Every ROP3 index over the three canonical operands gives the index. */
    for (unsigned index = 0; index < 256; ++index) {
        CHECK((sxw_rop3(index, 0xf0, 0xcc, 0xaa) & 0xff) == index);
    }
    CHECK(sxw_rop3(0xf0, 0x123456, 0, 0x654321) == 0x123456); /* PATCOPY */
    CHECK(sxw_rop3(0x55, 0, 0, 0x008000) == 0xff7fff);        /* DSTINVERT */
    CHECK(sxw_rop3(0x5a, 0x0000ff, 0, 0x0000f0) == 0x00000f); /* PATINVERT */
    CHECK(sxw_rop3(0xcc, 0, 0x777777, 0) == 0x777777);        /* SRCCOPY */
}

typedef struct collected {
    int count;
    int x[256];
    int y[256];
} collected;

static void collect(int x, int y, void *context) {
    collected *c = context;
    if (c->count < 256) {
        c->x[c->count] = x;
        c->y[c->count] = y;
    }
    ++c->count;
}

static int abs_of(int v) {
    return v < 0 ? -v : v;
}

static void test_lines(void) {
    srand(11);
    for (int round = 0; round < 3000; ++round) {
        const int x0 = rand() % 101 - 50;
        const int y0 = rand() % 101 - 50;
        const int x1 = rand() % 101 - 50;
        const int y1 = rand() % 101 - 50;
        collected c = {0};
        const int n = sxw_line_points(x0, y0, x1, y1, collect, &c);
        const int dx = x1 - x0;
        const int dy = y1 - y0;
        const int major = abs_of(dx) > abs_of(dy) ? abs_of(dx) : abs_of(dy);
        CHECK(n == major && c.count == major);
        for (int i = 0; i < c.count && i < 256; ++i) {
            /* Starts at the start, the end point left out, one pixel a step
             * along the major axis, and the nearest pixel to the true line
             * with a tie going to the smaller coordinate. */
            CHECK(!(c.x[i] == x1 && c.y[i] == y1) || major == 0);
            if (abs_of(dx) >= abs_of(dy)) {
                CHECK(c.x[i] == x0 + (dx > 0 ? i : -i));
                /* 2 * major * (y - y0) against 2 * i * dy, in exact integers */
                const long twice_offset = 2L * major * (c.y[i] - y0) - 2L * i * dy;
                CHECK(twice_offset <= major && twice_offset >= -major);
                if (twice_offset == major || twice_offset == -major) {
                    CHECK(twice_offset < 0); /* the smaller y of the two */
                }
            } else {
                CHECK(c.y[i] == y0 + (dy > 0 ? i : -i));
                const long twice_offset = 2L * major * (c.x[i] - x0) - 2L * i * dx;
                CHECK(twice_offset <= major && twice_offset >= -major);
                if (twice_offset == major || twice_offset == -major) {
                    CHECK(twice_offset < 0);
                }
            }
        }
        CHECK(c.count == 0 || (c.x[0] == x0 && c.y[0] == y0));
    }
}

static void test_fills(void) {
    unsigned int pixels[16 * 16];
    for (int i = 0; i < 256; ++i) {
        pixels[i] = 0x008000;
    }
    sxw_target t = {pixels, 16, 16, 16};
    sxw_region clip;
    const sxw_rect all = {0, 0, 16, 16};
    const sxw_rect child = {4, 4, 8, 8};
    sxw_region_set(&clip, &all);
    sxw_region_subtract_rect(&clip, &child);
    const sxw_rect fill = {-5, 2, 30, 10};
    sxw_raster_fill(&t, &clip, &fill, 0xf0, 0x0000ff);
    for (int y = 0; y < 16; ++y) {
        for (int x = 0; x < 16; ++x) {
            const int inside = y >= 2 && y < 10 && !(x >= 4 && x < 8 && y >= 4 && y < 8);
            CHECK(pixels[y * 16 + x] == (inside ? 0x0000ffu : 0x008000u));
        }
    }
    sxw_raster_fill(&t, &clip, &all, 0x55, 0); /* DSTINVERT */
    CHECK(pixels[0] == 0xff7fff && pixels[2 * 16] == 0xffff00 && pixels[5 * 16 + 5] == 0x008000);
    const unsigned char glyph[6] = {0xff, 0, 0xff, 0xff, 0xff, 0};
    sxw_raster_glyph(&t, &clip, 3, 3, glyph, 2, 3, 0x123456);
    CHECK(pixels[3 * 16 + 3] == 0x123456 && pixels[3 * 16 + 4] != 0x123456);
    CHECK(pixels[4 * 16 + 3] == 0x123456 && pixels[4 * 16 + 4] != 0x123456); /* the clipped corner */
}

/* ---- DIBs ------------------------------------------------------------------ */

static unsigned char g_dib[4096];

static unsigned put16(unsigned at, unsigned v) {
    g_dib[at] = (unsigned char)v;
    g_dib[at + 1] = (unsigned char)(v >> 8);
    return at + 2;
}

static unsigned put32(unsigned at, unsigned v) {
    return put16(put16(at, v & 0xffffu), v >> 16);
}

/* A DIB whose pixel (x, y), counted from the top, is pattern(x, y) as an
 * index (or a colour at 24 bits). Its size. */
static unsigned make_dib(int core, int width, int height, int bpp, const unsigned *palette, unsigned colors,
                         unsigned (*pattern)(int, int)) {
    memset(g_dib, 0, sizeof(g_dib));
    unsigned at;
    if (core) {
        at = put32(0, 12);
        at = put16(at, (unsigned)width);
        at = put16(at, (unsigned)height);
        at = put16(at, 1);
        at = put16(at, (unsigned)bpp);
    } else {
        at = put32(0, 40);
        at = put32(at, (unsigned)width);
        at = put32(at, (unsigned)height);
        at = put16(at, 1);
        at = put16(at, (unsigned)bpp);
        at = put32(at, 0);
        at = 32;
        at = put32(at, colors);
        at = 40;
    }
    for (unsigned i = 0; i < colors; ++i) {
        g_dib[at++] = (unsigned char)palette[i];
        g_dib[at++] = (unsigned char)(palette[i] >> 8);
        g_dib[at++] = (unsigned char)(palette[i] >> 16);
        if (!core) {
            g_dib[at++] = 0;
        }
    }
    const int rows = height < 0 ? -height : height;
    const unsigned stride = (((unsigned)width * (unsigned)bpp + 31u) / 32u) * 4u;
    for (int r = 0; r < rows; ++r) {
        const int y = height < 0 ? r : rows - 1 - r;
        unsigned char *row = g_dib + at + (unsigned)r * stride;
        for (int x = 0; x < width; ++x) {
            const unsigned v = pattern(x, y);
            if (bpp == 24) {
                row[3 * x] = (unsigned char)v;
                row[3 * x + 1] = (unsigned char)(v >> 8);
                row[3 * x + 2] = (unsigned char)(v >> 16);
            } else {
                row[x * bpp / 8] |= (unsigned char)(v << (8 - bpp - (x * bpp) % 8));
            }
        }
    }
    return at + stride * (unsigned)rows;
}

static unsigned checker(int x, int y) {
    return (unsigned)((x + 2 * y) % 3 == 0);
}

static unsigned sixteen(int x, int y) {
    return (unsigned)((x + 3 * y) % 16);
}

static unsigned colours(int x, int y) {
    return (unsigned)(x * 36) << 16 | (unsigned)(y * 40) << 8 | (unsigned)((x * y * 9) % 256);
}

static void test_dibs(void) {
    static const unsigned bw[2] = {0x000000, 0xffffff};
    static const unsigned wb[2] = {0xffffff, 0x000000};
    static const unsigned red[2] = {0xff0000, 0xffffff};
    unsigned palette[16];
    for (unsigned i = 0; i < 16; ++i) {
        palette[i] = (i * 16u) << 16 | (255u - i * 16u) << 8 | ((i * 37u) % 256u);
    }
    unsigned row[64];
    sxw_dib dib;
    for (int core = 0; core < 2; ++core) {
        unsigned size = make_dib(core, 13, 9, 1, bw, 2, checker);
        CHECK(sxw_dib_parse(g_dib, size, &dib) == 0 && dib.width == 13 && dib.height == 9 && dib.stride == 4);
        CHECK(sxw_dib_monochrome(&dib));
        for (int y = 0; y < 9; ++y) {
            sxw_dib_row(&dib, y, row);
            for (int x = 0; x < 13; ++x) {
                CHECK(row[x] == (checker(x, y) ? 0xffffffu : 0u));
            }
        }
        CHECK(sxw_dib_parse(g_dib, size - 1, &dib) != 0); /* a row short */
        size = make_dib(core, 13, 9, 1, wb, 2, checker);
        CHECK(sxw_dib_parse(g_dib, size, &dib) == 0 && !sxw_dib_monochrome(&dib));
        size = make_dib(core, 11, 7, 1, red, 2, checker);
        CHECK(sxw_dib_parse(g_dib, size, &dib) == 0 && !sxw_dib_monochrome(&dib));
        size = make_dib(core, 9, 5, 4, palette, 16, sixteen);
        CHECK(sxw_dib_parse(g_dib, size, &dib) == 0 && dib.bpp == 4);
        for (int y = 0; y < 5; ++y) {
            sxw_dib_row(&dib, y, row);
            for (int x = 0; x < 9; ++x) {
                CHECK(row[x] == palette[sixteen(x, y)]);
            }
        }
    }
    /* 24 bits, top-down. */
    unsigned size = make_dib(0, 7, -6, 24, 0, 0, colours);
    CHECK(sxw_dib_parse(g_dib, size, &dib) == 0 && dib.top_down && dib.height == 6);
    for (int y = 0; y < 6; ++y) {
        sxw_dib_row(&dib, y, row);
        for (int x = 0; x < 7; ++x) {
            CHECK(row[x] == colours(x, y));
        }
    }
    /* Refused: compression, two planes, an empty or huge size, a palette
     * past the end, any depth but 1, 4, 8, 24 and 32, a short header. */
    size = make_dib(0, 7, 6, 24, 0, 0, colours);
    put32(16, 1);
    CHECK(sxw_dib_parse(g_dib, size, &dib) != 0);
    size = make_dib(0, 7, 6, 24, 0, 0, colours);
    put16(12, 2);
    CHECK(sxw_dib_parse(g_dib, size, &dib) != 0);
    size = make_dib(0, 7, 6, 24, 0, 0, colours);
    put32(4, 0);
    CHECK(sxw_dib_parse(g_dib, size, &dib) != 0);
    put32(4, 0x10000);
    CHECK(sxw_dib_parse(g_dib, size, &dib) != 0);
    size = make_dib(0, 9, 5, 4, palette, 16, sixteen);
    CHECK(sxw_dib_parse(g_dib, 40 + 30, &dib) != 0);
    size = make_dib(0, 7, 6, 24, 0, 0, colours);
    put16(14, 16);
    CHECK(sxw_dib_parse(g_dib, size, &dib) != 0);
    CHECK(sxw_dib_parse(g_dib, 11, &dib) != 0);
}

/* ---- Blits ----------------------------------------------------------------- */

static void test_blits(void) {
    unsigned int mono_pixels[8 * 4];
    unsigned int color_pixels[8 * 4];
    for (int i = 0; i < 32; ++i) {
        mono_pixels[i] = (i % 3 == 0) ? 0xffffffu : 0u;
        color_pixels[i] = 0x100000u * (unsigned)(i % 7) + 0x000101u * (unsigned)i;
    }
    color_pixels[5] = 0x123456;
    color_pixels[9] = 0x123456;
    const sxw_target mono = {mono_pixels, 8, 8, 4};
    const sxw_target colour = {color_pixels, 8, 8, 4};
    unsigned int out_pixels[10 * 6];
    sxw_target out = {out_pixels, 10, 10, 6};
    sxw_region all;
    const sxw_rect whole = {0, 0, 10, 6};
    sxw_region_set(&all, &whole);

    /* Monochrome to colour: black is the text colour, white the background. */
    memset(out_pixels, 0, sizeof(out_pixels));
    sxw_convert c = {SXW_CONVERT_MONO_TO_COLOR, 0x800000, 0x000080, 0};
    sxw_raster_blit(&out, &all, 1, 1, 8, 4, &mono, 0, 0, 8, 4, 0xcc, 0, &c);
    for (int i = 0; i < 32; ++i) {
        CHECK(out_pixels[(1 + i / 8) * 10 + 1 + i % 8] == (mono_pixels[i] ? 0x000080u : 0x800000u));
    }
    CHECK(out_pixels[0] == 0 && out_pixels[9] == 0);
    /* Colour to monochrome: the source's background colour is white. */
    c = (sxw_convert){SXW_CONVERT_COLOR_TO_MONO, 0, 0, 0x123456};
    sxw_raster_blit(&out, &all, 0, 0, 8, 4, &colour, 0, 0, 8, 4, 0xcc, 0, &c);
    for (int i = 0; i < 32; ++i) {
        CHECK(out_pixels[(i / 8) * 10 + i % 8] == (color_pixels[i] == 0x123456u ? 0xffffffu : 0u));
    }
    /* A source partly outside its bitmap moves the destination with it. */
    memset(out_pixels, 0, sizeof(out_pixels));
    c = (sxw_convert){SXW_CONVERT_NONE, 0, 0, 0};
    sxw_raster_blit(&out, &all, 0, 0, 5, 3, &colour, -2, -1, 5, 3, 0xcc, 0, &c);
    CHECK(out_pixels[0] == 0 && out_pixels[1 * 10 + 1] == 0 && out_pixels[1 * 10 + 2] == color_pixels[0]);
    CHECK(out_pixels[2 * 10 + 4] == color_pixels[8 + 2]);

    /* Overlapping, both ways, as if through a copy. */
    for (int direction = 0; direction < 2; ++direction) {
        unsigned int canvas[12 * 12];
        unsigned int copy[12 * 12];
        for (int i = 0; i < 144; ++i) {
            canvas[i] = copy[i] = (unsigned)i * 0x010203u;
        }
        sxw_target t = {canvas, 12, 12, 12};
        sxw_region r;
        const sxw_rect area = {0, 0, 12, 12};
        sxw_region_set(&r, &area);
        const int sx = direction ? 3 : 0, sy = direction ? 2 : 0, dx = direction ? 0 : 3, dy = direction ? 0 : 2;
        sxw_raster_blit(&t, &r, dx, dy, 7, 6, &t, sx, sy, 7, 6, 0xcc, 0, &c);
        for (int y = 0; y < 6; ++y) {
            for (int x = 0; x < 7; ++x) {
                CHECK(canvas[(dy + y) * 12 + dx + x] == copy[(sy + y) * 12 + sx + x]);
            }
        }
    }

    /* Growing takes the pixel under each destination pixel's centre: 9 to 20
     * repeats source columns 2 and 6 three times, 5 to 11 row 2 (Windows XP,
     * fixtures/resource). */
    unsigned int line[9];
    for (int i = 0; i < 9; ++i) {
        line[i] = (unsigned)i;
    }
    const sxw_target strip = {line, 9, 9, 1};
    unsigned int wide[20];
    sxw_target wide_target = {wide, 20, 20, 1};
    sxw_region wide_clip;
    const sxw_rect wide_area = {0, 0, 20, 1};
    sxw_region_set(&wide_clip, &wide_area);
    sxw_raster_blit(&wide_target, &wide_clip, 0, 0, 20, 1, &strip, 0, 0, 9, 1, 0xcc, 0, &c);
    static const unsigned expected_wide[20] = {0, 0, 1, 1, 2, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 6, 7, 7, 8, 8};
    CHECK(memcmp(wide, expected_wide, sizeof(wide)) == 0);
    /* Shrinking ANDs what falls on one pixel and drops what is left over:
     * 7 to 3 with the Bresenham walk from 2 * 3 - 7 takes 0-1, 2-3, 4-5. */
    unsigned int seven[7] = {0xff00ff, 0x0f0f0f, 0xffffff, 0xf0f0f0, 0x00ff00, 0x00ffff, 0x000001};
    const sxw_target seven_target = {seven, 7, 7, 1};
    unsigned int three[3];
    sxw_target three_target = {three, 3, 3, 1};
    sxw_region three_clip;
    const sxw_rect three_area = {0, 0, 3, 1};
    sxw_region_set(&three_clip, &three_area);
    sxw_raster_blit(&three_target, &three_clip, 0, 0, 3, 1, &seven_target, 0, 0, 7, 1, 0xcc, 0, &c);
    CHECK(three[0] == (0xff00ffu & 0x0f0f0fu) && three[1] == (0xffffffu & 0xf0f0f0u) &&
          three[2] == (0x00ff00u & 0x00ffffu));
}

int main(void) {
    test_dibs();
    test_blits();
    test_regions();
    test_rops();
    test_lines();
    test_fills();
    if (g_failures != 0) {
        fprintf(stderr, "win32-window-test: %d failures\n", g_failures);
        return 1;
    }
    printf("win32-window-test: ok\n");
    return 0;
}
