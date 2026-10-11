#include "savanxp/img.h"

#include <stdlib.h>
#include <string.h>

/* Decodificador PNG (subconjunto 8-bit truecolor, ver img.h).
 *
 * Dos mitades independientes: inflate sigue RFC 1951 al pie (bits LSB,
 * Huffman canonico fijo y dinamico, stored, Adler del wrapper zlib) y la
 * capa PNG entiende firma, IHDR, IDAT concatenados y los cinco unfilters.
 * Solo usa malloc/free/memcpy/memset para poder vivir en libsximg.so.0.4,
 * que resuelve contra el ejecutable.
 */

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

int sx_png_decode(const unsigned char *data, size_t size, struct sx_png_image *out)
{
    static const unsigned char k_signature[8] = {137, 80, 78, 71, 13, 10, 26, 10};
    size_t pos;
    uint32_t width = 0u;
    uint32_t height = 0u;
    int channels = 0;
    int have_header = 0;
    unsigned char *packed = 0;
    size_t packed_size = 0;
    size_t packed_capacity = 0;
    unsigned char *raw = 0;
    uint32_t *pixels = 0;
    size_t stride;
    size_t raw_size;
    size_t produced = 0;
    uint32_t y;
    int ok = 0;

    if (out == 0)
    {
        return -1;
    }
    out->pixels = 0;
    out->width = 0u;
    out->height = 0u;
    if (data == 0 || size < 8u || memcmp(data, k_signature, 8) != 0)
    {
        return -1;
    }
    pos = 8;
    while (pos + 8u <= size)
    {
        uint32_t length = read_u32be(data + pos);
        uint32_t type = read_u32be(data + pos + 4u);
        const unsigned char *body;

        if (length > size - pos - 8u || 4u > size - pos - 8u - length)
        {
            goto done;
        }
        body = data + pos + 8u;
        if (type == 0x49484452u) /* IHDR */
        {
            if (have_header || pos != 8u || length != 13u)
            {
                goto done;
            }
            width = read_u32be(body);
            height = read_u32be(body + 4u);
            if (width == 0u || height == 0u || width > SX_PNG_MAX_DIMENSION ||
                height > SX_PNG_MAX_DIMENSION)
            {
                goto done;
            }
            if (body[8] != 8u || body[10] != 0u || body[11] != 0u || body[12] != 0u)
            {
                /* Solo 8 bpp, sin compresion ni filtro raros, sin Adam7. */
                goto done;
            }
            if (body[9] == 2u)
            {
                channels = 3;
            }
            else if (body[9] == 6u)
            {
                channels = 4;
            }
            else
            {
                /* Sin paleta, sin grises, sin alfa puro: fuera del subconjunto. */
                goto done;
            }
            have_header = 1;
        }
        else if (have_header == 0)
        {
            /* El primer chunk tiene que ser IHDR. */
            goto done;
        }
        else if (type == 0x504C5445u || type == 0x74524E53u) /* PLTE, tRNS */
        {
            goto done;
        }
        else if (type == 0x49444154u) /* IDAT */
        {
            if (!append_bytes(&packed, &packed_size, &packed_capacity, body, length))
            {
                goto done;
            }
        }
        else if (type == 0x49454E44u) /* IEND */
        {
            if (length != 0u)
            {
                goto done;
            }
            pos += 12u;
            break;
        }
        pos += 12u + length;
    }
    if (have_header == 0 || packed_size == 0u)
    {
        goto done;
    }
    stride = 1u + (size_t)width * (size_t)channels;
    raw_size = (size_t)height * stride;
    raw = (unsigned char *)malloc(raw_size);
    if (raw == 0)
    {
        goto done;
    }
    if (!sx_zlib_decompress(packed, packed_size, raw, raw_size, &produced) || produced != raw_size)
    {
        goto done;
    }
    pixels = (uint32_t *)malloc((size_t)width * (size_t)height * 4u);
    if (pixels == 0)
    {
        goto done;
    }
    /* Unfilter in place: cada fila ya reconstruida sirve de previa, y dentro
     * de la fila los bytes a la izquierda ya estan reconstruidos. */
    for (y = 0u; y < height; ++y)
    {
        unsigned char *row = raw + (size_t)y * stride;
        unsigned char *previous = y != 0u ? raw + (size_t)(y - 1u) * stride + 1u : 0;
        unsigned char *current = row + 1u;
        size_t row_bytes = (size_t)width * (size_t)channels;
        size_t i;
        int filter = (int)row[0];

        if (filter < 0 || filter > 4)
        {
            goto done;
        }
        for (i = 0; i < row_bytes; ++i)
        {
            int a = i >= (size_t)channels ? (int)current[i - (size_t)channels] : 0;
            int b = previous != 0 ? (int)previous[i] : 0;
            int prediction = 0;

            if (filter == 1)
            {
                prediction = a;
            }
            else if (filter == 2)
            {
                prediction = b;
            }
            else if (filter == 3)
            {
                prediction = (a + b) >> 1;
            }
            else if (filter == 4)
            {
                int c = (previous != 0 && i >= (size_t)channels) ? (int)previous[i - (size_t)channels]
                                                                 : 0;

                prediction = paeth_predictor(a, b, c);
            }
            current[i] = (unsigned char)((int)current[i] + prediction);
        }
        /* RGB(A) a 0x00RRGGBB, el alfa integrado sobre blanco. */
        for (i = 0; i < (size_t)width; ++i)
        {
            uint32_t red = current[i * (size_t)channels];
            uint32_t green = current[i * (size_t)channels + 1u];
            uint32_t blue = current[i * (size_t)channels + 2u];

            if (channels == 4)
            {
                uint32_t alpha = current[i * (size_t)channels + 3u];

                red = (red * alpha + 255u * (255u - alpha) + 127u) / 255u;
                green = (green * alpha + 255u * (255u - alpha) + 127u) / 255u;
                blue = (blue * alpha + 255u * (255u - alpha) + 127u) / 255u;
            }
            pixels[(size_t)y * (size_t)width + i] = (red << 16) | (green << 8) | blue;
        }
    }
    out->pixels = pixels;
    out->width = width;
    out->height = height;
    pixels = 0;
    ok = 1;

done:
    free(packed);
    free(raw);
    free(pixels);
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
