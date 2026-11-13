/*
 * dib.c -- See dib.h.
 */
#include "dib.h"

static unsigned int get16(const unsigned char *p) {
    return (unsigned int)p[0] | ((unsigned int)p[1] << 8);
}

static unsigned int get32(const unsigned char *p) {
    return get16(p) | (get16(p + 2) << 16);
}

int sxw_dib_parse(const void *data, unsigned int size, sxw_dib *out) {
    const unsigned char *p = data;
    if (size < 12u) {
        return -1;
    }
    const unsigned int header = get32(p);
    unsigned int entry = 0;
    unsigned int colors = 0;
    int height = 0;
    if (header == 12u) {
        out->width = (int)get16(p + 4);
        height = (int)(short)get16(p + 6);
        if (get16(p + 8) != 1u) {
            return -1;
        }
        out->bpp = (int)get16(p + 10);
        entry = 3;
    } else if (header >= 40u && header <= size) {
        out->width = (int)get32(p + 4);
        height = (int)get32(p + 8);
        if (get16(p + 12) != 1u || get32(p + 16) != 0u /* BI_RGB */) {
            return -1;
        }
        out->bpp = (int)get16(p + 14);
        colors = get32(p + 32);
        entry = 4;
    } else {
        return -1;
    }
    if (out->bpp != 1 && out->bpp != 4 && out->bpp != 8 && out->bpp != 24 && out->bpp != 32) {
        return -1;
    }
    if (out->width <= 0 || out->width > 0x4000 || height == 0 || height < -0x4000 || height > 0x4000) {
        return -1;
    }
    out->top_down = height < 0;
    out->height = height < 0 ? -height : height;
    out->palette_count = 0;
    if (out->bpp <= 8) {
        const unsigned int most = 1u << out->bpp;
        out->palette_count = colors != 0u && colors < most ? colors : most;
    }
    const unsigned int palette_bytes = out->palette_count * entry;
    if (palette_bytes > size - header) {
        return -1;
    }
    for (unsigned int i = 0; i < out->palette_count; ++i) {
        const unsigned char *e = p + header + i * entry;
        out->palette[i] = ((unsigned int)e[2] << 16) | ((unsigned int)e[1] << 8) | e[0];
    }
    out->stride = (((unsigned int)out->width * (unsigned int)out->bpp + 31u) / 32u) * 4u;
    const unsigned int offset = header + palette_bytes;
    if ((unsigned long long)out->stride * (unsigned int)out->height > size - offset) {
        return -1;
    }
    out->bits = p + offset;
    return 0;
}

void sxw_dib_row(const sxw_dib *dib, int y, unsigned int *out) {
    const int stored = dib->top_down ? y : dib->height - 1 - y;
    const unsigned char *row = dib->bits + (unsigned int)stored * dib->stride;
    for (int x = 0; x < dib->width; ++x) {
        unsigned int index;
        switch (dib->bpp) {
        case 1:
            index = (row[x >> 3] >> (7 - (x & 7))) & 1u;
            break;
        case 4:
            index = (row[x >> 1] >> ((x & 1) ? 0 : 4)) & 15u;
            break;
        case 8:
            index = row[x];
            break;
        case 24:
            out[x] = ((unsigned int)row[3 * x + 2] << 16) | ((unsigned int)row[3 * x + 1] << 8) | row[3 * x];
            continue;
        default: /* 32 */
            out[x] = ((unsigned int)row[4 * x + 2] << 16) | ((unsigned int)row[4 * x + 1] << 8) | row[4 * x];
            continue;
        }
        out[x] = index < dib->palette_count ? dib->palette[index] : 0u;
    }
}

int sxw_dib_monochrome(const sxw_dib *dib) {
    return dib->bpp == 1 && dib->palette_count == 2u && dib->palette[0] == 0u && dib->palette[1] == 0xffffffu;
}
