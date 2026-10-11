/* Prueba de host del decodificador PNG (libsximg): arma PNG en memoria y
 * afirma pixeles exactos, sin QEMU de por medio. Cubre los tres tipos de
 * bloque deflate (stored a mano, fijo y dinamico pegados de zlib real), los
 * cinco filtros, RGB y RGBA, y el rechazo de lo que esta fuera del
 * subconjunto. Es la unica verificacion del motor; el smoke en el invitado
 * solo prueba que la libreria carga y que Paint lee un archivo. */
#include <stdint.h>

extern "C" {
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
}

#include <vector>

#include "savanxp/img.h"

namespace {

int g_failures = 0;

bool check(bool ok, const char* what) {
    if (!ok) {
        ++g_failures;
        printf("  FAIL %s\n", what);
    } else {
        printf("  ok   %s\n", what);
    }
    fflush(0);
    return ok;
}

/* --- constructores -------------------------------------------------------- */

uint32_t adler(const std::vector<unsigned char>& data) {
    uint32_t a = 1, b = 0;
    for (unsigned char byte : data) {
        a += byte;
        b += a;
    }
    a %= 65521u;
    b %= 65521u;
    return (b << 16) | a;
}

struct Png {
    std::vector<unsigned char> bytes;

    void be32(uint32_t v) {
        bytes.push_back((unsigned char)(v >> 24));
        bytes.push_back((unsigned char)(v >> 16));
        bytes.push_back((unsigned char)(v >> 8));
        bytes.push_back((unsigned char)v);
    }

    void chunk(const char* type, const std::vector<unsigned char>& data) {
        be32((uint32_t)data.size());
        for (int i = 0; i < 4; ++i) {
            bytes.push_back((unsigned char)type[i]);
        }
        bytes.insert(bytes.end(), data.begin(), data.end());
        be32(0); /* CRC: el decodificador no lo comprueba */
    }

    void header(uint32_t w, uint32_t h, unsigned char ctype, unsigned char depth = 8,
                unsigned char interlace = 0) {
        static const unsigned char sig[8] = {137, 80, 78, 71, 13, 10, 26, 10};
        bytes.insert(bytes.end(), sig, sig + 8);
        std::vector<unsigned char> ihdr;
        ihdr.push_back((unsigned char)(w >> 24));
        ihdr.push_back((unsigned char)(w >> 16));
        ihdr.push_back((unsigned char)(w >> 8));
        ihdr.push_back((unsigned char)w);
        ihdr.push_back((unsigned char)(h >> 24));
        ihdr.push_back((unsigned char)(h >> 16));
        ihdr.push_back((unsigned char)(h >> 8));
        ihdr.push_back((unsigned char)h);
        ihdr.push_back(depth);
        ihdr.push_back(ctype);
        ihdr.push_back(0);
        ihdr.push_back(0);
        ihdr.push_back(interlace);
        chunk("IHDR", ihdr);
    }

    /* Un bloque stored por fila, FILTRANDO de verdad: prueba stored +
     * multi-bloque + los cinco filtros contra expected_rgb. */
    static int paeth(int a, int b, int c) {
        int p = a + b - c;
        int pa = p > a ? p - a : a - p;
        int pb = p > b ? p - b : b - p;
        int pc = p > c ? p - c : c - p;
        if (pa <= pb && pa <= pc) {
            return a;
        }
        if (pb <= pc) {
            return b;
        }
        return c;
    }

