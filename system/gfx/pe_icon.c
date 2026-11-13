#include "sxgfx/pe_icon.h"

/* See include/sxgfx/pe_icon.h. */

#define RT_ICON 3u
#define RT_GROUP_ICON 14u

typedef struct pe_view {
    const uint8_t* file;
    size_t size;
    uint32_t resource_rva;
    const uint8_t* sections;
    uint32_t section_count;
} pe_view;

static int in_file(const pe_view* v, size_t offset, size_t length) {
    return offset <= v->size && length <= v->size - offset;
}

static uint32_t get16(const uint8_t* p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8;
}

static uint32_t get32(const uint8_t* p) {
    return get16(p) | get16(p + 2) << 16;
}

/* A file offset for length bytes at rva, through the section table; 0 when
 * no section holds all of them in the file. */
static const uint8_t* at_rva(const pe_view* v, uint32_t rva, uint32_t length) {
    for (uint32_t i = 0; i < v->section_count; ++i) {
        const uint8_t* s = v->sections + 40u * i;
        const uint32_t start = get32(s + 12);
        const uint32_t raw_size = get32(s + 16);
        const uint32_t raw = get32(s + 20);
        if (rva >= start && rva - start <= raw_size && length <= raw_size - (rva - start)) {
            const size_t offset = (size_t)raw + (rva - start);
            return in_file(v, offset, length) ? v->file + offset : 0;
        }
    }
    return 0;
}

/* The entry of a resource directory (at rva, relative to the resource
 * section) with this id, or its first entry when id is 0. Returns the entry's
 * target, an offset from the resource section; the subdirectory bit tells
 * which kind. 0 when there is none. */
static uint32_t find_entry(const pe_view* v, uint32_t directory, uint32_t id) {
    const uint8_t* d = at_rva(v, v->resource_rva + directory, 16);
    if (d == 0) {
        return 0;
    }
    const uint32_t count = get16(d + 12) + get16(d + 14);
    const uint8_t* entries = at_rva(v, v->resource_rva + directory + 16u, 8u * count);
    if (entries == 0) {
        return 0;
    }
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t name = get32(entries + 8u * i);
        if (id == 0 || ((name & 0x80000000u) == 0 && name == id)) {
            return get32(entries + 8u * i + 4);
        }
    }
    return 0;
}

/* The data of the resource of this type and id (0: the first), its first
 * language. */
static const uint8_t* find_resource(const pe_view* v, uint32_t type, uint32_t id, uint32_t* size) {
    uint32_t target = find_entry(v, 0, type);
    if ((target & 0x80000000u) == 0) {
        return 0;
    }
    target = find_entry(v, target & 0x7fffffffu, id);
    if ((target & 0x80000000u) == 0) {
        return 0;
    }
    target = find_entry(v, target & 0x7fffffffu, 0);
    if (target == 0 || (target & 0x80000000u) != 0) {
        return 0;
    }
    const uint8_t* entry = at_rva(v, v->resource_rva + target, 16);
    if (entry == 0) {
        return 0;
    }
    *size = get32(entry + 4);
    return at_rva(v, get32(entry), *size);
}

/* A DIB icon image (BITMAPINFOHEADER, colours, XOR bits, AND mask) sampled
 * into extent x extent pixels. */
static int decode(const uint8_t* dib, uint32_t size, uint32_t extent, uint32_t* out) {
    if (size < 40u || get32(dib) < 40u || get32(dib) > size) {
        return 0;
    }
    const uint32_t header = get32(dib);
    const int32_t width = (int32_t)get32(dib + 4);
    const int32_t height = (int32_t)get32(dib + 8) / 2;
    const uint32_t bits = get16(dib + 14);
    if (width <= 0 || height <= 0 || width > 256 || height > 256 || get32(dib + 16) != 0u ||
        (bits != 1u && bits != 4u && bits != 8u && bits != 24u && bits != 32u)) {
        return 0;
    }
    uint32_t colors = bits <= 8u ? get32(dib + 32) : 0u;
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
                pixel = get32(line + 4u * x);
            } else if (bits == 24u) {
                pixel = (uint32_t)line[3u * x] | (uint32_t)line[3u * x + 1u] << 8 | (uint32_t)line[3u * x + 2u] << 16;
            } else {
                const uint32_t bit = x * bits;
                const uint32_t index = (line[bit / 8u] >> (8u - bits - bit % 8u)) & ((1u << bits) - 1u);
                pixel = index < colors ? get32(palette + 4u * index) & 0xffffffu : 0u;
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
    pe_view v = {file, size, 0, 0, 0};
    if (file == 0 || out == 0 || extent == 0u || extent > PE_ICON_MAX_EXTENT || !in_file(&v, 0, 64) ||
        file[0] != 'M' || file[1] != 'Z') {
        return 0;
    }
    const uint32_t pe = get32(file + 60);
    if (!in_file(&v, pe, 24) || get32(file + pe) != 0x00004550u) {
        return 0;
    }
    const uint32_t optional_size = get16(file + pe + 20);
    const size_t optional = (size_t)pe + 24u;
    if (!in_file(&v, optional, optional_size) || optional_size < 2u) {
        return 0;
    }
    const uint32_t magic = get16(file + optional);
    /* The data directories: after 96 bytes in PE32, 112 in PE32+; the
     * resource table is the third. */
    const uint32_t directories = magic == 0x10bu ? 96u : magic == 0x20bu ? 112u : 0u;
    if (directories == 0u || optional_size < directories + 24u) {
        return 0;
    }
    v.resource_rva = get32(file + optional + directories + 16);
    v.section_count = get16(file + pe + 6);
    if (v.resource_rva == 0u || !in_file(&v, optional + optional_size, 40u * (size_t)v.section_count)) {
        return 0;
    }
    v.sections = file + optional + optional_size;

    uint32_t group_size = 0;
    const uint8_t* group = find_resource(&v, RT_GROUP_ICON, 0, &group_size);
    if (group == 0 || group_size < 6u) {
        return 0;
    }
    const uint32_t count = get16(group + 4);
    int best = -1;
    int64_t best_score = 0;
    for (uint32_t i = 0; i < count && 6u + 14u * (i + 1u) <= group_size; ++i) {
        const uint8_t* e = group + 6u + 14u * i;
        const int32_t w = e[0] == 0 ? 256 : e[0];
        const int32_t depth = (int32_t)get16(e + 6);
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
    const uint8_t* image = find_resource(&v, RT_ICON, get16(group + 6u + 14u * (uint32_t)best + 12u), &image_size);
    return image != 0 && decode(image, image_size, extent, out);
}
