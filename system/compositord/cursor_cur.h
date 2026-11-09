#pragma once
#include <stddef.h>
#include <stdint.h>

#define CUR_MAX_DIMENSION 64
#define CUR_MAX_FILE_SIZE (1024u * 1024u)

/* Pixels are straight-alpha ARGB. In classic mode each pixel is
 * (background & and_mask) ^ xor_rgb; background alpha is preserved.
 * Hardware pixels are also provided when that operation is background-free. */
struct cursor_image {
    int width, height, hotspot_x, hotspot_y;
    int classic, needs_background;
    uint32_t pixels[CUR_MAX_DIMENSION * CUR_MAX_DIMENSION];
    uint32_t and_mask[CUR_MAX_DIMENSION * CUR_MAX_DIMENSION];
    uint32_t xor_rgb[CUR_MAX_DIMENSION * CUR_MAX_DIMENSION];
};

/* Returns NULL on success, otherwise a static diagnostic. Output is only
 * changed on success. The largest supported image wins, then bit depth. */
const char *cursor_cur_decode(const void *bytes, size_t size, struct cursor_image *out);
uint32_t cursor_image_pixel(const struct cursor_image *image, unsigned index, uint32_t background);
