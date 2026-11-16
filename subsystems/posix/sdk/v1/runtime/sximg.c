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
        default: return SX_IMAGE_ERROR_HEADER;
    }
    if ((depth != 1u && depth != 2u && depth != 4u && depth != 8u && depth != 16u) ||
        (type == 3u && depth == 16u) || (type != 0u && type != 3u && depth < 8u) ||
        body[10] != 0u || body[11] != 0u || body[12] > 1u)
    {
        return SX_IMAGE_ERROR_HEADER;
    }
    layout->width = read_u32be(body);
    layout->height = read_u32be(body + 4u);
    layout->depth = depth;
    layout->type = type;
    layout->channels = channels;
    layout->interlace = body[12];
    if (layout->width == 0u || layout->height == 0u)
    {
        return SX_IMAGE_ERROR_HEADER;
    }
    if (layout->width > SX_PNG_MAX_DIMENSION || layout->height > SX_PNG_MAX_DIMENSION)
    {
        return SX_IMAGE_ERROR_LIMIT;
    }
    return SX_IMAGE_OK;
}

static int png_read_header(const unsigned char *data, size_t size, struct png_layout *layout)
{
    static const unsigned char signature[8] = {137, 80, 78, 71, 13, 10, 26, 10};
    if (data == 0 || size < 33u || memcmp(data, signature, 8) != 0 ||
        read_u32be(data + 8) != 13u || read_u32be(data + 12) != 0x49484452u)
    {
        return SX_IMAGE_ERROR_HEADER;
    }
    return png_header(data + 16u, layout);
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
            if (have_header || pos != 8u || length != 13u || png_header(body, &layout) != SX_IMAGE_OK) goto done;
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
    struct png_layout layout;

    /* Only the signature and complete IHDR are needed; decode validates the
     * remaining chunks, compressed data and palette indices. */
    if (png_read_header(data, size, &layout) != SX_IMAGE_OK)
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
    if (width <= 0 || height <= 0 ||
        (layout->top_down && (compression == 1u || compression == 2u)))
    {
        return -1;
    }
    if (width > (int64_t)SX_BMP_MAX_DIMENSION || height > (int64_t)SX_BMP_MAX_DIMENSION)
    {
        return SX_IMAGE_ERROR_LIMIT;
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

/* --- Sequential JPEG (T.81, Annexes B/C/F and A.3.3) ------------------------ */

static const unsigned char jpeg_zigzag[64] = {
    0,1,8,16,9,2,3,10,17,24,32,25,18,11,4,5,
    12,19,26,33,40,48,41,34,27,20,13,6,7,14,21,28,
    35,42,49,56,57,50,43,36,29,22,15,23,30,37,44,51,
    58,59,52,45,38,31,39,46,53,60,61,54,47,55,62,63
};

struct jpeg_huffman
{
    uint16_t count[17];
    uint32_t first[17];
    uint16_t offset[17];
    unsigned char symbols[256];
    unsigned valid;
};

struct jpeg_component
{
    unsigned id, h, v, quant_id, dc_id, ac_id;
    uint32_t width, height, sample_width, sample_height;
    unsigned char *samples;
};

struct jpeg_state
{
    const unsigned char *data;
    size_t size, entropy;
    uint32_t width, height, output_width, output_height;
    unsigned scale_denom, frame, components, hmax, vmax;
    struct jpeg_component component[3];
    unsigned scan_count, scan[3], decoded;
    unsigned restart_interval, jfif, adobe, transform;
    uint16_t quant[4][64];
    unsigned quant_present[4], quant_wide[4];
    struct jpeg_huffman huff[2][4];
};

struct jpeg_reader
{
    const unsigned char *data;
    size_t size, pos;
    unsigned byte, remaining;
};

/* JPEG canonical codes are MSB-first and reserve the all-ones code, which
 * makes one-bit padding distinguishable from actual Huffman symbols. */
static int jpeg_huffman_build(struct jpeg_huffman *table, const unsigned char *counts,
                              const unsigned char *symbols, size_t total, unsigned ac)
{
    unsigned seen[256] = {0};
    uint32_t code = 0u;
    size_t offset = 0u;
    unsigned length;
    if (total == 0u || total > 256u) return 0;
    memset(table, 0, sizeof(*table));
    for (length = 1u; length <= 16u; ++length)
    {
        unsigned count = counts[length - 1u];
        size_t i;
        if (offset + count > total || code + count >= (1u << length)) return 0;
        table->count[length] = (uint16_t)count;
        table->first[length] = code;
        table->offset[length] = (uint16_t)offset;
        for (i = offset; i < offset + count; ++i)
        {
            unsigned symbol = symbols[i];
            unsigned bits = symbol & 15u;
            if (seen[symbol]++ || (!ac && symbol > 11u) ||
                (ac && (bits > 10u || (bits == 0u && symbol != 0u && symbol != 0xf0u)))) return 0;
            table->symbols[i] = (unsigned char)symbol;
        }
        code = (code + count) << 1;
        offset += count;
    }
    if (offset != total) return 0;
    table->valid = 1u;
    return 1;
}

static int jpeg_marker(const unsigned char *data, size_t size, size_t *pos, unsigned *marker)
{
    if (*pos >= size || data[(*pos)++] != 0xffu) return 0;
    while (*pos < size && data[*pos] == 0xffu) ++*pos;
    if (*pos >= size || data[*pos] == 0u) return 0;
    *marker = data[(*pos)++];
    return 1;
}

static int jpeg_segment(const unsigned char *data, size_t size, size_t *pos,
                        const unsigned char **body, size_t *length)
{
    uint32_t bytes;
    if (size - *pos < 2u) return 0;
    bytes = read_u16be(data + *pos);
    if (bytes < 2u || bytes > size - *pos) return 0;
    *body = data + *pos + 2u;
    *length = bytes - 2u;
    *pos += bytes;
    return 1;
}

static int jpeg_quant_tables(struct jpeg_state *state, const unsigned char *body, size_t length)
{
    size_t at = 0u;
    if (length == 0u) return 0;
    while (at < length)
    {
        unsigned spec = body[at++], precision = spec >> 4, id = spec & 15u;
        unsigned i;
        if (precision > 1u || id > 3u || length - at < 64u * (precision + 1u)) return 0;
        for (i = 0u; i < 64u; ++i)
        {
            uint32_t value = precision ? read_u16be(body + at) : body[at];
            if (value == 0u) return 0;
            state->quant[id][jpeg_zigzag[i]] = (uint16_t)value;
            at += precision + 1u;
        }
        state->quant_present[id] = 1u;
        state->quant_wide[id] = precision;
    }
    return 1;
}

static int jpeg_huffman_tables(struct jpeg_state *state, const unsigned char *body, size_t length)
{
    size_t at = 0u;
    if (length == 0u) return 0;
    while (at < length)
    {
        unsigned spec = body[at++], ac = spec >> 4, id = spec & 15u;
        size_t total = 0u;
        unsigned i;
        const unsigned char *counts;
        if (ac > 1u || id > 3u || length - at < 16u) return 0;
        counts = body + at;
        for (i = 0u; i < 16u; ++i) total += counts[i];
        at += 16u;
        if (total > length - at ||
            !jpeg_huffman_build(&state->huff[ac][id], counts, body + at, total, ac)) return 0;
        at += total;
    }
    return 1;
}

static int jpeg_process_error(unsigned marker)
{
    if (marker == 0xc2u || marker == 0xc6u) return SX_IMAGE_ERROR_JPEG_PROGRESSIVE;
    if (marker == 0xc9u || marker == 0xcau || marker == 0xcbu || marker == 0xcdu ||
        marker == 0xceu || marker == 0xcfu || marker == 0xccu) return SX_IMAGE_ERROR_JPEG_ARITHMETIC;
    if (marker == 0xc3u || marker == 0xc7u) return SX_IMAGE_ERROR_JPEG_LOSSLESS;
    return SX_IMAGE_ERROR_UNSUPPORTED;
}

/* Colour is a file convention outside T.81. JFIF takes precedence over
 * Adobe; otherwise APP14 or the ordered R/G/B identifiers select RGB. */
static int jpeg_rgb(const struct jpeg_state *state)
{
    if (state->jfif) return 0;
    if (state->adobe) return state->transform == 0u;
    return state->component[0].id == 'R' && state->component[1].id == 'G' &&
           state->component[2].id == 'B';
}

static int jpeg_application(struct jpeg_state *state, unsigned marker,
                            const unsigned char *body, size_t length)
{
    if (marker == 0xe0u && length >= 5u && memcmp(body, "JFIF\0", 5u) == 0)
    {
        if (length < 14u || length < 14u + (size_t)body[12] * body[13] * 3u)
            return SX_IMAGE_ERROR_HEADER;
        state->jfif = 1u;
    }
    if (marker == 0xeeu && length >= 5u && memcmp(body, "Adobe", 5u) == 0)
    {
        if (length < 12u) return SX_IMAGE_ERROR_HEADER;
        if (body[11] > 1u) return SX_IMAGE_ERROR_JPEG_COLOR;
        if (state->adobe && state->transform != body[11]) return SX_IMAGE_ERROR_JPEG_COLOR;
        state->adobe = 1u;
        state->transform = body[11];
    }
    return SX_IMAGE_OK;
}

/* Scan selectors must be unique, in frame order for interleaved scans, and
 * each sequential component occurs exactly once in the complete frame. */
static int jpeg_scan_header(struct jpeg_state *state, const unsigned char *body, size_t length)
{
    unsigned i, blocks = 0u, mask = state->decoded, count;
    if (!state->frame || length < 1u) return SX_IMAGE_ERROR_HEADER;
    count = body[0];
    if (count == 0u || count > state->components || length != 4u + 2u * count ||
        body[1u + 2u * count] != 0u || body[2u + 2u * count] != 63u ||
        body[3u + 2u * count] != 0u) return SX_IMAGE_ERROR_HEADER;
    for (i = 0u; i < count; ++i)
    {
        unsigned c, dc = body[2u + 2u * i] >> 4, ac = body[2u + 2u * i] & 15u;
        struct jpeg_component *component;
        for (c = 0u; c < state->components && state->component[c].id != body[1u + 2u * i]; ++c) {}
        if (c == state->components || (mask & (1u << c)) ||
            (i != 0u && c <= state->scan[i - 1u])) return SX_IMAGE_ERROR_HEADER;
        component = &state->component[c];
        if (dc > 3u || ac > 3u || (state->frame == 0xc0u && (dc > 1u || ac > 1u)) ||
            !state->huff[0][dc].valid || !state->huff[1][ac].valid ||
            !state->quant_present[component->quant_id] ||
            (state->frame == 0xc0u && state->quant_wide[component->quant_id])) return SX_IMAGE_ERROR_HEADER;
        component->dc_id = dc; component->ac_id = ac;
        state->scan[i] = c;
        mask |= 1u << c;
        blocks += component->h * component->v;
    }
    if (count > 1u && blocks > 10u) return SX_IMAGE_ERROR_JPEG_SAMPLING;
    state->scan_count = count;
    return SX_IMAGE_OK;
}

/* Returns 0 at SOS, 1 at a complete EOI, or a public error. Info stops at
 * the first SOS; decode reuses this parser for tables and subsequent scans. */
static int jpeg_headers(struct jpeg_state *state, size_t pos)
{
    while (pos < state->size)
    {
        unsigned marker;
        size_t length;
        const unsigned char *body;
        if (!jpeg_marker(state->data, state->size, &pos, &marker)) return SX_IMAGE_ERROR_HEADER;
        if (marker == 0xd9u)
            return state->components && state->decoded == (1u << state->components) - 1u ? 1 : SX_IMAGE_ERROR_HEADER;
        if (marker == 0xd8u || marker == 1u ||
            (marker >= 0xd0u && marker <= 0xd7u)) return SX_IMAGE_ERROR_HEADER;
        if (!jpeg_segment(state->data, state->size, &pos, &body, &length)) return SX_IMAGE_ERROR_HEADER;
        if (marker == 0xc0u || marker == 0xc1u)
        {
            unsigned c;
            if (state->frame || length < 6u || body[5] == 0u ||
                length != 6u + (size_t)body[5] * 3u) return SX_IMAGE_ERROR_HEADER;
            if (body[0] != 8u) return SX_IMAGE_ERROR_JPEG_PRECISION;
            if (body[5] != 1u && body[5] != 3u) return SX_IMAGE_ERROR_JPEG_COLOR;
            state->height = read_u16be(body + 1u);
            state->width = read_u16be(body + 3u);
            if (state->width == 0u || state->height == 0u) return SX_IMAGE_ERROR_HEADER;
            state->components = body[5];
            for (c = 0u; c < state->components; ++c)
            {
                unsigned i, sampling = body[7u + 3u * c];
                struct jpeg_component *component = &state->component[c];
                component->id = body[6u + 3u * c]; component->quant_id = body[8u + 3u * c];
                component->h = sampling >> 4; component->v = sampling & 15u;
                if (component->quant_id > 3u) return SX_IMAGE_ERROR_HEADER;
                for (i = 0u; i < c; ++i)
                    if (component->id == state->component[i].id) return SX_IMAGE_ERROR_HEADER;
                if (component->h < 1u || component->h > 2u || component->v < 1u || component->v > 2u)
                    return SX_IMAGE_ERROR_JPEG_SAMPLING;
                if (component->h > state->hmax) state->hmax = component->h;
                if (component->v > state->vmax) state->vmax = component->v;
            }
            for (c = 0u; c < state->components; ++c)
            {
                struct jpeg_component *component = &state->component[c];
                component->width = (state->width * component->h + state->hmax - 1u) / state->hmax;
                component->height = (state->height * component->v + state->vmax - 1u) / state->vmax;
            }
            state->frame = marker;
        }
        else if (marker == 0xdbu)
        {
            if (!jpeg_quant_tables(state, body, length)) return SX_IMAGE_ERROR_HEADER;
        }
        else if (marker == 0xc4u)
        {
            if (!jpeg_huffman_tables(state, body, length)) return SX_IMAGE_ERROR_HEADER;
        }
        else if (marker == 0xddu)
        {
            if (length != 2u) return SX_IMAGE_ERROR_HEADER;
            state->restart_interval = read_u16be(body);
        }
        else if (marker == 0xdau)
        {
            int result = jpeg_scan_header(state, body, length);
            if (result != SX_IMAGE_OK) return result;
            state->entropy = pos;
            return SX_IMAGE_OK;
        }
        else if ((marker >= 0xe0u && marker <= 0xefu) || marker == 0xfeu)
        {
            int result = jpeg_application(state, marker, body, length);
            if (result != SX_IMAGE_OK) return result;
        }
        else return jpeg_process_error(marker);
    }
    return SX_IMAGE_ERROR_HEADER;
}

static int jpeg_prepare(const unsigned char *data, size_t size, struct jpeg_state *state)
{
    if (size < 2u || data[0] != 0xffu || data[1] != 0xd8u) return SX_IMAGE_ERROR_HEADER;
    memset(state, 0, sizeof(*state));
    state->data = data;
    state->size = size;
    return jpeg_headers(state, 2u);
}

static int jpeg_bits(struct jpeg_reader *reader, unsigned count, uint32_t *value)
{
    unsigned i;
    uint32_t result = 0u;
    if (count > 16u) return 0;
    for (i = 0u; i < count; ++i)
    {
        if (reader->remaining == 0u)
        {
            if (reader->pos >= reader->size) return 0;
            reader->byte = reader->data[reader->pos++];
            if (reader->byte == 0xffu)
            {
                if (reader->pos >= reader->size || reader->data[reader->pos++] != 0u) return 0;
            }
            reader->remaining = 8u;
        }
        result = (result << 1) | ((reader->byte >> --reader->remaining) & 1u);
    }
    *value = result;
    return 1;
}

static int jpeg_symbol(struct jpeg_reader *reader, const struct jpeg_huffman *table, unsigned *symbol)
{
    uint32_t code = 0u;
    unsigned length;
    for (length = 1u; length <= 16u; ++length)
    {
        uint32_t bit;
        if (!jpeg_bits(reader, 1u, &bit)) return 0;
        code = (code << 1) | bit;
        if (code >= table->first[length] && code - table->first[length] < table->count[length])
        {
            *symbol = table->symbols[table->offset[length] + code - table->first[length]];
            return 1;
        }
    }
    return 0;
}

static int jpeg_receive(struct jpeg_reader *reader, unsigned count, int32_t *value)
{
    uint32_t bits;
    if (!jpeg_bits(reader, count, &bits)) return 0;
    *value = (int32_t)bits;
    if (count != 0u && bits < (1u << (count - 1u))) *value -= (int32_t)((1u << count) - 1u);
    return 1;
}

static int jpeg_block(struct jpeg_reader *reader, const struct jpeg_huffman *dc,
                      const struct jpeg_huffman *ac, int32_t *predictor, int32_t block[64])
{
    unsigned symbol, k = 1u;
    int32_t difference;
    memset(block, 0, 64u * sizeof(*block));
    if (!jpeg_symbol(reader, dc, &symbol) || symbol > 11u ||
        !jpeg_receive(reader, symbol, &difference)) return 0;
    difference += *predictor;
    if (difference < -1024 || difference > 1023) return 0;
    *predictor = block[0] = difference;
    while (k < 64u)
    {
        unsigned run, count;
        int32_t value;
        if (!jpeg_symbol(reader, ac, &symbol)) return 0;
        run = symbol >> 4; count = symbol & 15u;
        if (count == 0u)
        {
            if (run == 0u) break;
            if (run != 15u || k + 16u > 64u) return 0;
            k += 16u;
        }
        else
        {
            if (count > 10u || k + run >= 64u || !jpeg_receive(reader, count, &value)) return 0;
            k += run;
            block[jpeg_zigzag[k++]] = value;
        }
    }
    return 1;
}

static void jpeg_dequantize(int32_t block[64], const uint16_t quant[64])
{
    unsigned i;
    /* The entropy stage bounds coefficients to [-1024,1023], so even
     * 16-bit SOF1 quantizers fit in signed 32 bits after multiplication. */
    for (i = 0u; i < 64u; ++i) block[i] *= (int32_t)quant[i];
}

/* Q14 C(u)*cos((2*x+1)*u*pi/16), calculated from the T.81 IDCT definition.
 * Both separable sums use int64_t; their worst-case magnitude with validated
 * coefficients and 16-bit quantizers is below 2^61. No floating point or
 * runtime trigonometry is required by the decoder. */
static const int16_t jpeg_basis[8][8] = {
    {11585,16069,15137,13623,11585,9102,6270,3196},
    {11585,13623,6270,-3196,-11585,-16069,-15137,-9102},
    {11585,9102,-6270,-16069,-11585,3196,15137,13623},
    {11585,3196,-15137,-9102,11585,13623,-6270,-16069},
    {11585,-3196,-15137,9102,11585,-13623,-6270,16069},
    {11585,-9102,-6270,16069,-11585,-3196,15137,-13623},
    {11585,-13623,6270,3196,-11585,16069,-15137,9102},
    {11585,-16069,15137,-13623,11585,-9102,6270,-3196},
};

static int64_t jpeg_round(int64_t value, int64_t divisor)
{
    return value < 0 ? -((-value + divisor / 2) / divisor) : (value + divisor / 2) / divisor;
}

static unsigned char jpeg_clamp(int64_t value)
{
    return value < 0 ? 0u : value > 255 ? 255u : (unsigned char)value;
}

static void jpeg_idct(const int32_t block[64], unsigned char pixels[64])
{
    int64_t temp[64];
    unsigned x, y, u, v;
    int dc_only = 1;
    for (u = 1u; u < 64u; ++u) if (block[u] != 0) dc_only = 0;
    if (dc_only)
    {
        memset(pixels, jpeg_clamp(jpeg_round(block[0], 8) + 128), 64u);
        return;
    }
    for (v = 0u; v < 8u; ++v) for (x = 0u; x < 8u; ++x)
    {
        int64_t sum = 0;
        for (u = 0u; u < 8u; ++u) sum += (int64_t)block[v * 8u + u] * jpeg_basis[x][u];
        temp[v * 8u + x] = sum;
    }
    for (y = 0u; y < 8u; ++y) for (x = 0u; x < 8u; ++x)
    {
        int64_t sum = 0;
        for (v = 0u; v < 8u; ++v) sum += temp[v * 8u + x] * jpeg_basis[y][v];
        pixels[y * 8u + x] = jpeg_clamp(jpeg_round(sum, INT64_C(1) << 30) + 128);
    }
}

/* Average the T.81 spatial basis over each group of 2 or 4 samples.
 * Q14 constants are rounded from the cosine definition, not from a full
 * reconstructed block. The final division remains 4*2^28 as at scale 1. */
static const int16_t jpeg_basis_half[4][8] = {
    {11585,14846,10703,5213,0,-3483,-4433,-2953},
    {11585,6149,-10703,-12586,0,8410,4433,-1223},
    {11585,-6149,-10703,12586,0,-8410,4433,1223},
    {11585,-14846,10703,-5213,0,3483,-4433,2953},
};

static const int16_t jpeg_basis_quarter[2][8] = {
    {11585,10498,0,-3686,0,2463,0,-2088},
    {11585,-10498,0,3686,0,-2463,0,2088},
};

static void jpeg_idct_scaled(const int32_t block[64], unsigned char *pixels, unsigned denom)
{
    int64_t temp[32];
    unsigned x, y, u, v, n = 8u / denom;
    const int16_t (*basis)[8] = denom == 2u ? jpeg_basis_half : jpeg_basis_quarter;
    if (denom == 1u) { jpeg_idct(block, pixels); return; }
    if (denom == 8u)
    {
        pixels[0] = jpeg_clamp(jpeg_round(block[0], 8) + 128);
        return;
    }
    {
        int dc_only = 1;
        for (u = 1u; u < 64u; ++u) if (block[u] != 0) dc_only = 0;
        if (dc_only)
        {
            memset(pixels, jpeg_clamp(jpeg_round(block[0], 8) + 128), n * n);
            return;
        }
    }
    for (v = 0u; v < 8u; ++v) for (x = 0u; x < n; ++x)
    {
        int64_t sum = 0;
        for (u = 0u; u < 8u; ++u) sum += (int64_t)block[v * 8u + u] * basis[x][u];
        temp[v * n + x] = sum;
    }
    for (y = 0u; y < n; ++y) for (x = 0u; x < n; ++x)
    {
        int64_t sum = 0;
        for (v = 0u; v < 8u; ++v) sum += temp[v * n + x] * basis[y][v];
        pixels[y * n + x] = jpeg_clamp(jpeg_round(sum, INT64_C(1) << 30) + 128);
    }
}

static int jpeg_align(struct jpeg_reader *reader)
{
    unsigned mask = (1u << reader->remaining) - 1u;
    if ((reader->byte & mask) != mask) return 0;
    reader->remaining = 0u;
    return 1;
}

/* Decode one MCU sequence using current tables. Noninterleaved MCUs have
 * one block; interleaved MCUs carry each selected component's H*V blocks. */
static int jpeg_decode_scan(struct jpeg_state *state, uint32_t *pixels, size_t *end)
{
    struct jpeg_reader reader = {state->data, state->size, state->entropy, 0u, 0u};
    struct jpeg_component *first = &state->component[state->scan[0]];
    unsigned interleaved = state->scan_count > 1u, n = 8u / state->scale_denom;
    uint32_t across = interleaved ? (state->width + 8u * state->hmax - 1u) / (8u * state->hmax) :
                                   (first->width + 7u) / 8u;
    uint32_t down = interleaved ? (state->height + 8u * state->vmax - 1u) / (8u * state->vmax) :
                                 (first->height + 7u) / 8u;
    uint32_t index, total = across * down;
    unsigned restart = 0u;
    int32_t predictor[3] = {0, 0, 0};
    for (index = 0u; index < total; ++index)
    {
        unsigned i;
        if (index != 0u && state->restart_interval != 0u && index % state->restart_interval == 0u)
        {
            unsigned marker;
            if (!jpeg_align(&reader) || !jpeg_marker(reader.data, reader.size, &reader.pos, &marker) ||
                marker != 0xd0u + restart) return 0;
            restart = (restart + 1u) & 7u;
            memset(predictor, 0, sizeof(predictor));
        }
        for (i = 0u; i < state->scan_count; ++i)
        {
            unsigned c = state->scan[i], dx, dy;
            struct jpeg_component *component = &state->component[c];
            unsigned h = interleaved ? component->h : 1u, v = interleaved ? component->v : 1u;
            for (dy = 0u; dy < v; ++dy) for (dx = 0u; dx < h; ++dx)
            {
                int32_t block[64];
                unsigned char samples[64];
                uint32_t x, y, bx = ((index % across) * h + dx) * n;
                uint32_t by = ((index / across) * v + dy) * n;
                if (!jpeg_block(&reader, &state->huff[0][component->dc_id], &state->huff[1][component->ac_id],
                                &predictor[c], block)) return 0;
                jpeg_dequantize(block, state->quant[component->quant_id]);
                jpeg_idct_scaled(block, samples, state->scale_denom);
                for (y = 0u; y < n && by + y < component->sample_height; ++y)
                    for (x = 0u; x < n && bx + x < component->sample_width; ++x)
                    {
                        size_t at = (size_t)(by + y) * component->sample_width + bx + x;
                        if (state->components == 1u) pixels[at] = (uint32_t)samples[y * n + x] * 0x010101u;
                        else component->samples[at] = samples[y * n + x];
                    }
            }
        }
    }
    if (!jpeg_align(&reader)) return 0;
    for (index = 0u; index < state->scan_count; ++index) state->decoded |= 1u << state->scan[index];
    *end = reader.pos;
    return 1;
}

/* Q16 full-range JFIF/CCIR 601 conversion, rounded to nearest and clamped.
 * Component samples are replicated for each integer sampling ratio. */
static uint32_t jpeg_ycbcr(unsigned y, unsigned cb, unsigned cr)
{
    int32_t b = (int32_t)cb - 128, r = (int32_t)cr - 128;
    unsigned red = jpeg_clamp((int64_t)y + jpeg_round(91881 * r, 65536));
    unsigned green = jpeg_clamp((int64_t)y + jpeg_round(-22554 * b - 46802 * r, 65536));
    unsigned blue = jpeg_clamp((int64_t)y + jpeg_round(116130 * b, 65536));
    return (red << 16) | (green << 8) | blue;
}

static int jpeg_decode(struct jpeg_state *state, uint32_t *pixels)
{
    unsigned c;
    size_t size = 0u, offset = 0u, end;
    unsigned char *workspace = 0;
    int result;
    if (state->components == 3u)
    {
        /* Each reduced byte plane is at most the output area: <= 3*4096*4096 bytes.
         * Dequantize immediately so later table redefinitions are harmless. */
        for (c = 0u; c < 3u; ++c) size += (size_t)state->component[c].sample_width * state->component[c].sample_height;
        workspace = (unsigned char *)malloc(size);
        if (workspace == 0) return 0;
        for (c = 0u; c < 3u; ++c)
        {
            state->component[c].samples = workspace + offset;
            offset += (size_t)state->component[c].sample_width * state->component[c].sample_height;
        }
    }
    do
    {
        if (!jpeg_decode_scan(state, pixels, &end)) { free(workspace); return 0; }
        result = jpeg_headers(state, end);
    } while (result == SX_IMAGE_OK);
    if (result == 1 && state->components == 3u)
    {
        uint32_t x, y;
        int rgb = jpeg_rgb(state);
        for (y = 0u; y < state->output_height; ++y) for (x = 0u; x < state->output_width; ++x)
        {
            unsigned sample[3];
            for (c = 0u; c < 3u; ++c)
            {
                const struct jpeg_component *component = &state->component[c];
                sample[c] = component->samples[(size_t)(y * component->v / state->vmax) * component->sample_width +
                                               x * component->h / state->hmax];
            }
            pixels[(size_t)y * state->output_width + x] = rgb ? (sample[0] << 16) | (sample[1] << 8) | sample[2] :
                                                       jpeg_ycbcr(sample[0], sample[1], sample[2]);
        }
    }
    free(workspace);
    return result == 1;
}

/* --- Common image interface ----------------------------------------------- */

uint32_t sx_image_format(const unsigned char *data, size_t size)
{
    static const unsigned char png_signature[8] = {137, 80, 78, 71, 13, 10, 26, 10};
    if (data == 0) return SX_IMAGE_FORMAT_UNKNOWN;
    if (size >= 8u && memcmp(data, png_signature, 8u) == 0) return SX_IMAGE_FORMAT_PNG;
    if (size >= 2u && data[0] == 'B' && data[1] == 'M') return SX_IMAGE_FORMAT_BMP;
    if (size >= 2u && data[0] == 0xffu && data[1] == 0xd8u) return SX_IMAGE_FORMAT_JPEG;
    return SX_IMAGE_FORMAT_UNKNOWN;
}

static uint32_t image_limit(uint32_t requested, uint32_t maximum)
{
    return requested != 0u && requested < maximum ? requested : maximum;
}

int sx_image_info(const unsigned char *data, size_t size,
                  const struct sx_image_options *options, struct sx_image_metadata *out)
{
    struct sx_image_options defaults = {0};
    struct sx_image_metadata metadata = {0};
    int result;

    if (out == 0) return SX_IMAGE_ERROR_ARGUMENT;
    memset(out, 0, sizeof(*out));
    if (data == 0) return SX_IMAGE_ERROR_ARGUMENT;
    if (options == 0) options = &defaults;
    if (options->scale_denom != 0u && options->scale_denom != 1u && options->scale_denom != 2u &&
        options->scale_denom != 4u && options->scale_denom != 8u) return SX_IMAGE_ERROR_ARGUMENT;
    metadata.format = sx_image_format(data, size);
    switch (metadata.format)
    {
        case SX_IMAGE_FORMAT_PNG:
        {
            struct png_layout layout;
            result = png_read_header(data, size, &layout);
            if (result != SX_IMAGE_OK) return result;
            metadata.source_width = layout.width;
            metadata.source_height = layout.height;
            break;
        }
        case SX_IMAGE_FORMAT_BMP:
        {
            struct bmp_layout layout;
            result = bmp_parse(data, size, &layout);
            if (result != 0) return result == SX_IMAGE_ERROR_LIMIT ? result : SX_IMAGE_ERROR_HEADER;
            metadata.source_width = layout.width;
            metadata.source_height = layout.height;
            break;
        }
        case SX_IMAGE_FORMAT_JPEG: return sx_jpeg_info(data, size, options, out);
        default: return SX_IMAGE_ERROR_FORMAT;
    }
    /* BMP/PNG have no decoder-level reduction. Do not silently resample or
     * accept an exact scale request that the selected decoder cannot honour. */
    if (options->scale_denom > 1u) return SX_IMAGE_ERROR_SCALE;
    metadata.width = metadata.source_width;
    metadata.height = metadata.source_height;
    metadata.scale_denom = 1u;
    if (metadata.width > image_limit(options->max_width, SX_IMAGE_MAX_DIMENSION) ||
        metadata.height > image_limit(options->max_height, SX_IMAGE_MAX_DIMENSION) ||
        (uint64_t)metadata.width * metadata.height > image_limit(options->max_pixels, SX_IMAGE_MAX_PIXELS))
    {
        return SX_IMAGE_ERROR_LIMIT;
    }
    *out = metadata;
    return SX_IMAGE_OK;
}

static int jpeg_plan(const unsigned char *data, size_t size, const struct sx_image_options *options,
                      struct sx_image_metadata *out, struct jpeg_state *state)
{
    struct sx_image_options defaults = {0};
    struct sx_image_metadata metadata = {0};
    int result;
    if (data == 0) return SX_IMAGE_ERROR_ARGUMENT;
    if (options == 0) options = &defaults;
    if (options->scale_denom != 0u && options->scale_denom != 1u && options->scale_denom != 2u &&
        options->scale_denom != 4u && options->scale_denom != 8u) return SX_IMAGE_ERROR_ARGUMENT;
    result = jpeg_prepare(data, size, state);
    if (result != SX_IMAGE_OK) return result;
    state->scale_denom = options->scale_denom ? options->scale_denom : 1u;
    for (;;)
    {
        state->output_width = (state->width + state->scale_denom - 1u) / state->scale_denom;
        state->output_height = (state->height + state->scale_denom - 1u) / state->scale_denom;
        if (state->output_width <= image_limit(options->max_width, SX_IMAGE_MAX_DIMENSION) &&
            state->output_height <= image_limit(options->max_height, SX_IMAGE_MAX_DIMENSION) &&
            (uint64_t)state->output_width * state->output_height <= image_limit(options->max_pixels, SX_IMAGE_MAX_PIXELS))
            break;
        if (options->scale_denom != 0u || state->scale_denom == 8u) return SX_IMAGE_ERROR_LIMIT;
        state->scale_denom *= 2u;
    }
    {
        unsigned c;
        for (c = 0u; c < state->components; ++c)
        {
            struct jpeg_component *component = &state->component[c];
            component->sample_width = (component->width + state->scale_denom - 1u) / state->scale_denom;
            component->sample_height = (component->height + state->scale_denom - 1u) / state->scale_denom;
        }
    }
    metadata.format = SX_IMAGE_FORMAT_JPEG;
    metadata.source_width = state->width;
    metadata.source_height = state->height;
    metadata.width = state->output_width;
    metadata.height = state->output_height;
    metadata.scale_denom = state->scale_denom;
    *out = metadata;
    return SX_IMAGE_OK;
}

int sx_jpeg_info(const unsigned char *data, size_t size, const struct sx_image_options *options,
                  struct sx_image_metadata *out)
{
    struct jpeg_state state;
    if (out == 0) return SX_IMAGE_ERROR_ARGUMENT;
    memset(out, 0, sizeof(*out));
    return jpeg_plan(data, size, options, out, &state);
}

int sx_jpeg_decode_into(const unsigned char *data, size_t size, const struct sx_image_options *options,
                         uint32_t *pixels, size_t pixel_count, struct sx_image_metadata *out)
{
    struct jpeg_state state;
    struct sx_image_metadata metadata;
    int result;
    if (out != 0) memset(out, 0, sizeof(*out));
    if (pixels == 0) return SX_IMAGE_ERROR_ARGUMENT;
    result = jpeg_plan(data, size, options, &metadata, &state);
    if (result != SX_IMAGE_OK) return result;
    if (pixel_count < (size_t)metadata.width * metadata.height) return SX_IMAGE_ERROR_BUFFER;
    if (!jpeg_decode(&state, pixels)) return SX_IMAGE_ERROR_DECODE;
    if (out != 0) *out = metadata;
    return SX_IMAGE_OK;
}

int sx_image_decode_into(const unsigned char *data, size_t size,
                         const struct sx_image_options *options, uint32_t *pixels,
                         size_t pixel_count, struct sx_image_metadata *out)
{
    struct sx_image_metadata metadata;
    int result;
    if (out != 0) memset(out, 0, sizeof(*out));
    if (pixels == 0) return SX_IMAGE_ERROR_ARGUMENT;
    result = sx_image_info(data, size, options, &metadata);
    if (result != SX_IMAGE_OK) return result;
    if (pixel_count < (size_t)metadata.width * metadata.height) return SX_IMAGE_ERROR_BUFFER;
    switch (metadata.format)
    {
        case SX_IMAGE_FORMAT_PNG: result = sx_png_decode_into(data, size, pixels, pixel_count); break;
        case SX_IMAGE_FORMAT_BMP: result = sx_bmp_decode_into(data, size, pixels, pixel_count); break;
        case SX_IMAGE_FORMAT_JPEG: return sx_jpeg_decode_into(data, size, options, pixels, pixel_count, out);
        default: return SX_IMAGE_ERROR_UNSUPPORTED;
    }
    if (result != 0) return SX_IMAGE_ERROR_DECODE;
    if (out != 0) *out = metadata;
    return SX_IMAGE_OK;
}

const char *sx_image_error_string(int result)
{
    switch (result)
    {
        case SX_IMAGE_OK: return "Success";
        case SX_IMAGE_ERROR_ARGUMENT: return "Invalid image argument";
        case SX_IMAGE_ERROR_FORMAT: return "Unrecognized image format";
        case SX_IMAGE_ERROR_UNSUPPORTED: return "Unsupported image format";
        case SX_IMAGE_ERROR_HEADER: return "Invalid or unsupported image header";
        case SX_IMAGE_ERROR_LIMIT: return "Image exceeds decode limits";
        case SX_IMAGE_ERROR_SCALE: return "Image decoder does not support the requested scale";
        case SX_IMAGE_ERROR_BUFFER: return "Image buffer is too small";
        case SX_IMAGE_ERROR_DECODE: return "Image decoding failed";
        case SX_IMAGE_ERROR_JPEG_PROGRESSIVE: return "Progressive JPEG is unsupported";
        case SX_IMAGE_ERROR_JPEG_ARITHMETIC: return "Arithmetic JPEG is unsupported";
        case SX_IMAGE_ERROR_JPEG_PRECISION: return "Unsupported JPEG sample precision";
        case SX_IMAGE_ERROR_JPEG_LOSSLESS: return "Lossless JPEG is unsupported";
        case SX_IMAGE_ERROR_JPEG_COLOR: return "Unsupported JPEG colour components";
        case SX_IMAGE_ERROR_JPEG_SAMPLING: return "Unsupported JPEG sampling factors";
        default: return "Unknown image error";
    }
}
