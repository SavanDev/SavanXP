#include "savanxp/img.h"

#include <stdlib.h>
#include <string.h>

/* System image decoders. PNG handles all base colour types and sample depths,
 * Adam7, concatenated IDAT chunks and all five filters. Inflate implements
 * RFC 1951 (stored, fixed and dynamic Huffman blocks). BMP follows below.
 * Only memory allocation and string functions are needed, so the same
 * decoders also work in SUN. */

/* --- inflate -------------------------------------------------------------- */

struct sx_zstream {
    const unsigned char *in;
    size_t in_size;
    size_t in_pos;
    uint32_t bitbuf;
    int bitcount;
    unsigned char *out;
    size_t out_size;
    size_t out_pos;
    int failed;
};

static int zs_bits(struct sx_zstream *s, int count, uint32_t *value)
{
    uint32_t mask;

    if (count < 0 || count > 16)
    {
        s->failed = 1;
        return 0;
    }
    while (s->bitcount < count)
    {
        if (s->in_pos >= s->in_size)
        {
            s->failed = 1;
            return 0;
        }
        s->bitbuf |= (uint32_t)s->in[s->in_pos++] << s->bitcount;
        s->bitcount += 8;
    }
    if (count == 0)
    {
        *value = 0;
        return 1;
    }
    mask = (1u << (unsigned int)count) - 1u;
    *value = s->bitbuf & mask;
    s->bitbuf >>= count;
    s->bitcount -= count;
    return 1;
}

static int zs_bit(struct sx_zstream *s, int *bit)
{
    uint32_t value;

    if (!zs_bits(s, 1, &value))
    {
        return 0;
    }
    *bit = (int)value;
    return 1;
}

#define SX_ZHUFF_MAX_BITS 15
#define SX_ZHUFF_MAX_SYMBOLS 288

struct sx_zhuff {
    uint16_t count[SX_ZHUFF_MAX_BITS + 1];
    uint16_t symbol[SX_ZHUFF_MAX_SYMBOLS];
};

static int zh_build(struct sx_zhuff *h, const unsigned char *lengths, int count)
{
    uint16_t offs[SX_ZHUFF_MAX_BITS + 2];
    int left = 1;
    int i;
    int len;

    if (count < 0 || count > SX_ZHUFF_MAX_SYMBOLS)
    {
        return 0;
    }
    for (len = 0; len <= SX_ZHUFF_MAX_BITS; ++len)
    {
        h->count[len] = 0;
    }
    for (i = 0; i < count; ++i)
    {
        if (lengths[i] > SX_ZHUFF_MAX_BITS)
        {
            return 0;
        }
        h->count[lengths[i]] += 1u;
    }
    h->count[0] = 0;
    /* Sobre-suscrito no vale; incompleto si: los codigos que falten fallan al
     * decodificar, que es lo correcto. */
    for (len = 1; len <= SX_ZHUFF_MAX_BITS; ++len)
    {
        left <<= 1;
        left -= (int)h->count[len];
        if (left < 0)
        {
            return 0;
        }
    }
    offs[1] = 0;
    for (len = 2; len <= SX_ZHUFF_MAX_BITS; ++len)
    {
        offs[len] = (uint16_t)(offs[len - 1] + h->count[len - 1]);
    }
    for (i = 0; i < count; ++i)
    {
        unsigned int length = lengths[i];

        if (length != 0u)
        {
            h->symbol[offs[length]++] = (uint16_t)i;
        }
    }
    return 1;
}

static int zh_decode(struct sx_zstream *s, const struct sx_zhuff *h, int *symbol)
{
    int code = 0;
    int first = 0;
    int base = 0;
    int len;
    int bit;

    for (len = 1; len <= SX_ZHUFF_MAX_BITS; ++len)
    {
        int count;

        if (!zs_bit(s, &bit))
        {
            return 0;
        }
        code = (code << 1) | bit;
        count = (int)h->count[len];
        if (code >= first && code - first < count)
        {
            *symbol = (int)h->symbol[base + (code - first)];
            return 1;
        }
        base += count;
        first = (first + count) << 1;
    }
    s->failed = 1;
    return 0;
}

static void zh_fixed(struct sx_zhuff *lit, struct sx_zhuff *dist)
{
    unsigned char lit_lengths[288];
    unsigned char dist_lengths[32];
    int i;

    for (i = 0; i < 144; ++i)
    {
        lit_lengths[i] = 8;
    }
    for (; i < 256; ++i)
    {
        lit_lengths[i] = 9;
    }
    for (; i < 280; ++i)
    {
        lit_lengths[i] = 7;
    }
    for (; i < 288; ++i)
    {
        lit_lengths[i] = 8;
    }
    for (i = 0; i < 32; ++i)
    {
        dist_lengths[i] = 5;
    }
    /* Las tablas fijas son validas por construccion. */
    (void)zh_build(lit, lit_lengths, 288);
    (void)zh_build(dist, dist_lengths, 32);
}

static const unsigned char k_code_length_order[19] = {
    16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15
};

