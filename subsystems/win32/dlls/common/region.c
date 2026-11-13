/*
 * region.c -- Rectangle sets. See region.h.
 */
#include "region.h"

int sxw_rect_empty(const sxw_rect *r) {
    return r->left >= r->right || r->top >= r->bottom;
}

int sxw_rect_intersect(sxw_rect *out, const sxw_rect *a, const sxw_rect *b) {
    sxw_rect r;
    r.left = a->left > b->left ? a->left : b->left;
    r.top = a->top > b->top ? a->top : b->top;
    r.right = a->right < b->right ? a->right : b->right;
    r.bottom = a->bottom < b->bottom ? a->bottom : b->bottom;
    if (sxw_rect_empty(&r)) {
        r.left = r.top = r.right = r.bottom = 0;
        *out = r;
        return 0;
    }
    *out = r;
    return 1;
}

void sxw_rect_offset(sxw_rect *r, int dx, int dy) {
    r->left += dx;
    r->right += dx;
    r->top += dy;
    r->bottom += dy;
}

void sxw_region_clear(sxw_region *region) {
    region->count = 0;
    region->overflowed = 0;
}

void sxw_region_set(sxw_region *region, const sxw_rect *rect) {
    sxw_region_clear(region);
    if (!sxw_rect_empty(rect)) {
        region->rects[0] = *rect;
        region->count = 1;
    }
}

int sxw_region_empty(const sxw_region *region) {
    return region->count == 0;
}

sxw_rect sxw_region_bounds(const sxw_region *region) {
    sxw_rect b = {0, 0, 0, 0};
    for (int i = 0; i < region->count; ++i) {
        const sxw_rect *r = &region->rects[i];
        if (i == 0) {
            b = *r;
            continue;
        }
        b.left = r->left < b.left ? r->left : b.left;
        b.top = r->top < b.top ? r->top : b.top;
        b.right = r->right > b.right ? r->right : b.right;
        b.bottom = r->bottom > b.bottom ? r->bottom : b.bottom;
    }
    return b;
}

/* The parts of r outside cut, at most four, into out. Their count. */
static int split(const sxw_rect *r, const sxw_rect *cut, sxw_rect *out) {
    sxw_rect inside;
    if (!sxw_rect_intersect(&inside, r, cut)) {
        out[0] = *r;
        return 1;
    }
    int n = 0;
    if (r->top < inside.top) {
        out[n++] = (sxw_rect){r->left, r->top, r->right, inside.top};
    }
    if (inside.bottom < r->bottom) {
        out[n++] = (sxw_rect){r->left, inside.bottom, r->right, r->bottom};
    }
    if (r->left < inside.left) {
        out[n++] = (sxw_rect){r->left, inside.top, inside.left, inside.bottom};
    }
    if (inside.right < r->right) {
        out[n++] = (sxw_rect){inside.right, inside.top, r->right, inside.bottom};
    }
    return n;
}

void sxw_region_subtract_rect(sxw_region *region, const sxw_rect *rect) {
    if (sxw_rect_empty(rect)) {
        return;
    }
    sxw_rect result[SXW_REGION_MAX];
    int n = 0;
    for (int i = 0; i < region->count; ++i) {
        sxw_rect pieces[4];
        int count = split(&region->rects[i], rect, pieces);
        if (n + count > SXW_REGION_MAX) {
            region->overflowed = 1;
            return;
        }
        for (int j = 0; j < count; ++j) {
            result[n++] = pieces[j];
        }
    }
    for (int i = 0; i < n; ++i) {
        region->rects[i] = result[i];
    }
    region->count = n;
}

void sxw_region_union_rect(sxw_region *region, const sxw_rect *rect) {
    if (sxw_rect_empty(rect)) {
        return;
    }
    /* The new rectangle minus what the region already holds, appended. */
    sxw_region added;
    sxw_region_set(&added, rect);
    for (int i = 0; i < region->count && added.count != 0; ++i) {
        sxw_region_subtract_rect(&added, &region->rects[i]);
    }
    if (added.overflowed || region->count + added.count > SXW_REGION_MAX) {
        sxw_rect bounds = sxw_region_bounds(region);
        if (region->count == 0) {
            bounds = *rect;
        }
        bounds.left = rect->left < bounds.left ? rect->left : bounds.left;
        bounds.top = rect->top < bounds.top ? rect->top : bounds.top;
        bounds.right = rect->right > bounds.right ? rect->right : bounds.right;
        bounds.bottom = rect->bottom > bounds.bottom ? rect->bottom : bounds.bottom;
        region->rects[0] = bounds;
        region->count = 1;
        region->overflowed = 1;
        return;
    }
    for (int i = 0; i < added.count; ++i) {
        region->rects[region->count++] = added.rects[i];
    }
}

void sxw_region_intersect_rect(sxw_region *region, const sxw_rect *rect) {
    int n = 0;
    for (int i = 0; i < region->count; ++i) {
        sxw_rect r;
        if (sxw_rect_intersect(&r, &region->rects[i], rect)) {
            region->rects[n++] = r;
        }
    }
    region->count = n;
}

void sxw_region_intersect(sxw_region *region, const sxw_region *other) {
    sxw_region result;
    sxw_region_clear(&result);
    result.overflowed = region->overflowed;
    for (int i = 0; i < region->count; ++i) {
        for (int j = 0; j < other->count; ++j) {
            sxw_rect r;
            if (sxw_rect_intersect(&r, &region->rects[i], &other->rects[j])) {
                if (result.count == SXW_REGION_MAX) {
                    /* Disjoint inputs give disjoint pieces, so this needs
                     * more than SXW_REGION_MAX of them: keep the original. */
                    region->overflowed = 1;
                    return;
                }
                result.rects[result.count++] = r;
            }
        }
    }
    *region = result;
}

void sxw_region_offset(sxw_region *region, int dx, int dy) {
    for (int i = 0; i < region->count; ++i) {
        sxw_rect_offset(&region->rects[i], dx, dy);
    }
}

int sxw_region_contains(const sxw_region *region, int x, int y) {
    for (int i = 0; i < region->count; ++i) {
        const sxw_rect *r = &region->rects[i];
        if (x >= r->left && x < r->right && y >= r->top && y < r->bottom) {
            return 1;
        }
    }
    return 0;
}