    void idat_stored_rows(uint32_t w, uint32_t h, int channels) {
        std::vector<unsigned char> raw;
        std::vector<unsigned char> prev(w * (uint32_t)channels, 0);
        for (uint32_t y = 0; y < h; ++y) {
            unsigned char f = (unsigned char)(y % 5u);
            std::vector<unsigned char> orig(w * (uint32_t)channels);
            raw.push_back(f);
            for (uint32_t x = 0; x < w * (uint32_t)channels; ++x) {
                orig[x] = (unsigned char)((x * 37u + y * 91u + f * 13u) & 0xFFu);
            }
            for (uint32_t x = 0; x < w * (uint32_t)channels; ++x) {
                int a = x >= (uint32_t)channels ? (int)orig[x - channels] : 0;
                int b = (int)prev[x];
                int c = x >= (uint32_t)channels ? (int)prev[x - channels] : 0;
                int v = (int)orig[x];
                if (f == 1) {
                    v -= a;
                } else if (f == 2) {
                    v -= b;
                } else if (f == 3) {
                    v -= (a + b) >> 1;
                } else if (f == 4) {
                    v -= paeth(a, b, c);
                }
                raw.push_back((unsigned char)(v & 0xFF));
            }
            prev = orig;
        }
        std::vector<unsigned char> stream;
        stream.push_back(0x78);
        stream.push_back(0x01);
        size_t stride = 1 + (size_t)w * channels;
        for (uint32_t y = 0; y < h; ++y) {
            bool last = (y + 1u == h);
            stream.push_back(last ? 0x01 : 0x00); /* BFINAL + stored */
            stream.push_back((unsigned char)(stride & 0xFFu));
            stream.push_back((unsigned char)(stride >> 8));
            stream.push_back((unsigned char)(~stride & 0xFFu));
            stream.push_back((unsigned char)((~stride >> 8) & 0xFFu));
            stream.insert(stream.end(), raw.begin() + y * stride, raw.begin() + (y + 1) * stride);
        }
        uint32_t sum = adler(raw);
        stream.push_back((unsigned char)(sum >> 24));
        stream.push_back((unsigned char)(sum >> 16));
        stream.push_back((unsigned char)(sum >> 8));
        stream.push_back((unsigned char)sum);
        chunk("IDAT", stream);
    }