static int zh_dynamic(struct sx_zstream *s, struct sx_zhuff *lit, struct sx_zhuff *dist)
{
    unsigned char code_lengths[19];
    unsigned char lengths[288 + 32];
    struct sx_zhuff code_tree;
    uint32_t hlit;
    uint32_t hdist;
    uint32_t hclen;
    uint32_t total;
    uint32_t i;

    if (!zs_bits(s, 5, &hlit) || !zs_bits(s, 5, &hdist) || !zs_bits(s, 4, &hclen))
    {
        return 0;
    }
    hlit += 257u;
    hdist += 1u;
    hclen += 4u;
    if (hlit > 286u || hdist > 30u)
    {
        return 0;
    }
    for (i = 0; i < 19u; ++i)
    {
        code_lengths[i] = 0;
    }
    for (i = 0; i < hclen; ++i)
    {
        uint32_t value;

        if (!zs_bits(s, 3, &value))
        {
            return 0;
        }
        code_lengths[k_code_length_order[i]] = (unsigned char)value;
    }
    if (!zh_build(&code_tree, code_lengths, 19))
    {
        return 0;
    }
    total = hlit + hdist;
    i = 0;
    while (i < total)
    {
        int symbol;

        if (!zh_decode(s, &code_tree, &symbol))
        {
            return 0;
        }
        if (symbol < 16)
        {
            lengths[i++] = (unsigned char)symbol;
        }
        else if (symbol == 16)
        {
            uint32_t repeat;
            unsigned char previous;
            uint32_t k;

            if (i == 0)
            {
                return 0;
            }
            if (!zs_bits(s, 2, &repeat))
            {
                return 0;
            }
            repeat += 3u;
            previous = lengths[i - 1u];
            if (i + repeat > total)
            {
                return 0;
            }
            for (k = 0; k < repeat; ++k)
            {
                lengths[i++] = previous;
            }
        }
        else if (symbol == 17)
        {
            uint32_t repeat;
            uint32_t k;

            if (!zs_bits(s, 3, &repeat))
            {
                return 0;
            }
            repeat += 3u;
            if (i + repeat > total)
            {
                return 0;
            }
            for (k = 0; k < repeat; ++k)
            {
                lengths[i++] = 0;
            }
        }
        else
        {
            uint32_t repeat;
            uint32_t k;

            if (!zs_bits(s, 7, &repeat))
            {
                return 0;
            }
            repeat += 11u;
            if (i + repeat > total)
            {
                return 0;
            }
            for (k = 0; k < repeat; ++k)
            {
                lengths[i++] = 0;
            }
        }
    }
    /* Sin fin de bloque no hay stream valido. */
    if (lengths[256] == 0)
    {
        return 0;
    }
    if (!zh_build(lit, lengths, (int)hlit))
    {
        return 0;
    }
    if (!zh_build(dist, lengths + hlit, (int)hdist))
    {
        return 0;
    }
    return 1;
}

static const uint16_t k_length_base[29] = {
    3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
    35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258
};
static const uint8_t k_length_extra[29] = {
    0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
    3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0
};
static const uint16_t k_dist_base[30] = {
    1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
    257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145,
    8193, 12289, 16385, 24577
};
static const uint8_t k_dist_extra[30] = {
    0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
    7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13
};

static int zs_match(struct sx_zstream *s, int symbol, const struct sx_zhuff *dist)
{
    int length_index = symbol - 257;
    uint32_t length;
    uint32_t distance;
    int dist_symbol;
    size_t k;

    if (length_index < 0 || length_index > 28)
    {
        s->failed = 1;
        return 0;
    }
    length = k_length_base[length_index];
    if (k_length_extra[length_index] != 0u)
    {
        uint32_t extra;

        if (!zs_bits(s, (int)k_length_extra[length_index], &extra))
        {
            return 0;
        }
        length += extra;
    }
    if (!zh_decode(s, dist, &dist_symbol))
    {
        return 0;
    }
    if (dist_symbol < 0 || dist_symbol > 29)
    {
        s->failed = 1;
        return 0;
    }
    distance = k_dist_base[dist_symbol];
    if (k_dist_extra[dist_symbol] != 0u)
    {
        uint32_t extra;

        if (!zs_bits(s, (int)k_dist_extra[dist_symbol], &extra))
        {
            return 0;
        }
        distance += extra;
    }
    if (distance == 0u || distance > (uint32_t)s->out_pos)
    {
        s->failed = 1;
        return 0;
    }
    if ((size_t)length > s->out_size - s->out_pos)
    {
        s->failed = 1;
        return 0;
    }
    /* Bucle byte a byte a proposito: con distancia < longitud la fuente se
     * solapa con el destino y hay que leer lo recien escrito. */
    for (k = 0; k < (size_t)length; ++k)
    {
        s->out[s->out_pos] = s->out[s->out_pos - (size_t)distance];
        s->out_pos += 1u;
    }
    return 1;
}

