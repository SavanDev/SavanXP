#include "view.h"
#include <stdint.h>
#include <string.h>

static int positive(int n) { return n > 0 ? n : 1; }
static int clamp(int n, int max) { return n < 0 ? 0 : n > max ? max : n; }
int iv_view_width(const struct iv_view *v) { return positive((int)((int64_t)v->width * v->zoom / 1000)); }
int iv_view_height(const struct iv_view *v) { return positive((int)((int64_t)v->height * v->zoom / 1000)); }
void iv_view_scroll(struct iv_view *v, int x, int y)
{
    int max_x = iv_view_width(v) - v->viewport_width;
    int max_y = iv_view_height(v) - v->viewport_height;
    v->x = clamp(x, max_x > 0 ? max_x : 0);
    v->y = clamp(y, max_y > 0 ? max_y : 0);
}
void iv_view_resize(struct iv_view *v, int width, int height)
{
    v->viewport_width = positive(width); v->viewport_height = positive(height);
    if (v->fit) {
        int x = (int)((int64_t)v->viewport_width * 1000 / positive(v->width));
        int y = (int)((int64_t)v->viewport_height * 1000 / positive(v->height));
        v->zoom = positive(x < y ? x : y);
        if (v->zoom > 1000) v->zoom = 1000;
    }
    iv_view_scroll(v, v->x, v->y);
}
void iv_view_zoom(struct iv_view *v, int zoom)
{
    int old = positive(v->zoom);
    int64_t cx = iv_view_width(v) <= v->viewport_width ? (int64_t)v->width * 500 :
        (int64_t)(v->x + v->viewport_width / 2) * 1000000 / old;
    int64_t cy = iv_view_height(v) <= v->viewport_height ? (int64_t)v->height * 500 :
        (int64_t)(v->y + v->viewport_height / 2) * 1000000 / old;
    v->fit = 0; v->zoom = zoom < 125 ? 125 : zoom > 16000 ? 16000 : zoom;
    iv_view_scroll(v, (int)(cx * v->zoom / 1000000) - v->viewport_width / 2,
                      (int)(cy * v->zoom / 1000000) - v->viewport_height / 2);
}
void iv_view_step(struct iv_view *v, int direction)
{
    static const int steps[] = {125,250,500,750,1000,1250,1500,2000,3000,4000,8000,16000};
    int i;
    if (direction > 0) {
        for (i = 0; i < 12; ++i) if (steps[i] > v->zoom) { iv_view_zoom(v, steps[i]); return; }
    } else {
        for (i = 11; i >= 0; --i) if (steps[i] < v->zoom) { iv_view_zoom(v, steps[i]); return; }
    }
}
static int lower(int c) { return c >= 'A' && c <= 'Z' ? c + 'a' - 'A' : c; }
int iv_name_compare(const void *left, const void *right)
{
    const unsigned char *a = left, *b = right;
    while (*a && lower(*a) == lower(*b)) { ++a; ++b; }
    return lower(*a) - lower(*b);
}
int iv_image_name(const char *name)
{
    const char *ext = strrchr(name, '.');
    return ext && (!iv_name_compare(ext,".bmp") || !iv_name_compare(ext,".png") ||
                   !iv_name_compare(ext,".jpg") || !iv_name_compare(ext,".jpeg"));
}
