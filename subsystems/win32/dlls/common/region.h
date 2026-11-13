/*
 * region.h -- Rectangle sets for clipping and invalidation, shared by user32
 * and gdi32 and tested on the build host (tests/host/win32_window_test.c).
 *
 * A region is a list of disjoint rectangles, left/top inclusive and
 * right/bottom exclusive like a Windows RECT. Pure C with no library: the
 * same source compiles into the PE32 DLLs and into the host test.
 *
 * The list is bounded. When a union would not fit, the region becomes its
 * bounding box, which only ever invalidates more than asked for; a
 * subtraction that would not fit leaves the region unchanged, which only
 * ever draws more. Both are flagged in `overflowed`, so a test can tell.
 */
#ifndef SAVANXP_WIN32_REGION_H
#define SAVANXP_WIN32_REGION_H

#define SXW_REGION_MAX 32

typedef struct sxw_rect {
    int left;
    int top;
    int right;
    int bottom;
} sxw_rect;

typedef struct sxw_region {
    int count;
    int overflowed;
    sxw_rect rects[SXW_REGION_MAX];
} sxw_region;

int sxw_rect_empty(const sxw_rect *r);
/* *out = a intersected with b; 1 when it is not empty. */
int sxw_rect_intersect(sxw_rect *out, const sxw_rect *a, const sxw_rect *b);
void sxw_rect_offset(sxw_rect *r, int dx, int dy);

void sxw_region_clear(sxw_region *region);
void sxw_region_set(sxw_region *region, const sxw_rect *rect);
int sxw_region_empty(const sxw_region *region);
/* The smallest rectangle holding the region; empty for an empty region. */
sxw_rect sxw_region_bounds(const sxw_region *region);
void sxw_region_union_rect(sxw_region *region, const sxw_rect *rect);
void sxw_region_subtract_rect(sxw_region *region, const sxw_rect *rect);
void sxw_region_intersect_rect(sxw_region *region, const sxw_rect *rect);
void sxw_region_intersect(sxw_region *region, const sxw_region *other);
void sxw_region_offset(sxw_region *region, int dx, int dy);
int sxw_region_contains(const sxw_region *region, int x, int y);

#endif