static int zs_blocks(struct sx_zstream *s)
{
    struct sx_zhuff lit;
    struct sx_zhuff dist;
    int fixed_ready = 0;

    for (;;)
    {
        int final_block;
        uint32_t block_type;

        if (!zs_bit(s, &final_block))
        {
            return 0;
        }
        if (!zs_bits(s, 2, &block_type))
        {
            return 0;
        }
        if (block_type == 0u)
        {
            uint32_t length;
            uint32_t complement;

            /* Alinear a byte: se descartan los bits sin leer. */
            s->bitbuf >>= (unsigned int)(s->bitcount & 7);
            s->bitcount &= ~7;
            if (!zs_bits(s, 16, &length) || !zs_bits(s, 16, &complement))
            {
                return 0;
            }
            if ((length ^ complement) != 0xFFFFu)
            {
                s->failed = 1;
                return 0;
            }
            if (length != 0u)
            {
                if ((size_t)length > s->in_size - s->in_pos ||
                    (size_t)length > s->out_size - s->out_pos)
                {
                    s->failed = 1;
                    return 0;
                }
                memcpy(s->out + s->out_pos, s->in + s->in_pos, (size_t)length);
                s->in_pos += (size_t)length;
                s->out_pos += (size_t)length;
            }
        }
        else if (block_type == 1u || block_type == 2u)
        {
            if (block_type == 1u)
            {
                if (!fixed_ready)
                {
                    zh_fixed(&lit, &dist);
                    fixed_ready = 1;
                }
            }
            else
            {
                if (!zh_dynamic(s, &lit, &dist))
                {
                    return 0;
                }
            }
            for (;;)
            {
                int symbol;

                if (!zh_decode(s, &lit, &symbol))
                {
                    return 0;
                }
                if (symbol == 256)
                {
                    break;
                }
                if (symbol < 256)
                {
                    if (s->out_pos >= s->out_size)
                    {
                        s->failed = 1;
                        return 0;
                    }
                    s->out[s->out_pos++] = (unsigned char)symbol;
                }
                else
                {
                    if (!zs_match(s, symbol, &dist))
                    {
                        return 0;
                    }
                }
            }
        }
        else
        {
            s->failed = 1;
            return 0;
        }
        if (final_block != 0)
        {
            break;
        }
    }
    return 1;
}

static uint32_t sx_adler32(const unsigned char *data, size_t size)
{
    uint32_t a = 1u;
    uint32_t b = 0u;

    /* Modulo diferido en tandas de 5552: a y b no desbordan en 32 bits. */
    while (size > 0u)
    {
        size_t chunk = size > 5552u ? 5552u : size;
        size_t i;

        for (i = 0; i < chunk; ++i)
        {
            a += data[i];
            b += a;
        }
        a %= 65521u;
        b %= 65521u;
        data += chunk;
        size -= chunk;
    }
    return (b << 16) | a;
}

static int sx_zlib_decompress(const unsigned char *in, size_t in_size, unsigned char *out,
                              size_t out_size, size_t *produced)
{
    struct sx_zstream s;
    uint32_t header;
    uint32_t adler_expected;
    uint32_t adler_actual;

    if (in_size < 6u)
    {
        return 0;
    }
    header = ((uint32_t)in[0] << 8) | (uint32_t)in[1];
    if ((in[0] & 0x0Fu) != 8u || (in[0] >> 4) > 7u || (header % 31u) != 0u || (in[1] & 0x20u) != 0u)
    {
        /* No es deflate, ventana mayor de 32K, mal checksum o diccionario
         * preestablecido: nada de eso se soporta. */
        return 0;
    }
    s.in = in;
    s.in_size = in_size;
    s.in_pos = 2;
    s.bitbuf = 0u;
    s.bitcount = 0;
    s.out = out;
    s.out_size = out_size;
    s.out_pos = 0;
    s.failed = 0;
    if (!zs_blocks(&s) || s.failed)
    {
        return 0;
    }
    /* Alinear a byte para el trailer. */
    s.bitbuf >>= (unsigned int)(s.bitcount & 7);
    s.bitcount &= ~7;
    if (s.in_pos + 4u > s.in_size)
    {
        return 0;
    }
    adler_expected = ((uint32_t)s.in[s.in_pos] << 24) | ((uint32_t)s.in[s.in_pos + 1u] << 16) |
        ((uint32_t)s.in[s.in_pos + 2u] << 8) | (uint32_t)s.in[s.in_pos + 3u];
    adler_actual = sx_adler32(out, s.out_pos);
    if (adler_expected != adler_actual)
    {
        return 0;
    }
    *produced = s.out_pos;
    return 1;
}

/* --- PNG ------------------------------------------------------------------ */

static uint32_t read_u32be(const unsigned char *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) |
        (uint32_t)p[3];
}

static int append_bytes(unsigned char **buffer, size_t *size, size_t *capacity,
                        const unsigned char *data, size_t count)
{
    if (count > 0u && *size + count < *size)
    {
        return 0;
    }
    if (*size + count > *capacity)
    {
        size_t grown = *capacity != 0u ? *capacity : 8192u;
        unsigned char *grown_buffer;

        while (grown < *size + count)
        {
            if (grown > (size_t)-1 / 2u)
            {
                return 0;
            }
            grown *= 2u;
        }
        grown_buffer = (unsigned char *)malloc(grown);
        if (grown_buffer == 0)
        {
            return 0;
        }
        if (*size != 0u)
        {
            memcpy(grown_buffer, *buffer, *size);
        }
        free(*buffer);
        *buffer = grown_buffer;
        *capacity = grown;
    }
    if (count != 0u)
    {
        memcpy(*buffer + *size, data, count);
    }
    *size += count;
    return 1;
}

