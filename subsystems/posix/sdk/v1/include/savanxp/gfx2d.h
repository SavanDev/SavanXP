#pragma once

#include <stddef.h>
#include <stdint.h>

#include "savanxp/syscall.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SX_RECT_SET_CAPACITY 32

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
};

struct sx_painter {
    struct sx_bitmap* target;
    struct sx_rect clip_rect;
    int has_clip;
};

struct sx_rect_set {
    size_t count;
    struct sx_rect rects[SX_RECT_SET_CAPACITY];
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

void sx_painter_init(struct sx_painter* painter, struct sx_bitmap* bitmap);
void sx_painter_clear_clip(struct sx_painter* painter);
void sx_painter_add_clip_rect(struct sx_painter* painter, struct sx_rect rect);
void sx_painter_fill(struct sx_painter* painter, uint32_t colour);
void sx_painter_fill_rect(struct sx_painter* painter, struct sx_rect rect, uint32_t colour);
void sx_painter_draw_frame(struct sx_painter* painter, struct sx_rect rect, uint32_t colour);
void sx_painter_blit_bitmap(struct sx_painter* painter, const struct sx_bitmap* source, int dst_x, int dst_y);
void sx_painter_draw_scaled_bitmap_nearest(
    struct sx_painter* painter,
    const struct sx_bitmap* source,
    struct sx_rect destination,
    struct sx_rect source_rect);
void sx_painter_draw_text(struct sx_painter* painter, int x, int y, const char* text, uint32_t colour);

void sx_rect_set_clear(struct sx_rect_set* set);
int sx_rect_set_add(struct sx_rect_set* set, struct sx_rect rect);
int sx_rect_set_add_translated(struct sx_rect_set* set, struct sx_rect rect, int dx, int dy);
struct sx_rect sx_rect_set_bounds(const struct sx_rect_set* set);
int sx_rect_set_valid(const struct sx_rect_set* set);

#ifdef __cplusplus
}
#endif