    void end() {
        chunk("IEND", std::vector<unsigned char>());
    }
};

/* Patron esperado del constructor de arriba, sin filtrar. */
uint32_t expected_rgb(uint32_t x, uint32_t y) {
    uint32_t f = y % 5u;
    uint32_t r = (x * 3u * 37u + y * 91u + f * 13u) & 0xFFu;
    uint32_t g = ((x * 3u + 1u) * 37u + y * 91u + f * 13u) & 0xFFu;
    uint32_t b = ((x * 3u + 2u) * 37u + y * 91u + f * 13u) & 0xFFu;
    return (r << 16) | (g << 8) | b;
}

bool check_rgb_image(const struct sx_png_image& img, uint32_t w, uint32_t h, const char* what) {
    char tag[128];
    if (img.width != w || img.height != h || img.pixels == 0) {
        snprintf(tag, sizeof(tag), "%s dims", what);
        return check(false, tag);
    }
    for (uint32_t y = 0; y < h; ++y) {
        for (uint32_t x = 0; x < w; ++x) {
            if (img.pixels[y * w + x] != expected_rgb(x, y)) {
                snprintf(tag, sizeof(tag), "%s pixel (%u,%u)", what, x, y);
                return check(false, tag);
            }
        }
    }
    snprintf(tag, sizeof(tag), "%s %ux%u exactos", what, w, h);
    return check(true, tag);
}

/* --- blobs de zlib real (dinamico + fijo), generados una vez con Python ---- */
/* Mismo patron que expected_rgb, 16x16 RGB, una fila por filtro. */

static const unsigned char k_png_rgb_dynamic[] = {
    0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d,
    0x49, 0x48, 0x44, 0x52, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x10,
    0x08, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0xd9, 0x49, 0x44, 0x41, 0x54, 0x78, 0xda, 0x63, 0x60, 0x50, 0xf5, 0xca,
    0x9f, 0xb2, 0xf3, 0x1e, 0xb3, 0x86, 0x6f, 0xd1, 0xf4, 0x3d, 0x0f, 0xd9,
    0xb4, 0x03, 0x4a, 0x67, 0xed, 0x7f, 0xc2, 0xa9, 0x17, 0x5c, 0x31, 0xf7,
    0xd0, 0x73, 0x1e, 0xc3, 0xb0, 0xea, 0x05, 0x47, 0x5f, 0xf1, 0x9b, 0x44,
    0xd6, 0x2d, 0x3e, 0xf1, 0x56, 0xc8, 0x3c, 0xa6, 0x71, 0xd9, 0x69, 0xc6,
    0x8c, 0xde, 0x4d, 0xf9, 0xa4, 0x00, 0xa6, 0x0c, 0x12, 0x01, 0xf3, 0x85,
    0xc7, 0xa5, 0x39, 0x38, 0xc1, 0x1b, 0x14, 0x0a, 0x0c, 0x58, 0x20, 0xfa,
    0x80, 0x76, 0x11, 0xa0, 0xae, 0x43, 0x38, 0xd7, 0x19, 0x8e, 0xbf, 0x11,
    0x34, 0x8b, 0x6e, 0x58, 0x7a, 0xea, 0xbd, 0x88, 0x65, 0x5c, 0xf3, 0x8a,
    0xb3, 0x9f, 0xc4, 0x6d, 0x12, 0xdb, 0x56, 0x5f, 0xf8, 0x2a, 0x65, 0x9f,
    0xd2, 0xb9, 0xee, 0xf2, 0x0f, 0x59, 0xa7, 0xf4, 0x9e, 0x8d, 0xd7, 0x7e,
    0x2b, 0xb8, 0x66, 0xf5, 0x6f, 0xb9, 0xf9, 0x4f, 0xd9, 0x23, 0x77, 0x12,
    0xa3, 0x7e, 0x48, 0x25, 0x8d, 0x3d, 0xbd, 0xe5, 0xd8, 0xcd, 0x37, 0x39,
    0x24, 0x00, 0x90, 0xa7, 0x91, 0xfd, 0x44, 0xd0, 0xf3, 0x0c, 0x7d, 0x9b,
    0x6f, 0xfc, 0x55, 0x72, 0xcf, 0x99, 0xb8, 0xed, 0x36, 0x71, 0x71, 0xce,
    0xf8, 0x4d, 0xda, 0x81, 0xc6, 0x9e, 0x9e, 0xbe, 0x6a, 0x0f, 0x31, 0x7e,
    0x85, 0xc7, 0x39, 0xd8, 0xd3, 0xf9, 0xc4, 0x45, 0x36, 0x98, 0x62, 0x08,
    0xad, 0x9a, 0x7f, 0xe4, 0x25, 0x9f, 0x71, 0x44, 0xed, 0x22, 0xe2, 0xe2,
    0x1c, 0x00, 0x4d, 0x76, 0x57, 0xcd, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x49, 0x45, 0x4e, 0x44, 0x00, 0x00, 0x00, 0x00,
};

static const unsigned char k_png_rgb_fixed[] = {
    0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d,
    0x49, 0x48, 0x44, 0x52, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x10,
    0x08, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01,
    0x24, 0x49, 0x44, 0x41, 0x54, 0x18, 0x19, 0x63, 0x60, 0x50, 0xf5, 0xca,
    0x9f, 0xb2, 0xf3, 0x1e, 0xb3, 0x86, 0x6f, 0xd1, 0xf4, 0x3d, 0x0f, 0xd9,
    0xb4, 0x03, 0x4a, 0x67, 0xed, 0x7f, 0xc2, 0xa9, 0x17, 0x5c, 0x31, 0xf7,
    0xd0, 0x73, 0x1e, 0xc3, 0xb0, 0xea, 0x05, 0x47, 0x5f, 0xf1, 0x9b, 0x44,
    0xd6, 0x2d, 0x3e, 0xf1, 0x56, 0xc8, 0x3c, 0xa6, 0x71, 0xd9, 0x69, 0xc6,
    0x8c, 0xde, 0x4d, 0xf9, 0xa4, 0x00, 0xa6, 0x0c, 0x12, 0x01, 0xf3, 0x85,
    0xc7, 0xa5, 0x39, 0x38, 0xc1, 0x1b, 0x14, 0x0a, 0x0c, 0x58, 0x20, 0xfa,
    0x80, 0x76, 0x11, 0xa0, 0xae, 0x43, 0x38, 0xd7, 0x19, 0x8e, 0xbf, 0x11,
    0x34, 0x8b, 0x6e, 0x58, 0x7a, 0xea, 0xbd, 0x88, 0x65, 0x5c, 0xf3, 0x8a,
    0xb3, 0x9f, 0xc4, 0x6d, 0x12, 0xdb, 0x56, 0x5f, 0xf8, 0x2a, 0x65, 0x9f,
    0xd2, 0xb9, 0xee, 0xf2, 0x0f, 0x59, 0xa7, 0xf4, 0x9e, 0x8d, 0xd7, 0x7e,
    0x2b, 0xb8, 0x66, 0xf5, 0x6f, 0xb9, 0xf9, 0x4f, 0xd9, 0x23, 0x77, 0x12,
    0xa3, 0x7e, 0x48, 0x25, 0x8d, 0x3d, 0xbd, 0xe5, 0xd8, 0xcd, 0x37, 0x39,
    0x24, 0x00, 0x90, 0xa7, 0x91, 0xfd, 0x84, 0xdb, 0xd7, 0x50, 0x8a, 0xa1,
    0x6f, 0xf3, 0x8d, 0xbf, 0x4a, 0xee, 0x39, 0x13, 0xb7, 0xdd, 0x66, 0x50,
    0xf5, 0xca, 0x9f, 0xb2, 0xf3, 0x1e, 0xb3, 0x86, 0x6f, 0xd1, 0xf4, 0x3d,
    0x0f, 0xd9, 0xb4, 0x03, 0x4a, 0x67, 0xed, 0x7f, 0xc2, 0xa9, 0x17, 0x5c,
    0x31, 0xf7, 0xd0, 0x73, 0x1e, 0xc3, 0xb0, 0xea, 0x05, 0x47, 0x5f, 0xf1,
    0x9b, 0x44, 0x32, 0x7e, 0x93, 0x76, 0xa0, 0xb1, 0xa7, 0xa7, 0xaf, 0xda,
    0x43, 0x8c, 0x5f, 0xdf, 0xc0, 0x28, 0xb0, 0xa7, 0x09, 0x79, 0x14, 0x99,
    0x62, 0x08, 0xad, 0x9a, 0x7f, 0xe4, 0x25, 0x9f, 0x71, 0x44, 0xed, 0xa2,
    0xe3, 0x6f, 0x04, 0xcd, 0xa2, 0x1b, 0x96, 0x9e, 0x7a, 0x2f, 0x62, 0x19,
    0xd7, 0xbc, 0xe2, 0xec, 0x27, 0x71, 0x9b, 0xc4, 0xb6, 0xd5, 0x17, 0xbe,
    0x4a, 0xd9, 0xa7, 0x74, 0xae, 0xbb, 0xfc, 0x43, 0xd6, 0x29, 0xbd, 0x67,
    0xe3, 0xb5, 0xdf, 0x0a, 0x00, 0x4d, 0x76, 0x57, 0xcd, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4e, 0x44, 0x00, 0x00, 0x00,
    0x00,
};

/* RGBA 8x8 con rampa de alfa, mismo patron de filtros. */
static const unsigned char k_png_rgba_dynamic[] = {
    0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d,
    0x49, 0x48, 0x44, 0x52, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00, 0x08,
    0x08, 0x06, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x7f, 0x49, 0x44, 0x41, 0x54, 0x78, 0xda, 0x63, 0x60, 0x00, 0x02, 0x53,
    0xc1, 0x68, 0x85, 0x2c, 0xa5, 0x6d, 0x0e, 0xf3, 0x8d, 0x05, 0x13, 0xae,
    0xb8, 0xe4, 0x34, 0x70, 0x86, 0x1e, 0x5f, 0x60, 0x97, 0xa6, 0x74, 0xa0,
    0xb8, 0xbc, 0xf6, 0x01, 0xa3, 0xac, 0x3b, 0x2f, 0x58, 0x01, 0x2e, 0xcc,
    0x04, 0x52, 0x80, 0x0f, 0x33, 0x5b, 0xf5, 0x49, 0x31, 0x68, 0xea, 0x98,
    0x08, 0x40, 0xf0, 0x16, 0x81, 0x95, 0x6b, 0x80, 0x34, 0x18, 0x6f, 0x01,
    0x8b, 0xb1, 0x80, 0x55, 0x0a, 0x42, 0x31, 0xd8, 0x3a, 0x24, 0x3e, 0x10,
    0x33, 0x4c, 0x4c, 0x76, 0x64, 0x38, 0x56, 0x32, 0x47, 0xe1, 0x77, 0xeb,
    0x77, 0x07, 0x83, 0x69, 0x41, 0x09, 0xa9, 0xcb, 0xd7, 0x36, 0xcc, 0xda,
    0xc1, 0xb1, 0xe0, 0xfc, 0xc9, 0xe4, 0x03, 0x2c, 0xb7, 0xf6, 0x3d, 0x60,
    0x5c, 0xb7, 0xca, 0x8f, 0x32, 0x47, 0x02, 0x00, 0x93, 0xa6, 0x3a, 0x10,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4e, 0x44,
    0x00, 0x00, 0x00, 0x00,
};

}  // namespace