static int paeth_predictor(int a, int b, int c)
{
    int p = a + b - c;
    int pa = p > a ? p - a : a - p;
    int pb = p > b ? p - b : b - p;
    int pc = p > c ? p - c : c - p;

    if (pa <= pb && pa <= pc)
    {
        return a;
    }
    if (pb <= pc)
    {
        return b;
    }
    return c;
}

struct png_layout
{
    uint32_t width;
    uint32_t height;
    unsigned depth;
    unsigned type;
    unsigned channels;
    unsigned interlace;
};

struct png_colours
{
    uint32_t palette[256]; /* straight-alpha ARGB; unspecified alpha is opaque */
    uint32_t palette_count;
    uint32_t key[3];       /* tRNS samples at the original depth */
    int have_key;
};

struct png_pass
{
    uint32_t x, y, dx, dy;
    uint32_t width, height;
};

static uint32_t read_u16be(const unsigned char *p)
{
    return ((uint32_t)p[0] << 8) | p[1];
}

static int png_header(const unsigned char *body, struct png_layout *layout)
{
    unsigned depth = body[8];
    unsigned type = body[9];
    unsigned channels;

    switch (type)
    {
        case 0u: channels = 1u; break;
        case 2u: channels = 3u; break;
        case 3u: channels = 1u; break;
        case 4u: channels = 2u; break;
        case 6u: channels = 4u; break;
        default: return 0;
    }
    if ((depth != 1u && depth != 2u && depth != 4u && depth != 8u && depth != 16u) ||
        (type == 3u && depth == 16u) || (type != 0u && type != 3u && depth < 8u) ||
        body[10] != 0u || body[11] != 0u || body[12] > 1u)
    {
        return 0;
    }
    layout->width = read_u32be(body);
    layout->height = read_u32be(body + 4u);
    layout->depth = depth;
    layout->type = type;
    layout->channels = channels;
    layout->interlace = body[12];
    return layout->width != 0u && layout->height != 0u &&
           layout->width <= SX_PNG_MAX_DIMENSION && layout->height <= SX_PNG_MAX_DIMENSION;
}

static void png_pass_layout(const struct png_layout *layout, unsigned index, struct png_pass *pass)
{
    static const unsigned char adam7[7][4] = {
        {0, 0, 8, 8}, {4, 0, 8, 8}, {0, 4, 4, 8}, {2, 0, 4, 4},
        {0, 2, 2, 4}, {1, 0, 2, 2}, {0, 1, 1, 2}
    };
    pass->x = layout->interlace ? adam7[index][0] : 0u;
    pass->y = layout->interlace ? adam7[index][1] : 0u;
    pass->dx = layout->interlace ? adam7[index][2] : 1u;
    pass->dy = layout->interlace ? adam7[index][3] : 1u;
    pass->width = layout->width > pass->x ? (layout->width - pass->x + pass->dx - 1u) / pass->dx : 0u;
    pass->height = layout->height > pass->y ? (layout->height - pass->y + pass->dy - 1u) / pass->dy : 0u;
}

static int png_unfilter(unsigned char *current, const unsigned char *previous,
                        size_t row_bytes, size_t bpp, unsigned filter)
{
    size_t i;
    if (filter > 4u)
    {
        return 0;
    }
    /* Reconstruct in place: left-hand bytes and the preceding row are already
     * unfiltered. bpp rounds up to one byte for packed low-depth samples. */
    for (i = 0u; i < row_bytes; ++i)
    {
        int a = i >= bpp ? current[i - bpp] : 0;
        int b = previous != 0 ? previous[i] : 0;
        int c = previous != 0 && i >= bpp ? previous[i - bpp] : 0;
        int prediction = 0;
        if (filter == 1u) prediction = a;
        else if (filter == 2u) prediction = b;
        else if (filter == 3u) prediction = (a + b) / 2;
        else if (filter == 4u) prediction = paeth_predictor(a, b, c);
        current[i] = (unsigned char)(current[i] + prediction);
    }
    return 1;
}

static uint32_t png_sample(const unsigned char *row, size_t index, unsigned depth)
{
    if (depth == 16u)
    {
        return read_u16be(row + index * 2u);
    }
    if (depth == 8u)
    {
        return row[index];
    }
    index *= depth;
    return (row[index / 8u] >> (8u - depth - index % 8u)) & ((1u << depth) - 1u);
}

