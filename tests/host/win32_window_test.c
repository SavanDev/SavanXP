/*
 * win32_window_test.c -- The pure parts of user32 and gdi32 on the build
 * host, with the sanitizers on: regions against a brute-force pixel set,
 * the raster operations against their truth tables, lines against the rule
 * Windows XP's were checked to follow, and clipped fills.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

int main(void) {
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
