#include "shadow.h"
#include <string.h>

struct sx_rect window_shadow_bounds(struct sx_rect frame) {
    return sx_rect_make(frame.x + WINDOW_SHADOW_OFFSET_X - WINDOW_SHADOW_RADIUS,
        frame.y + WINDOW_SHADOW_OFFSET_Y - WINDOW_SHADOW_RADIUS,
        frame.width + 2 * WINDOW_SHADOW_RADIUS, frame.height + 2 * WINDOW_SHADOW_RADIUS);
}

unsigned window_shadow_alpha(struct sx_rect frame, int x, int y) {
    /* A cached radial falloff for the corners; straight edges use row zero. */
    static unsigned char falloff[WINDOW_SHADOW_RADIUS + 1][WINDOW_SHADOW_RADIUS + 1];
    static int ready;
    const int radius_squared = WINDOW_SHADOW_RADIUS * WINDOW_SHADOW_RADIUS;
    int dx, dy;
    if (!ready) {
        for (dy = 0; dy <= WINDOW_SHADOW_RADIUS; ++dy) for (dx = 0; dx <= WINDOW_SHADOW_RADIUS; ++dx) {
            int distance = dx * dx + dy * dy;
            int remaining = radius_squared - distance;
            falloff[dy][dx] = remaining > 0 ? (unsigned char)((WINDOW_SHADOW_ALPHA * remaining * remaining +
                radius_squared * radius_squared / 2) / (radius_squared * radius_squared)) : 0;
        }
        ready = 1;
    }
    frame.x += WINDOW_SHADOW_OFFSET_X; frame.y += WINDOW_SHADOW_OFFSET_Y;
    dx = x < frame.x ? frame.x - x : x >= frame.x + frame.width ? x - (frame.x + frame.width - 1) : 0;
    dy = y < frame.y ? frame.y - y : y >= frame.y + frame.height ? y - (frame.y + frame.height - 1) : 0;
    return dx <= WINDOW_SHADOW_RADIUS && dy <= WINDOW_SHADOW_RADIUS ? falloff[dy][dx] : 0;
}

void cursor_shadow_build(const struct cursor_image *image, struct cursor_shadow *shadow) {
    int x, y, dx, dy;
    memset(shadow, 0, sizeof(*shadow));
    shadow->width = image->width + 8;
    shadow->height = image->height + 8;
    /* A separable triangular kernel, normalized to a maximum alpha of 80.
     * Classic inverted pixels belong to the silhouette too; transparent
     * AND=white/XOR=black pixels do not. Original masks remain untouched. */
    for (y = 0; y < shadow->height; ++y) for (x = 0; x < shadow->width; ++x) {
        unsigned sum = 0;
        for (dy = -3; dy <= 3; ++dy) for (dx = -3; dx <= 3; ++dx) {
            int sx = x - 3 - 1 + dx, sy = y - 3 - 2 + dy;
            if (sx >= 0 && sy >= 0 && sx < image->width && sy < image->height) {
                unsigned i = sy * image->width + sx;
                unsigned a = image->classic ?
                    ((image->and_mask[i] != 0x00ffffffu || image->xor_rgb[i]) ? 255 : 0) : image->pixels[i] >> 24;
                sum += a * (4 - (dx < 0 ? -dx : dx)) * (4 - (dy < 0 ? -dy : dy));
            }
        }
        shadow->pixels[y * shadow->width + x] = ((sum * 80u + 32640u) / (255u * 256u)) << 24;
    }
}

int cursor_shadow_pack(const struct cursor_image *image, const struct cursor_shadow *shadow, uint32_t pixels[64 * 64]) {
    int x, y, margin = shadow ? CURSOR_SHADOW_MARGIN : 0;
    if (image->needs_background || image->width + margin > 64 || image->height + margin > 64 ||
        (shadow && (shadow->width > 64 || shadow->height > 64))) return -1;
    memset(pixels, 0, 64 * 64 * sizeof(*pixels));
    if (shadow) for (y = 0; y < shadow->height; ++y) for (x = 0; x < shadow->width; ++x)
        pixels[y * 64 + x] = shadow->pixels[y * shadow->width + x];
    for (y = 0; y < image->height; ++y) for (x = 0; x < image->width; ++x) {
        uint32_t src = image->pixels[y * image->width + x];
        uint32_t *dst = &pixels[(y + margin) * 64 + x + margin];
        unsigned a = src >> 24, sa = *dst >> 24;
        unsigned out = a + (sa * (255 - a) + 127) / 255;
        /* The shadow is black. Preserve straight-alpha colour for VirtIO. */
        if (out) *dst = (out << 24) |
            (((((src >> 16) & 255) * a + out / 2) / out) << 16) |
            (((((src >> 8) & 255) * a + out / 2) / out) << 8) |
            (((src & 255) * a + out / 2) / out);
    }
    return 0;
}