static int png_pixel(const struct png_layout *layout, const struct png_colours *colours,
                     const unsigned char *row, uint32_t x, int keep_alpha, uint32_t *pixel)
{
    uint32_t maximum = (1u << layout->depth) - 1u;
    uint32_t samples[4] = {0u, 0u, 0u, maximum};
    uint32_t red, green, blue, alpha;
    unsigned c;
    for (c = 0u; c < layout->channels; ++c)
    {
        samples[c] = png_sample(row, (size_t)x * layout->channels + c, layout->depth);
    }
    if (layout->type == 3u)
    {
        uint32_t entry;
        if (samples[0] >= colours->palette_count) return 0;
        entry = colours->palette[samples[0]];
        red = (entry >> 16) & 255u;
        green = (entry >> 8) & 255u;
        blue = entry & 255u;
        alpha = entry >> 24;
    }
    else
    {
        uint32_t r = samples[0];
        uint32_t g = layout->type == 0u || layout->type == 4u ? r : samples[1];
        uint32_t b = layout->type == 0u || layout->type == 4u ? r : samples[2];
        uint32_t a = layout->type == 4u ? samples[1] : samples[3];
        /* Compare the complete key before rescaling, especially at 16 bits. */
        if (colours->have_key && r == colours->key[0] &&
            (layout->type == 0u || (g == colours->key[1] && b == colours->key[2])))
        {
            a = 0u;
        }
        red = (r * 255u + maximum / 2u) / maximum;
        green = (g * 255u + maximum / 2u) / maximum;
        blue = (b * 255u + maximum / 2u) / maximum;
        alpha = (a * 255u + maximum / 2u) / maximum;
    }
    if (!keep_alpha)
    {
        red = (red * alpha + 255u * (255u - alpha) + 127u) / 255u;
        green = (green * alpha + 255u * (255u - alpha) + 127u) / 255u;
        blue = (blue * alpha + 255u * (255u - alpha) + 127u) / 255u;
    }
    *pixel = (red << 16) | (green << 8) | blue | (keep_alpha ? alpha << 24 : 0u);
    return 1;
}

/* Decodes into caller-owned `dest`, or returns allocated pixels through `out`. */
static int png_decode(const unsigned char *data, size_t size, struct sx_png_image *out, int keep_alpha,
                      uint32_t *dest, size_t dest_count)
{
    static const unsigned char k_signature[8] = {137, 80, 78, 71, 13, 10, 26, 10};
    struct png_layout layout;
    struct png_colours colours;
    size_t pos;
    int have_header = 0, have_palette = 0, have_trns = 0;
    int have_data = 0, data_finished = 0, have_end = 0;
    unsigned char *packed = 0;
    size_t packed_size = 0, packed_capacity = 0;
    unsigned char *raw = 0;
    uint32_t *pixels = 0;
    size_t raw_size = 0u, raw_pos = 0u, produced = 0u;
    unsigned pass_count, pass_index;
    int ok = 0;

    if (out == 0 && dest == 0) return -1;
    if (out != 0)
    {
        out->pixels = 0;
        out->width = 0u;
        out->height = 0u;
    }
    if (data == 0 || size < 8u || memcmp(data, k_signature, 8) != 0) return -1;
    memset(&colours, 0, sizeof(colours));
    pos = 8u;
    while (size - pos >= 12u)
    {
        uint32_t length = read_u32be(data + pos);
        uint32_t type = read_u32be(data + pos + 4u);
        const unsigned char *body = data + pos + 8u;
        if (length > size - pos - 12u) goto done;
        if (have_data && type != 0x49444154u) data_finished = 1;
        if (type == 0x49484452u) /* IHDR */
        {
            if (have_header || pos != 8u || length != 13u || !png_header(body, &layout)) goto done;
            if (dest != 0 && dest_count < (size_t)layout.width * layout.height) goto done;
            have_header = 1;
        }
        else if (!have_header)
        {
            goto done;
        }
        else if (type == 0x504c5445u) /* PLTE */
        {
            uint32_t i;
            if (have_palette || have_trns || have_data || layout.type == 0u || layout.type == 4u ||
                length == 0u || length % 3u != 0u || length > 768u ||
                (layout.type == 3u && length / 3u > (1u << layout.depth))) goto done;
            colours.palette_count = length / 3u;
            for (i = 0u; i < colours.palette_count; ++i)
            {
                colours.palette[i] = 0xff000000u | ((uint32_t)body[i * 3u] << 16) |
                                     ((uint32_t)body[i * 3u + 1u] << 8) | body[i * 3u + 2u];
            }
            have_palette = 1;
        }
        else if (type == 0x74524e53u) /* tRNS */
        {
            uint32_t i;
            uint32_t mask = (1u << layout.depth) - 1u;
            if (have_trns || have_data) goto done;
            if (layout.type == 3u)
            {
                if (!have_palette || length > colours.palette_count) goto done;
                for (i = 0u; i < length; ++i)
                {
                    colours.palette[i] = (colours.palette[i] & 0xffffffu) | ((uint32_t)body[i] << 24);
                }
            }
            else if (layout.type == 0u || layout.type == 2u)
            {
                unsigned count = layout.type == 0u ? 1u : 3u;
                if (length != count * 2u) goto done;
                for (i = 0u; i < count; ++i) colours.key[i] = read_u16be(body + i * 2u) & mask;
                colours.have_key = 1;
            }
            else goto done;
            have_trns = 1;
        }
        else if (type == 0x49444154u) /* IDAT */
        {
            if (data_finished || (layout.type == 3u && !have_palette) ||
                !append_bytes(&packed, &packed_size, &packed_capacity, body, length)) goto done;
            have_data = 1;
        }
        else if (type == 0x49454e44u) /* IEND */
        {
            if (length != 0u || !have_data) goto done;
            have_end = 1;
            break;
        }
        else if ((type & 0x20000000u) == 0u) /* unknown critical chunk */
        {
            goto done;
        }
        pos += 12u + (size_t)length;
    }
    if (!have_end || packed_size == 0u) goto done;
    pass_count = layout.interlace ? 7u : 1u;
    for (pass_index = 0u; pass_index < pass_count; ++pass_index)
    {
        struct png_pass pass;
        png_pass_layout(&layout, pass_index, &pass);
        if (pass.width != 0u && pass.height != 0u)
        {
            size_t stride = 1u + ((size_t)pass.width * layout.channels * layout.depth + 7u) / 8u;
            raw_size += stride * pass.height;
        }
    }
    /* The 4096-per-side bound keeps every raw/output size below SIZE_MAX,
     * even for RGBA16 and the extra filter bytes of the seven passes. */
    raw = (unsigned char *)malloc(raw_size);
    if (raw == 0 || !sx_zlib_decompress(packed, packed_size, raw, raw_size, &produced) ||
        produced != raw_size) goto done;
    pixels = dest != 0 ? dest : (uint32_t *)malloc((size_t)layout.width * layout.height * 4u);
    if (pixels == 0) goto done;
    for (pass_index = 0u; pass_index < pass_count; ++pass_index)
    {
        struct png_pass pass;
        unsigned char *previous = 0;
        size_t row_bytes, bpp;
        uint32_t y;
        png_pass_layout(&layout, pass_index, &pass);
        if (pass.width == 0u || pass.height == 0u) continue;
        row_bytes = ((size_t)pass.width * layout.channels * layout.depth + 7u) / 8u;
        bpp = ((size_t)layout.channels * layout.depth + 7u) / 8u;
        for (y = 0u; y < pass.height; ++y)
        {
            unsigned char filter = raw[raw_pos++];
            unsigned char *current = raw + raw_pos;
            uint32_t x;
            if (!png_unfilter(current, previous, row_bytes, bpp, filter)) goto done;
            for (x = 0u; x < pass.width; ++x)
            {
                size_t target = (size_t)(pass.y + y * pass.dy) * layout.width + pass.x + x * pass.dx;
                if (!png_pixel(&layout, &colours, current, x, keep_alpha, &pixels[target])) goto done;
            }
            previous = current;
            raw_pos += row_bytes;
        }
    }
    if (dest == 0)
    {
        out->pixels = pixels;
        out->width = layout.width;
        out->height = layout.height;
    }
    pixels = 0;
    ok = 1;

done:
    free(packed);
    free(raw);
    if (pixels != dest) free(pixels);
    return ok ? 0 : -1;
}

