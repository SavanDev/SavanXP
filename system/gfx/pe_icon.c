#include "sxgfx/pe_icon.h"

#include "pe_resource.h"

/* See include/sxgfx/pe_icon.h. */

#define RT_ICON 3u
#define RT_GROUP_ICON 14u

/* A DIB icon image (BITMAPINFOHEADER, colours, XOR bits, AND mask) sampled
 * into extent x extent pixels. */
static int decode(const uint8_t* dib, uint32_t size, uint32_t extent, uint32_t* out) {
    if (size < 40u || pe_get32(dib) < 40u || pe_get32(dib) > size) {
        return 0;
    }
    const uint32_t header = pe_get32(dib);
    const int32_t width = (int32_t)pe_get32(dib + 4);
    const int32_t height = (int32_t)pe_get32(dib + 8) / 2;
    const uint32_t bits = pe_get16(dib + 14);
    if (width <= 0 || height <= 0 || width > 256 || height > 256 || pe_get32(dib + 16) != 0u ||
        (bits != 1u && bits != 4u && bits != 8u && bits != 24u && bits != 32u)) {
        return 0;
    }
    uint32_t colors = bits <= 8u ? pe_get32(dib + 32) : 0u;
    if (bits <= 8u && (colors == 0u || colors > (1u << bits))) {
        colors = 1u << bits;
    }
    const uint32_t xor_stride = (((uint32_t)width * bits + 31u) / 32u) * 4u;
    const uint32_t and_stride = (((uint32_t)width + 31u) / 32u) * 4u;
    const uint64_t need = (uint64_t)header + 4u * colors + ((uint64_t)xor_stride + and_stride) * (uint32_t)height;
    if (need > size) {
        return 0;
    }
    const uint8_t* palette = dib + header;
    const uint8_t* xor_bits = palette + 4u * colors;
    const uint8_t* and_bits = xor_bits + xor_stride * (uint32_t)height;
    int alpha = 0;
    for (int32_t y = 0; bits == 32u && y < height && !alpha; ++y) {
        for (int32_t x = 0; x < width && !alpha; ++x) {
            alpha = xor_bits[(uint32_t)y * xor_stride + 4u * (uint32_t)x + 3u] != 0;
        }
    }
    for (uint32_t oy = 0; oy < extent; ++oy) {
        for (uint32_t ox = 0; ox < extent; ++ox) {
            const uint32_t x = ox * (uint32_t)width / extent;
            const uint32_t row = (uint32_t)height - 1u - oy * (uint32_t)height / extent; /* bottom-up */
            const uint8_t* line = xor_bits + row * xor_stride;
            uint32_t pixel;
            if (bits == 32u) {
                pixel = pe_get32(line + 4u * x);
            } else if (bits == 24u) {
                pixel = (uint32_t)line[3u * x] | (uint32_t)line[3u * x + 1u] << 8 | (uint32_t)line[3u * x + 2u] << 16;
            } else {
                const uint32_t bit = x * bits;
                const uint32_t index = (line[bit / 8u] >> (8u - bits - bit % 8u)) & ((1u << bits) - 1u);
                pixel = index < colors ? pe_get32(palette + 4u * index) & 0xffffffu : 0u;
            }
            if (!alpha) {
                const int masked = (and_bits[row * and_stride + x / 8u] >> (7u - x % 8u)) & 1u;
                pixel = masked ? 0u : (pixel & 0xffffffu) | 0xff000000u;
            }
            out[oy * extent + ox] = pixel;
        }
    }
    return 1;
}

int pe_icon_from_file(const uint8_t* file, size_t size, uint32_t extent, uint32_t* out) {
    pe_resources v;
    if (out == 0 || extent == 0u || extent > PE_ICON_MAX_EXTENT || !pe_resources_open(&v, file, size)) {
        return 0;
    }

    uint32_t group_size = 0;
    const uint8_t* group = pe_resources_find(&v, RT_GROUP_ICON, 0, &group_size);
    if (group == 0 || group_size < 6u) {
        return 0;
    }
    const uint32_t count = pe_get16(group + 4);
    int best = -1;
    int64_t best_score = 0;
    for (uint32_t i = 0; i < count && 6u + 14u * (i + 1u) <= group_size; ++i) {
        const uint8_t* e = group + 6u + 14u * i;
        const int32_t w = e[0] == 0 ? 256 : e[0];
        const int32_t depth = (int32_t)pe_get16(e + 6);
        /* Exact first, then larger before smaller, then nearer; more colours
         * breaks a tie. */
        const int32_t distance = w - (int32_t)extent;
        const int64_t score = (distance == 0 ? 0 : distance > 0 ? 1000 + distance : 100000 - distance) * 64 - depth;
        if (best < 0 || score < best_score) {
            best = (int)i;
            best_score = score;
        }
    }
    if (best < 0) {
        return 0;
    }
    uint32_t image_size = 0;
    const uint8_t* image = pe_resources_find(&v, RT_ICON, pe_get16(group + 6u + 14u * (uint32_t)best + 12u), &image_size);
    return image != 0 && decode(image, image_size, extent, out);
}
