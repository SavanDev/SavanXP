#include "cursor_cur.h"
#include <string.h>

static unsigned u16(const unsigned char *p) { return p[0] | ((unsigned)p[1] << 8); }
static uint32_t u32(const unsigned char *p) {
    return u16(p) | ((uint32_t)u16(p + 2) << 16);
}

struct candidate {
    const unsigned char *data;
    unsigned width, height, bits, colors, hotspot_x, hotspot_y;
    size_t stride, mask_stride;
};

static int candidate_read(const unsigned char *bytes, size_t size, size_t directory_end,
                          const unsigned char *entry, struct candidate *c) {
    size_t offset = u32(entry + 12), length = u32(entry + 8);
    unsigned width = entry[0] ? entry[0] : 256, height = entry[1] ? entry[1] : 256;
    unsigned bits, colors;
    size_t stride, mask_stride, required;
    const unsigned char *p;
    if (entry[3] || width > CUR_MAX_DIMENSION || height > CUR_MAX_DIMENSION ||
        u16(entry + 4) >= width || u16(entry + 6) >= height ||
        offset < directory_end || offset > size || length > size - offset || length < 40) return 0;
    p = bytes + offset;
    if (u32(p) != 40 || u32(p + 4) != width || u32(p + 8) != height * 2 ||
        u16(p + 12) != 1 || u32(p + 16) != 0) return 0;
    bits = u16(p + 14);
    if (bits != 1 && bits != 4 && bits != 8 && bits != 24 && bits != 32) return 0;
    colors = u32(p + 32);
    if (bits <= 8) {
        if (colors == 0) colors = 1u << bits;
        if (colors > (1u << bits)) return 0;
    } else if (colors != 0) return 0;
    stride = ((width * bits + 31) / 32) * 4;
    mask_stride = ((width + 31) / 32) * 4;
    required = 40 + colors * 4 + (stride + mask_stride) * height;
    if (required > length) return 0;
    /* Palette indices are validated before choosing an image. */
    if (bits <= 8) {
        unsigned x, y;
        for (y = 0; y < height; ++y) for (x = 0; x < width; ++x) {
            const unsigned char *row = p + 40 + colors * 4 + y * stride;
            unsigned index = (row[x * bits / 8] >> (8 - bits - ((x * bits) % 8))) & ((1u << bits) - 1);
            if (index >= colors) return 0;
        }
    }
    c->data = p; c->width = width; c->height = height; c->bits = bits; c->colors = colors;
    c->hotspot_x = u16(entry + 4); c->hotspot_y = u16(entry + 6);
    c->stride = stride; c->mask_stride = mask_stride;
    return 1;
}

/* Prefer the desktop's native 32px size without resampling file contents. */
static int candidate_preferred(const struct candidate *c, const struct candidate *best) {
    unsigned area = c->width * c->height, best_area = best->width * best->height;
    int fits = c->width <= CUR_PREFERRED_DIMENSION && c->height <= CUR_PREFERRED_DIMENSION;
    int best_fits = best->width <= CUR_PREFERRED_DIMENSION && best->height <= CUR_PREFERRED_DIMENSION;
    if (!best->data) return 1;
    if (fits != best_fits) return fits;
    if (area != best_area) return fits ? area > best_area : area < best_area;
    return c->bits > best->bits;
}

const char *cursor_cur_decode(const void *bytes, size_t size, struct cursor_image *out) {
    const unsigned char *p = bytes;
    struct candidate best = {0}, c;
    unsigned count, i, x, y;
    int alpha = 0;
    const unsigned char *bitmap, *mask;
    if (!p || !out || size < 6 || size > CUR_MAX_FILE_SIZE) return "invalid file size";
    count = u16(p + 4);
    if (u16(p) != 0 || u16(p + 2) != 2 || count == 0 || count > 256 ||
        6u + count * 16u > size) return "invalid CUR directory";
    for (i = 0; i < count; ++i) {
        if (!candidate_read(p, size, 6u + count * 16u, p + 6 + i * 16, &c)) continue;
        if (candidate_preferred(&c, &best)) best = c;
    }
    if (!best.data) return "no supported CUR image";
    bitmap = best.data + 40 + best.colors * 4;
    mask = bitmap + best.stride * best.height;
    if (best.bits == 32) {
        for (y = 0; y < best.height; ++y) for (x = 0; x < best.width; ++x)
            if (bitmap[y * best.stride + x * 4 + 3]) alpha = 1;
    }
    memset(out, 0, sizeof(*out));
    out->width = best.width; out->height = best.height;
    out->hotspot_x = best.hotspot_x; out->hotspot_y = best.hotspot_y;
    out->classic = !alpha;
    for (y = 0; y < best.height; ++y) for (x = 0; x < best.width; ++x) {
        const unsigned char *row = bitmap + (best.height - 1 - y) * best.stride;
        const unsigned char *color;
        unsigned index = y * best.width + x;
        unsigned and_bit = (mask[(best.height - 1 - y) * best.mask_stride + x / 8] >> (7 - x % 8)) & 1;
        uint32_t rgb;
        if (best.bits <= 8) {
            unsigned palette = (row[x * best.bits / 8] >> (8 - best.bits - ((x * best.bits) % 8))) & ((1u << best.bits) - 1);
            color = best.data + 40 + palette * 4;
        } else color = row + x * (best.bits / 8);
        rgb = color[0] | ((uint32_t)color[1] << 8) | ((uint32_t)color[2] << 16);
        if (alpha) out->pixels[index] = rgb | ((uint32_t)color[3] << 24);
        else {
            out->and_mask[index] = and_bit ? 0x00ffffffu : 0;
            out->xor_rgb[index] = rgb;
            out->pixels[index] = and_bit ? 0 : 0xff000000u | rgb;
            if (and_bit && rgb) out->needs_background = 1;
        }
    }
    return 0;
}

uint32_t cursor_image_pixel(const struct cursor_image *image, unsigned index, uint32_t background) {
    uint32_t src = image->pixels[index];
    unsigned a = src >> 24, inv = 255 - a;
    if (image->classic)
        return (background & 0xff000000u) | ((background & image->and_mask[index]) ^ image->xor_rgb[index]);
    /* Same straight-alpha source-over rounding as the painter. */
    return 0xff000000u |
        (((((src >> 16) & 255) * a + ((background >> 16) & 255) * inv + 127) / 255) << 16) |
        (((((src >> 8) & 255) * a + ((background >> 8) & 255) * inv + 127) / 255) << 8) |
        (((src & 255) * a + (background & 255) * inv + 127) / 255);
}