void sx_png_free(struct sx_png_image *image)
{
    if (image == 0)
    {
        return;
    }
    free(image->pixels);
    image->pixels = 0;
    image->width = 0u;
    image->height = 0u;
}

int sx_png_decode(const unsigned char *data, size_t size, struct sx_png_image *out)
{
    return png_decode(data, size, out, 0, 0, 0u);
}

int sx_png_decode_argb(const unsigned char *data, size_t size, struct sx_png_image *out)
{
    return png_decode(data, size, out, 1, 0, 0u);
}

int sx_png_info(const unsigned char *data, size_t size, uint32_t *width, uint32_t *height)
{
    static const unsigned char k_signature[8] = {137, 80, 78, 71, 13, 10, 26, 10};
    struct png_layout layout;

    /* Only the signature and complete IHDR are needed; decode validates the
     * remaining chunks, compressed data and palette indices. */
    if (data == 0 || size < 33u || memcmp(data, k_signature, 8) != 0 ||
        read_u32be(data + 8) != 13u || read_u32be(data + 12) != 0x49484452u ||
        !png_header(data + 16u, &layout))
    {
        return -1;
    }
    if (width != 0) *width = layout.width;
    if (height != 0) *height = layout.height;
    return 0;
}

int sx_png_decode_into(const unsigned char *data, size_t size, uint32_t *pixels, size_t pixel_count)
{
    if (pixels == 0)
    {
        return -1;
    }
    return png_decode(data, size, 0, 0, pixels, pixel_count);
}

/* --- BMP ------------------------------------------------------------------ */

/* The file header is 14 bytes; BITMAPINFOHEADER, the smallest header this
 * decoder takes, is 40. */
#define BMP_FILE_HEADER_BYTES 14u
#define BMP_INFO_HEADER_BYTES 40u

struct bmp_channel
{
    uint32_t mask;
    uint32_t maximum;
    unsigned shift;
};

struct bmp_layout
{
    uint32_t width;
    uint32_t height;
    uint32_t bits;
    uint32_t compression;
    uint32_t palette_count;
    size_t palette_offset;
    size_t stride;
    size_t pixel_offset;
    size_t pixel_bytes;
    int top_down;
    struct bmp_channel channels[4]; /* red, green, blue, optional alpha */
};

static uint32_t read_u16le(const unsigned char *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8);
}

