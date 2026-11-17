#pragma once

/* Call-free viewport model. All zoom values are per mille of decoded pixels. */
struct iv_view {
    int width, height, viewport_width, viewport_height;
    int zoom, fit, x, y;
};
void iv_view_resize(struct iv_view *view, int width, int height);
int iv_view_width(const struct iv_view *view);
int iv_view_height(const struct iv_view *view);
void iv_view_scroll(struct iv_view *view, int x, int y);
void iv_view_zoom(struct iv_view *view, int zoom);
void iv_view_step(struct iv_view *view, int direction);
int iv_name_compare(const void *left, const void *right);
int iv_image_name(const char *name);