static int decode(const unsigned char* data, size_t size, struct sx_png_image* out) {
    return sx_png_decode(data, size, out);
}

int main() {
    /* stored + filtros + RGB */
    {
        Png png;
        png.header(4, 4, 2);
        png.idat_stored_rows(4, 4, 3);
        png.end();
        struct sx_png_image img = {0, 0, 0};
        bool ok = decode(png.bytes.data(), png.bytes.size(), &img) == 0;
        ok = check(ok, "stored RGB decodifica") && ok;
        if (ok) {
            ok = check_rgb_image(img, 4, 4, "stored RGB") && ok;
        }
        sx_png_free(&img);
        if (!ok) {
            return 1;
        }
    }
    /* dinamico + RGB */
    {
        struct sx_png_image img = {0, 0, 0};
        bool ok = decode(k_png_rgb_dynamic, sizeof(k_png_rgb_dynamic), &img) == 0;
        ok = check(ok, "dinamico RGB decodifica") && ok;
        if (ok) {
            ok = check_rgb_image(img, 16, 16, "dinamico RGB") && ok;
        }
        sx_png_free(&img);
        if (!ok) {
            return 1;
        }
    }
    /* fijo + RGB */
    {
        struct sx_png_image img = {0, 0, 0};
        bool ok = decode(k_png_rgb_fixed, sizeof(k_png_rgb_fixed), &img) == 0;
        ok = check(ok, "fijo RGB decodifica") && ok;
        if (ok) {
            ok = check_rgb_image(img, 16, 16, "fijo RGB") && ok;
        }
        sx_png_free(&img);
        if (!ok) {
            return 1;
        }
    }
    /* dinamico + RGBA con alfa 0/224/96 */
    {
        struct sx_png_image img = {0, 0, 0};
        bool ok = decode(k_png_rgba_dynamic, sizeof(k_png_rgba_dynamic), &img) == 0;
        ok = check(ok, "dinamico RGBA decodifica") && ok;
        if (ok) {
            ok = check(img.width == 8 && img.height == 8, "RGBA dims") && ok;
            ok = check(img.pixels[0] == 0xFFFFFFu, "RGBA alfa 0 -> blanco") && ok;
            ok = check(img.pixels[7] == 0x84888Du, "RGBA alfa 224") && ok;
            ok = check(img.pixels[4 * 8 + 3] == 0xA6BDB9u, "RGBA alfa 96") && ok;
        }
        sx_png_free(&img);
        if (!ok) {
            return 1;
        }
    }
    /* rechazos */
    {
        struct sx_png_image img = {0, 0, 0};
        /* firma rota */
        {
            std::vector<unsigned char> bad(k_png_rgb_dynamic,
                                           k_png_rgb_dynamic + sizeof(k_png_rgb_dynamic));
            bad[0] = 0;
            check(decode(bad.data(), bad.size(), &img) != 0, "rechaza firma rota");
            sx_png_free(&img);
        }
        /* truncado */
        check(decode(k_png_rgb_dynamic, 40, &img) != 0, "rechaza truncado");
        sx_png_free(&img);
        /* entrelazado */
        {
            Png png;
            png.header(4, 4, 2, 8, 1);
            png.idat_stored_rows(4, 4, 3);
            png.end();
            check(decode(png.bytes.data(), png.bytes.size(), &img) != 0, "rechaza Adam7");
            sx_png_free(&img);
        }
        /* paleta */
        {
            Png png;
            png.header(4, 4, 3);
            png.idat_stored_rows(4, 4, 1);
            png.end();
            check(decode(png.bytes.data(), png.bytes.size(), &img) != 0, "rechaza paleta");
            sx_png_free(&img);
        }
        /* 16 bpp */
        {
            Png png;
            png.header(4, 4, 2, 16);
            png.idat_stored_rows(4, 4, 3);
            png.end();
            check(decode(png.bytes.data(), png.bytes.size(), &img) != 0, "rechaza 16 bpp");
            sx_png_free(&img);
        }
        /* tRNS */
        {
            Png png;
            png.header(4, 4, 2);
            std::vector<unsigned char> trns(6, 0);
            png.chunk("tRNS", trns);
            png.idat_stored_rows(4, 4, 3);
            png.end();
            check(decode(png.bytes.data(), png.bytes.size(), &img) != 0, "rechaza tRNS");
            sx_png_free(&img);
        }
        /* adler corrupto: ultimo byte de datos del IDAT (el CRC del chunk no
         * se comprueba, asi que hay que tocar dentro del stream) */
        {
            std::vector<unsigned char> bad(k_png_rgb_dynamic,
                                           k_png_rgb_dynamic + sizeof(k_png_rgb_dynamic));
            bad[sizeof(k_png_rgb_dynamic) - 17] ^= 0xFF;
            check(decode(bad.data(), bad.size(), &img) != 0, "rechaza adler corrupto");
            sx_png_free(&img);
        }
        /* filtro inexistente */
        {
            Png png;
            png.header(4, 4, 2);
            std::vector<unsigned char> raw;
            raw.push_back(9); /* filtro 9 no existe */
            for (int i = 0; i < 48; ++i) {
                raw.push_back(0);
            }
            std::vector<unsigned char> stream;
            stream.push_back(0x78);
            stream.push_back(0x01);
            stream.push_back(0x01);
            uint16_t len = (uint16_t)raw.size();
            stream.push_back((unsigned char)(len & 0xFFu));
            stream.push_back((unsigned char)(len >> 8));
            stream.push_back((unsigned char)(~len & 0xFFu));
            stream.push_back((unsigned char)((~len >> 8) & 0xFFu));
            stream.insert(stream.end(), raw.begin(), raw.end());
            /* 3 filas mas sanas para aislar el fallo en la primera */
            for (int r = 1; r < 4; ++r) {
                stream.push_back(r == 3 ? 0x01 : 0x00);
                stream.push_back((unsigned char)(len & 0xFFu));
                stream.push_back((unsigned char)(len >> 8));
                stream.push_back((unsigned char)(~len & 0xFFu));
                stream.push_back((unsigned char)((~len >> 8) & 0xFFu));
                for (int i = 0; i < 49; ++i) {
                    stream.push_back((unsigned char)r);
                }
            }
            /* BFINAL en el ultimo bloque: reescribir el primero como no-final */
            stream[2] = 0x00;
            /* adler sobre TODO lo crudo */
            std::vector<unsigned char> all = raw;
            for (int r = 1; r < 4; ++r) {
                all.push_back((unsigned char)r);
                for (int i = 0; i < 48; ++i) {
                    all.push_back((unsigned char)r);
                }
            }
            uint32_t sum = adler(all);
            stream.push_back((unsigned char)(sum >> 24));
            stream.push_back((unsigned char)(sum >> 16));
            stream.push_back((unsigned char)(sum >> 8));
            stream.push_back((unsigned char)sum);
            png.chunk("IDAT", stream);
            png.end();
            check(decode(png.bytes.data(), png.bytes.size(), &img) != 0, "rechaza filtro 9");
            sx_png_free(&img);
        }
    }
    /* free tolera vacia y nula */
    {
        struct sx_png_image img = {0, 0, 0};
        sx_png_free(&img);
        sx_png_free(0);
        check(img.pixels == 0, "free vacia");
    }

    if (g_failures != 0) {
        printf("SXIMG TEST FAIL %d\n", g_failures);
        return 1;
    }
    printf("SXIMG TEST PASS\n");
    return 0;
}