static uint32_t read_u32le(const unsigned char *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int bmp_channel_init(struct bmp_channel *channel, uint32_t mask, uint32_t bits)
{
    channel->mask = mask;
    channel->shift = 0u;
    if (mask == 0u || (bits == 16u && (mask >> 16) != 0u))
    {
        return -1;
    }
    while ((mask & 1u) == 0u)
    {
        ++channel->shift;
        mask >>= 1;
    }
    channel->maximum = mask;
    /* A component is one contiguous run of bits. Unsigned wrap is intentional. */
    return (mask & (mask + 1u)) == 0u ? 0 : -1;
}

/* Validates the layout and the extent of the rows or compressed stream.
 * Palette indices and RLE instructions are checked while decoding. */
static int bmp_parse(const unsigned char *data, size_t size, struct bmp_layout *layout)
{
    uint32_t header_bytes;
    uint32_t bits;
    uint32_t compression;
    uint32_t used_masks = 0u;
    int64_t width;
    int64_t height;
    size_t metadata_end;
    size_t offset;
    unsigned i;

    if (data == 0 || size < BMP_FILE_HEADER_BYTES + BMP_INFO_HEADER_BYTES || data[0] != 'B' ||
        data[1] != 'M')
    {
        return -1;
    }
    memset(layout, 0, sizeof(*layout));
    header_bytes = read_u32le(data + 14);
    bits = read_u16le(data + 28);
    compression = read_u32le(data + 30);
    if (header_bytes < BMP_INFO_HEADER_BYTES || header_bytes > size - BMP_FILE_HEADER_BYTES ||
        read_u16le(data + 26) != 1u ||
        (bits != 1u && bits != 4u && bits != 8u && bits != 16u && bits != 24u && bits != 32u) ||
        (compression != 0u && !(compression == 1u && bits == 8u) &&
         !(compression == 2u && bits == 4u) &&
         !(compression == 3u && (bits == 16u || bits == 32u))))
    {
        return -1;
    }
    width = (int32_t)read_u32le(data + 18);
    height = (int32_t)read_u32le(data + 22);
    layout->top_down = height < 0;
    if (height < 0)
    {
        height = -height;
    }
    if (width <= 0 || height <= 0 || width > (int64_t)SX_BMP_MAX_DIMENSION ||
        height > (int64_t)SX_BMP_MAX_DIMENSION ||
        (layout->top_down && (compression == 1u || compression == 2u)))
    {
        return -1;
    }
    layout->width = (uint32_t)width;
    layout->height = (uint32_t)height;
    layout->bits = bits;
    layout->compression = compression;
    metadata_end = BMP_FILE_HEADER_BYTES + (size_t)header_bytes;
    offset = read_u32le(data + 10);
    if (offset < metadata_end || offset > size)
    {
        return -1;
    }
    if (compression == 3u)
    {
        /* INFO stores three masks after its header; V2/V3/V4/V5 embed them. */
        if (header_bytes == 40u)
        {
            if (offset - metadata_end < 12u) return -1;
            metadata_end += 12u;
        }
        else if (header_bytes < 52u)
        {
            return -1;
        }
        for (i = 0u; i < 4u; ++i)
        {
            uint32_t mask = i == 3u && header_bytes < 56u ? 0u : read_u32le(data + 54u + i * 4u);
            if (i == 3u && mask == 0u) continue;
            if ((mask & used_masks) != 0u || bmp_channel_init(&layout->channels[i], mask, bits) != 0)
            {
                return -1;
            }
            used_masks |= mask;
        }
    }
    else if (bits == 16u)
    {
        bmp_channel_init(&layout->channels[0], 0x7c00u, bits);
        bmp_channel_init(&layout->channels[1], 0x03e0u, bits);
        bmp_channel_init(&layout->channels[2], 0x001fu, bits);
    }
    if (bits <= 8u)
    {
        uint32_t count = read_u32le(data + 46);
        if (count == 0u) count = 1u << bits;
        if (count > (1u << bits) || (size_t)count * 4u > offset - metadata_end)
        {
            return -1;
        }
        layout->palette_count = count;
        layout->palette_offset = metadata_end;
    }
    layout->pixel_offset = offset;
    if (compression == 1u || compression == 2u)
    {
        layout->pixel_bytes = read_u32le(data + 34);
        if (layout->pixel_bytes < 2u || layout->pixel_bytes > size - offset) return -1;
    }
    else
    {
        layout->stride = (((size_t)layout->width * bits + 31u) / 32u) * 4u;
        layout->pixel_bytes = layout->stride * layout->height;
        if (layout->pixel_bytes > size - offset) return -1;
    }
    return 0;
}

static uint32_t bmp_palette_pixel(const unsigned char *data, const struct bmp_layout *layout,
                                  uint32_t index, int keep_alpha)
{
    const unsigned char *entry = data + layout->palette_offset + index * 4u;
    /* RGBQUAD's reserved byte is not alpha. */
    return ((uint32_t)entry[2] << 16) | ((uint32_t)entry[1] << 8) | entry[0] |
           (keep_alpha ? 0xff000000u : 0u);
}

static uint32_t bmp_component(uint32_t pixel, const struct bmp_channel *channel)
{
    uint32_t value = (pixel & channel->mask) >> channel->shift;
    return (uint32_t)(((uint64_t)value * 255u + channel->maximum / 2u) / channel->maximum);
}

static int bmp_rle(const unsigned char *data, const struct bmp_layout *layout,
                   uint32_t *pixels, int keep_alpha)
{
    const unsigned char *stream = data + layout->pixel_offset;
    size_t at = 0u;
    uint32_t x = 0u, y = 0u;
    size_t i;
    /* Early EOB, short rows and delta skips leave palette entry zero. */
    for (i = 0u; i < (size_t)layout->width * layout->height; ++i)
    {
        pixels[i] = bmp_palette_pixel(data, layout, 0u, keep_alpha);
    }
    while (layout->pixel_bytes - at >= 2u)
    {
        uint32_t count = stream[at++];
        uint32_t code = stream[at++];
        size_t literal_bytes = 0u;
        size_t padded_bytes = 0u;
        if (count == 0u)
        {
            if (code == 1u) return 0; /* end of bitmap */
            if (y >= layout->height) return -1;
            if (code == 0u)
            {
                x = 0u;
                ++y;
                continue;
            }
            if (code == 2u)
            {
                if (layout->pixel_bytes - at < 2u) return -1;
                x += stream[at++];
                y += stream[at++];
                if (x > layout->width || y >= layout->height) return -1;
                continue;
            }
            count = code;
            literal_bytes = layout->bits == 8u ? count : (count + 1u) / 2u;
            padded_bytes = (literal_bytes + 1u) & ~(size_t)1u;
            if (padded_bytes > layout->pixel_bytes - at) return -1;
        }
        if (y >= layout->height || count > layout->width - x) return -1;
        for (i = 0u; i < count; ++i)
        {
            uint32_t index;
            uint32_t packed = literal_bytes ? stream[at + (layout->bits == 8u ? i : i / 2u)] : code;
            index = layout->bits == 8u ? packed : ((i & 1u) ? packed & 15u : packed >> 4);
            if (index >= layout->palette_count) return -1;
            pixels[(size_t)(layout->height - 1u - y) * layout->width + x++] =
                bmp_palette_pixel(data, layout, index, keep_alpha);
        }
        at += padded_bytes;
    }
    return -1; /* An explicit EOB is required, even after the last full row. */
}

int sx_bmp_info(const unsigned char *data, size_t size, uint32_t *width, uint32_t *height)
{
    struct bmp_layout layout;

    if (bmp_parse(data, size, &layout) != 0)
    {
        return -1;
    }
    if (width != 0)
    {
        *width = layout.width;
    }
    if (height != 0)
    {
        *height = layout.height;
    }
    return 0;
}

static int bmp_decode_into(const unsigned char *data, size_t size, uint32_t *pixels,
                           size_t pixel_count, int keep_alpha)
{
    struct bmp_layout layout;
    uint32_t y;
    if (pixels == 0 || bmp_parse(data, size, &layout) != 0 ||
        pixel_count < (size_t)layout.width * layout.height)
    {
        return -1;
    }
    if (layout.compression == 1u || layout.compression == 2u)
    {
        return bmp_rle(data, &layout, pixels, keep_alpha);
    }
    for (y = 0u; y < layout.height; ++y)
    {
        uint32_t source_row = layout.top_down ? y : layout.height - 1u - y;
        const unsigned char *source = data + layout.pixel_offset + (size_t)source_row * layout.stride;
        uint32_t *target = pixels + (size_t)y * layout.width;
        uint32_t x;
        for (x = 0u; x < layout.width; ++x)
        {
            uint32_t red, green, blue, alpha = 255u;
            if (layout.bits <= 8u)
            {
                uint32_t bit = x * layout.bits;
                uint32_t index = (source[bit / 8u] >> (8u - layout.bits - bit % 8u)) &
                                 ((1u << layout.bits) - 1u);
                if (index >= layout.palette_count) return -1;
                target[x] = bmp_palette_pixel(data, &layout, index, keep_alpha);
                continue;
            }
            if (layout.bits == 16u || layout.compression == 3u)
            {
                const unsigned char *p = source + (size_t)x * (layout.bits / 8u);
                uint32_t value = layout.bits == 16u ? read_u16le(p) : read_u32le(p);
                red = bmp_component(value, &layout.channels[0]);
                green = bmp_component(value, &layout.channels[1]);
                blue = bmp_component(value, &layout.channels[2]);
                if (layout.channels[3].mask) alpha = bmp_component(value, &layout.channels[3]);
            }
            else
            {
                const unsigned char *p = source + (size_t)x * (layout.bits / 8u);
                red = p[2]; green = p[1]; blue = p[0];
            }
            if (!keep_alpha && alpha != 255u)
            {
                red = (red * alpha + 255u * (255u - alpha) + 127u) / 255u;
                green = (green * alpha + 255u * (255u - alpha) + 127u) / 255u;
                blue = (blue * alpha + 255u * (255u - alpha) + 127u) / 255u;
            }
            target[x] = (red << 16) | (green << 8) | blue | (keep_alpha ? alpha << 24 : 0u);
        }
    }
    return 0;
}

int sx_bmp_decode_into(const unsigned char *data, size_t size, uint32_t *pixels, size_t pixel_count)
{
    return bmp_decode_into(data, size, pixels, pixel_count, 0);
}

int sx_bmp_decode_argb_into(const unsigned char *data, size_t size, uint32_t *pixels, size_t pixel_count)
{
    return bmp_decode_into(data, size, pixels, pixel_count, 1);
}
