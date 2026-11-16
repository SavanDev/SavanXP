/* Host tests for libsximg: construct images in memory and compare exact
 * pixels without QEMU. PNG covers stored/fixed/dynamic deflate, every row
 * filter and RGB/RGBA; BMP covers palettes, bitfields, alpha and RLE.
 * Guest scenarios prove that consumers load the library and read files. */
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

/* --- BMP builder ---------------------------------------------------------- */

/* A BMP with a BITMAPINFOHEADER. Pixel (x, y), counted from the top, gets
 * the bytes B = x * 16 + y, G = 0x80 + y, R = 0xF0 - x, so every pixel is
 * distinct and a row read in the wrong order shows. */
struct Bmp {
    std::vector<unsigned char> bytes;

    void le16(size_t at, uint32_t v) {
        bytes[at] = (unsigned char)v;
        bytes[at + 1] = (unsigned char)(v >> 8);
    }

    void le32(size_t at, uint32_t v) {
        for (int i = 0; i < 4; ++i) {
            bytes[at + (size_t)i] = (unsigned char)(v >> (8 * i));
        }
    }

    Bmp() = default;

    Bmp(int32_t width, int32_t height, unsigned bits, bool top_down = false) {
        const uint32_t w = (uint32_t)width;
        const uint32_t h = (uint32_t)(height < 0 ? -height : height);
        const uint32_t bpp = bits / 8u;
        const uint32_t stride = (w * bpp + 3u) & ~3u;
        bytes.assign(54u + (size_t)stride * h, 0);
        bytes[0] = 'B';
        bytes[1] = 'M';
        le32(2, (uint32_t)bytes.size());
        le32(10, 54);
        le32(14, 40);
        le32(18, (uint32_t)width);
        le32(22, (uint32_t)(top_down ? -(int32_t)h : (int32_t)h));
        le16(26, 1);
        le16(28, bits);
        for (uint32_t y = 0; y < h; ++y) {
            uint32_t file_row = top_down ? y : h - 1u - y;
            unsigned char* row = bytes.data() + 54u + (size_t)file_row * stride;
            for (uint32_t x = 0; x < w; ++x) {
                row[x * bpp] = (unsigned char)(x * 16u + y);
                row[x * bpp + 1] = (unsigned char)(0x80u + y);
                row[x * bpp + 2] = (unsigned char)(0xF0u - x);
                if (bpp == 4u) {
                    row[x * bpp + 3] = 0x5A; /* ignored by BI_RGB */
                }
            }
            /* Padding bytes are not pixels: fill them so a decoder that reads
             * them shows. */
            for (uint32_t pad = w * bpp; pad < stride; ++pad) {
                row[pad] = 0xEE;
            }
        }
    }

    static uint32_t expected(uint32_t x, uint32_t y) {
        return ((0xF0u - x) << 16) | ((0x80u + y) << 8) | ((x * 16u + y) & 0xFFu);
    }
};

/* Decodes and compares every pixel against Bmp::expected. */
bool bmp_matches(const Bmp& bmp, uint32_t width, uint32_t height) {
    uint32_t w = 0, h = 0;
    if (sx_bmp_info(bmp.bytes.data(), bmp.bytes.size(), &w, &h) != 0 || w != width || h != height) {
        return false;
    }
    std::vector<uint32_t> pixels((size_t)w * h, 0xDEADBEEFu);
    if (sx_bmp_decode_into(bmp.bytes.data(), bmp.bytes.size(), pixels.data(), pixels.size()) != 0) {
        return false;
    }
    for (uint32_t y = 0; y < h; ++y) {
        for (uint32_t x = 0; x < w; ++x) {
            if (pixels[(size_t)y * w + x] != Bmp::expected(x, y)) {
                return false;
            }
        }
    }
    return true;
}

bool bmp_refused(const Bmp& bmp) {
    uint32_t pixel = 0;
    std::vector<uint32_t> pixels(64u * 64u);
    return sx_bmp_info(bmp.bytes.data(), bmp.bytes.size(), &pixel, &pixel) != 0 &&
           sx_bmp_decode_into(bmp.bytes.data(), bmp.bytes.size(), pixels.data(), pixels.size()) != 0;
}

void bmp_tests() {
    /* 3 pixels wide at 24 bpp is 9 bytes a row, padded to 12. */
    check(bmp_matches(Bmp(3, 2, 24), 3, 2), "bmp 24 bpp bottom-up with padding");
    check(bmp_matches(Bmp(3, 2, 24, true), 3, 2), "bmp 24 bpp top-down");
    check(bmp_matches(Bmp(5, 4, 32), 5, 4), "bmp 32 bpp bottom-up, fourth byte ignored");
    check(bmp_matches(Bmp(5, 4, 32, true), 5, 4), "bmp 32 bpp top-down");
    {
        /* A larger header (BITMAPV5HEADER, 124 bytes) with the pixels after it. */
        Bmp bmp(2, 2, 24);
        std::vector<unsigned char> v5(14u + 124u + (bmp.bytes.size() - 54u), 0);
        memcpy(v5.data(), bmp.bytes.data(), 54u);
        memcpy(v5.data() + 14u + 124u, bmp.bytes.data() + 54u, bmp.bytes.size() - 54u);
        bmp.bytes = v5;
        bmp.le32(10, 14u + 124u);
        bmp.le32(14, 124);
        check(bmp_matches(bmp, 2, 2), "bmp with a V5 header");
    }
    {
        Bmp bmp(2, 2, 24);
        bmp.le32(10, 54u + 4u); /* the pixels start 4 bytes later than a valid file's */
        check(bmp_refused(bmp), "bmp rejects rows past the end");
    }
    {
        Bmp bmp(4, 4, 24);
        bmp.bytes.resize(bmp.bytes.size() - 1u);
        check(bmp_refused(bmp), "bmp rejects a truncated last row");
    }
    {
        Bmp bmp(4, 4, 24);
        bmp.le32(10, 40); /* inside the headers */
        check(bmp_refused(bmp), "bmp rejects pixels inside the headers");
    }
    {
        Bmp bmp(4, 4, 24);
        bmp.bytes[1] = 'A';
        check(bmp_refused(bmp), "bmp rejects a bad signature");
    }
    for (unsigned bits : {0u, 2u, 12u, 48u}) {
        Bmp bmp(4, 4, 24);
        bmp.le16(28, bits);
        check(bmp_refused(bmp), "bmp rejects unsupported bit depths");
    }
    for (uint32_t compression : {1u, 2u, 3u}) {
        Bmp bmp(4, 4, 32);
        bmp.le32(30, compression);
        check(bmp_refused(bmp), "bmp rejects wrong RLE depth or missing bitfield masks");
    }
    {
        Bmp bmp(4, 4, 24);
        bmp.le16(26, 2);
        check(bmp_refused(bmp), "bmp rejects planes other than 1");
    }
    {
        Bmp bmp(4, 4, 24);
        bmp.le32(14, 12); /* BITMAPCOREHEADER */
        check(bmp_refused(bmp), "bmp rejects the OS/2 core header");
    }
    {
        Bmp zero_width(4, 4, 24);
        zero_width.le32(18, 0);
        Bmp negative_width(4, 4, 24);
        negative_width.le32(18, (uint32_t)-4);
        Bmp zero_height(4, 4, 24);
        zero_height.le32(22, 0);
        Bmp huge(4, 4, 24);
        huge.le32(18, SX_BMP_MAX_DIMENSION + 1u);
        Bmp most_negative(4, 4, 24);
        most_negative.le32(22, 0x80000000u);
        check(bmp_refused(zero_width) && bmp_refused(negative_width) && bmp_refused(zero_height) &&
                  bmp_refused(huge) && bmp_refused(most_negative),
              "bmp rejects empty, negative and oversized dimensions");
    }
    {
        Bmp bmp(4, 4, 24);
        std::vector<uint32_t> small(15);
        check(sx_bmp_decode_into(bmp.bytes.data(), bmp.bytes.size(), small.data(), small.size()) != 0 &&
                  sx_bmp_decode_into(bmp.bytes.data(), bmp.bytes.size(), 0, 16) != 0,
              "bmp rejects a buffer that is too small or missing");
    }
    {
        Bmp bmp(4, 4, 24);
        check(sx_bmp_info(bmp.bytes.data(), 53, 0, 0) != 0 && sx_bmp_info(0, 0, 0, 0) != 0 &&
                  sx_bmp_info(bmp.bytes.data(), bmp.bytes.size(), 0, 0) == 0,
              "bmp info tolerates null outputs and rejects short input");
    }
}

/* Independent fixtures: raw bytes and hand-calculated expected colours. */
Bmp bmp_fixture(uint32_t w, int32_t h, unsigned bits, uint32_t compression,
                const std::vector<unsigned char>& rows, uint32_t colours = 0u,
                uint32_t header = 40u, const std::vector<uint32_t>& masks = {}) {
    Bmp bmp;
    size_t offset = 14u + header + colours * 4u + (header == 40u ? masks.size() * 4u : 0u);
    bmp.bytes.assign(offset + rows.size(), 0);
    bmp.bytes[0] = 'B'; bmp.bytes[1] = 'M';
    bmp.le32(2, (uint32_t)bmp.bytes.size()); bmp.le32(10, (uint32_t)offset);
    bmp.le32(14, header); bmp.le32(18, w); bmp.le32(22, (uint32_t)h);
    bmp.le16(26, 1); bmp.le16(28, bits); bmp.le32(30, compression);
    bmp.le32(34, (uint32_t)rows.size()); bmp.le32(46, colours);
    for (size_t i = 0; i < masks.size(); ++i) bmp.le32(54u + i * 4u, masks[i]);
    for (uint32_t i = 0; i < colours; ++i) {
        /* Distinct RGB, with a nonzero reserved byte that must be ignored. */
        bmp.le32(14u + header + i * 4u, 0x5a000000u | (i * 0x10203u));
    }
    memcpy(bmp.bytes.data() + offset, rows.data(), rows.size());
    return bmp;
}

bool bmp_pixels(const Bmp& bmp, const std::vector<uint32_t>& expected, bool argb = false) {
    uint32_t w = 0u, h = 0u;
    if (sx_bmp_info(bmp.bytes.data(), bmp.bytes.size(), &w, &h) != 0 ||
        (size_t)w * h != expected.size()) return false;
    std::vector<uint32_t> pixels(expected.size() + 2u, 0xdeadbeefu);
    int result = argb ? sx_bmp_decode_argb_into(bmp.bytes.data(), bmp.bytes.size(), pixels.data() + 1u, expected.size()) :
                        sx_bmp_decode_into(bmp.bytes.data(), bmp.bytes.size(), pixels.data() + 1u, expected.size());
    return result == 0 && pixels.front() == 0xdeadbeefu && pixels.back() == 0xdeadbeefu &&
           std::vector<uint32_t>(pixels.begin() + 1, pixels.end() - 1) == expected;
}

bool bmp_decode_refused(const Bmp& bmp) {
    uint32_t pixels[66];
    for (auto& p : pixels) p = 0xdeadbeefu;
    bool refused = sx_bmp_decode_into(bmp.bytes.data(), bmp.bytes.size(), pixels + 1, 64) < 0 &&
                   sx_bmp_decode_argb_into(bmp.bytes.data(), bmp.bytes.size(), pixels + 1, 64) < 0;
    return refused && pixels[0] == 0xdeadbeefu && pixels[65] == 0xdeadbeefu;
}

void bmp_extended_tests() {
    const uint32_t c1 = 0x010203u, c2 = 0x020406u, c3 = 0x030609u;
    for (bool top_down : {false, true}) {
        /* Odd widths force unused bits/nibbles and DWORD row padding. */
        auto mono = bmp_fixture(5, top_down ? -2 : 2, 1, 0,
                                {0xa8, 0xee, 0xee, 0xee, 0x50, 0xee, 0xee, 0xee}, 2);
        std::vector<uint32_t> first = {c1, 0, c1, 0, c1, 0, c1, 0, c1, 0};
        std::vector<uint32_t> reversed = {0, c1, 0, c1, 0, c1, 0, c1, 0, c1};
        check(bmp_pixels(mono, top_down ? first : reversed), "bmp 1 bpp bit order, padding and row order");
        auto four = bmp_fixture(3, top_down ? -2 : 2, 4, 0,
                                {0x12, 0x3f, 0xee, 0xee, 0x32, 0x1f, 0xee, 0xee}, 4);
        check(bmp_pixels(four, top_down ? std::vector<uint32_t>{c1,c2,c3,c3,c2,c1} :
                                         std::vector<uint32_t>{c3,c2,c1,c1,c2,c3}),
              "bmp 4 bpp nibble order and unused trailing nibble");
        auto eight = bmp_fixture(3, top_down ? -2 : 2, 8, 0, {1,2,3,0xee,3,2,1,0xee}, 4);
        check(bmp_pixels(eight, top_down ? std::vector<uint32_t>{c1,c2,c3,c3,c2,c1} :
                                          std::vector<uint32_t>{c3,c2,c1,c1,c2,c3}),
              "bmp 8 bpp reduced palette, reserved bytes and row order");
    }
    {
        auto bmp = bmp_fixture(3, 1, 1, 0, {0xa0,0,0,0}, 2);
        bmp.le32(46, 0);
        check(bmp_pixels(bmp, {c1,0,c1}), "bmp implicit full palette");
        check(bmp_pixels(bmp, {0xff010203u,0xff000000u,0xff010203u}, true), "bmp palette ARGB is opaque");
        bmp.le32(46, 3);
        check(bmp_refused(bmp), "bmp rejects excessive palette count");
        bmp.le32(46, 2); bmp.le32(10, 58);
        check(bmp_refused(bmp), "bmp rejects palette overlapping pixels");
        bmp.le32(10, 0xffffffffu);
        check(bmp_refused(bmp), "bmp rejects out-of-range pixel offset");
    }
    for (unsigned bits : {1u,4u,8u}) {
        auto bmp = bmp_fixture(1, 1, bits, 0, {0xff,0,0,0}, 1);
        check(bmp_decode_refused(bmp), "bmp rejects an index outside the declared palette");
        bmp.le32(10, 54); bmp.bytes.resize(58);
        check(bmp_refused(bmp), "bmp rejects a missing palette");
    }
    for (bool top_down : {false, true}) {
        auto rgb555 = bmp_fixture(3, top_down ? -1 : 1, 16, 0,
                                  {0x00,0xfc, 0xe0,0x03, 0x10,0x42, 0xee,0xee});
        check(bmp_pixels(rgb555, {0xff0000,0x00ff00,0x848484}), "bmp RGB555 ignores high bit and scales midtones");
        auto rgb565 = bmp_fixture(3, top_down ? -1 : 1, 16, 3,
                                  {0x00,0xf8, 0xe0,0x07, 0x10,0x84, 0xee,0xee}, 0, 40,
                                  {0xf800,0x07e0,0x001f});
        check(bmp_pixels(rgb565, {0xff0000,0x00ff00,0x848284}), "bmp RGB565 external masks, scaling and padding");
    }
    for (bool top_down : {false, true}) {
        auto bmp = bmp_fixture(1, top_down ? -2 : 2, 16, 0, {0,0x7c,0,0,0x1f,0,0,0});
        check(bmp_pixels(bmp, top_down ? std::vector<uint32_t>{0xff0000,0xff} :
                                         std::vector<uint32_t>{0xff,0xff0000}), "bmp 16 bpp multiple row order");
    }
    for (unsigned bits : {4u,8u}) {
        auto bmp = bmp_fixture(1, 1, bits, 0, {0x10,0,0,0}, 1u << bits);
        bmp.le32(46, 0);
        check(bmp_pixels(bmp, {bits == 4 ? c1 : 16u * 0x10203u}), "bmp implicit full 4/8-bit palette");
    }
    for (uint32_t header : {52u,56u,108u,124u}) {
        auto bmp = bmp_fixture(1, -1, 32, 3, {0x11,0x22,0x33,0x99}, 0, header,
                               {0xff,0xff00,0xff0000});
        check(bmp_pixels(bmp, {0x112233}), "bmp embedded RGB masks can swap channel order");
        check(bmp_pixels(bmp, {0xff112233u}, true), "bmp absent alpha mask is opaque");
    }
    for (uint32_t header : {108u,124u}) {
        auto bmp = bmp_fixture(3, 1, 32, 3,
                               {0,0,0xff,0, 0,0,0xff,128, 0,0,0xff,255}, 0, header,
                               {0xff0000,0xff00,0xff,0xff000000});
        check(bmp_pixels(bmp, {0xffffff,0xff7f7f,0xff0000}), "bmp V4/V5 alpha composites onto white");
        check(bmp_pixels(bmp, {0x00ff0000,0x80ff0000,0xffff0000}, true), "bmp V4/V5 retains straight alpha");
        bmp.le32(30, 0);
        check(bmp_pixels(bmp, {0xff0000,0xff0000,0xff0000}), "bmp BI_RGB ignores embedded masks and high bytes");
    }
    {
        auto bmp = bmp_fixture(1, 1, 16, 3, {0x21,0x84,0,0}, 0, 108, {0x7c00,0x03e0,0x1f,0x8000});
        check(bmp_pixels(bmp, {0xff080808}, true), "bmp 16 bpp alpha bitfield");
        auto wide = bmp_fixture(1, 1, 32, 3, {0,0,0,0x20}, 0, 124,
                                {0x3ff00000,0xffc00,0x3ff,0xc0000000});
        check(bmp_pixels(wide, {0x00800000}, true), "bmp 10-bit components scale without overflow");
        auto widest = bmp_fixture(1, 1, 32, 3, {0,0,0,0x20}, 0, 108,
                                  {0x3fffffff,0x40000000,0x80000000});
        check(bmp_pixels(widest, {0xff800000}, true), "bmp 30-bit component scaling uses a wide intermediate");
        auto rgb = bmp_fixture(1, 1, 32, 0, {0x11,0x22,0x33,0});
        check(bmp_pixels(rgb, {0xff332211}, true), "bmp BI_RGB has opaque alpha even with a zero high byte");
    }
    for (auto masks : {std::vector<uint32_t>{0,0x7e0,0x1f}, {0xf800,0xf800,0x1f},
                       {0xa800,0x7e0,0x1f}, {0x1f0000,0x7e0,0x1f}, {0xf800,0x7e0,0x1f,0x8000},
                       {0xf800,0x7e0,0x1f,0x50000000}}) {
        auto bmp = bmp_fixture(1, 1, 16, 3, {0,0,0,0}, 0, 108, masks);
        check(bmp_refused(bmp), "bmp rejects zero, overlapping, noncontiguous or out-of-depth masks");
    }
    {
        auto bmp = bmp_fixture(1, 1, 16, 3, {0,0,0,0}, 0, 40, {0xf800,0x7e0,0x1f});
        bmp.le32(10, 62);
        check(bmp_refused(bmp), "bmp rejects incomplete external masks");
        bmp.le32(14, 48); bmp.le32(10, 66);
        check(bmp_refused(bmp), "bmp rejects incomplete embedded masks");
    }
    for (unsigned bits : {4u,8u}) {
        uint32_t compression = bits == 4 ? 2u : 1u;
        auto encoded = bmp_fixture(5, 2, bits, compression,
                                   {5, (unsigned char)(bits == 4 ? 0x12 : 1), 0,0, 3, (unsigned char)(bits == 4 ? 0x32 : 3), 0,1}, 4);
        check(bmp_pixels(encoded, bits == 4 ? std::vector<uint32_t>{c3,c2,c3,0,0,c1,c2,c1,c2,c1} :
                                             std::vector<uint32_t>{c3,c3,c3,0,0,c1,c1,c1,c1,c1}),
              "bmp RLE encoded runs, alternating nibbles, EOL and early EOB");
        auto literal = bmp_fixture(5, 1, bits, compression, bits == 4 ?
                                    std::vector<unsigned char>{0,5,0x12,0x31,0x2f,0,0,1} :
                                    std::vector<unsigned char>{0,5,1,2,3,1,2,0,0,1}, 4);
        check(bmp_pixels(literal, {c1,c2,c3,c1,c2}), "bmp RLE absolute odd run with word padding");
        auto even = bmp_fixture(4, 1, bits, compression, bits == 4 ?
                                 std::vector<unsigned char>{0,4,0x12,0x31,0,1} :
                                 std::vector<unsigned char>{0,4,1,2,3,1,0,1}, 4);
        check(bmp_pixels(even, {c1,c2,c3,c1}), "bmp RLE absolute even run without padding");
        auto delta = bmp_fixture(4, 3, bits, compression,
                                 {1,(unsigned char)(bits == 4 ? 0x11 : 1),0,2,1,1,
                                  1,(unsigned char)(bits == 4 ? 0x22 : 2),0,0,0,1}, 4);
        check(bmp_pixels(delta, {0,0,0,0,0,0,c2,0,c1,0,0,0}), "bmp RLE delta moves right and up; skips initialized");
        auto blank = bmp_fixture(2, 1, bits, compression, {0,1}, 4);
        check(bmp_pixels(blank, {0xff000000,0xff000000}, true), "bmp RLE early EOB leaves opaque palette zero");
        auto last_eol = bmp_fixture(2, 1, bits, compression, {0,0,0,1}, 4);
        check(bmp_pixels(last_eol, {0,0}), "bmp RLE accepts final EOL before EOB");
        for (auto stream : {std::vector<unsigned char>{0,0}, {0,0,0,0,0,1}, {0,2}, {0,2,0},
                            {0,2,5,0,0,1}, {0,2,0,1,0,1}, {5,1,0,1}, {1,0xff,0,1},
                            {0,5,1,2,3,4,5}, {0,3,0xff,0xff,0xff,0,0,1}, {0,0,1,1,0,1}}) {
            auto bad = bmp_fixture(4, 1, bits, compression, stream, 4);
            check(bmp_decode_refused(bad), "bmp RLE rejects truncation, missing EOB, overflow and bad indices");
        }
        auto bad = literal;
        bad.le32(34, bits == 4 ? 7u : 9u);
        check(bmp_decode_refused(bad), "bmp RLE cannot read EOB beyond biSizeImage");
        bad = literal; bad.le32(34, 10000);
        check(bmp_refused(bad), "bmp RLE rejects compressed extent beyond input");
        bad = literal; bad.le32(34, 0);
        check(bmp_refused(bad), "bmp RLE requires compressed size");
        bad = literal; bad.le32(22, (uint32_t)-1);
        check(bmp_refused(bad), "bmp RLE rejects top-down storage");
        check(sx_bmp_decode_argb_into(literal.bytes.data(), literal.bytes.size(), nullptr, 5) < 0,
              "bmp ARGB rejects missing destination");
        uint32_t small[4];
        check(sx_bmp_decode_argb_into(literal.bytes.data(), literal.bytes.size(), small, 4) < 0,
              "bmp ARGB rejects short destination");
    }
    for (const Bmp& complete : {
             bmp_fixture(1, 1, 4, 0, {0x10,0,0,0}, 2),
             bmp_fixture(1, 1, 16, 3, {0,0,0,0}, 0, 40, {0xf800,0x7e0,0x1f}),
             bmp_fixture(1, 1, 32, 3, {0,0,0,0}, 0, 124, {0xff0000,0xff00,0xff,0xff000000}),
             bmp_fixture(5, 1, 4, 2, {0,5,0x12,0x31,0x20,0,0,1}, 4),
             bmp_fixture(5, 1, 8, 1, {0,5,1,2,3,1,2,0,0,1}, 4)}) {
        bool refused = true;
        for (size_t length = 0; length < complete.bytes.size(); ++length) {
            Bmp truncated = complete;
            truncated.bytes.resize(length);
            refused = bmp_decode_refused(truncated) && refused;
        }
        check(refused, "bmp rejects every truncated prefix of palette, masks and RLE fixtures");
    }

}

/* sx_png_info and sx_png_decode_into against the allocating decoder. */
void png_into_tests() {
    const struct {
        const unsigned char* data;
        size_t size;
        const char* name;
    } fixtures[] = {
        {k_png_rgb_dynamic, sizeof(k_png_rgb_dynamic), "png into matches decode (rgb dynamic)"},
        {k_png_rgb_fixed, sizeof(k_png_rgb_fixed), "png into matches decode (rgb fixed)"},
        {k_png_rgba_dynamic, sizeof(k_png_rgba_dynamic), "png into matches decode (rgba, onto white)"},
    };
    for (const auto& fixture : fixtures) {
        struct sx_png_image img = {0, 0, 0};
        uint32_t w = 0, h = 0;
        bool ok = sx_png_decode(fixture.data, fixture.size, &img) == 0 &&
                  sx_png_info(fixture.data, fixture.size, &w, &h) == 0 && w == img.width && h == img.height;
        if (ok) {
            std::vector<uint32_t> pixels((size_t)w * h, 0xDEADBEEFu);
            ok = sx_png_decode_into(fixture.data, fixture.size, pixels.data(), pixels.size()) == 0 &&
                 memcmp(pixels.data(), img.pixels, pixels.size() * 4u) == 0;
            std::vector<uint32_t> small((size_t)w * h - 1u);
            ok = ok && sx_png_decode_into(fixture.data, fixture.size, small.data(), small.size()) != 0 &&
                 sx_png_decode_into(fixture.data, fixture.size, 0, pixels.size()) != 0;
        }
        check(ok, fixture.name);
        sx_png_free(&img);
    }
    {
        Png palette;
        palette.header(4, 4, 3);
        Png grey16;
        grey16.header(4, 4, 0, 16);
        Png interlaced;
        interlaced.header(4, 4, 2, 8, 1);
        Png rgb;
        rgb.header(5, 3, 2);
        uint32_t w = 0, h = 0;
        check(sx_png_info(palette.bytes.data(), palette.bytes.size(), &w, &h) != 0 &&
                  sx_png_info(grey16.bytes.data(), grey16.bytes.size(), &w, &h) != 0 &&
                  sx_png_info(interlaced.bytes.data(), interlaced.bytes.size(), &w, &h) != 0,
              "png info rejects palette, 16-bit grey and Adam7");
        check(sx_png_info(rgb.bytes.data(), rgb.bytes.size(), &w, &h) == 0 && w == 5 && h == 3 &&
                  sx_png_info(rgb.bytes.data(), rgb.bytes.size() - 1u, &w, &h) != 0,
              "png info reads IHDR alone and rejects a short one");
        Bmp bmp(4, 4, 24);
        check(sx_png_info(bmp.bytes.data(), bmp.bytes.size(), &w, &h) != 0 &&
                  sx_bmp_info(k_png_rgb_dynamic, sizeof(k_png_rgb_dynamic), &w, &h) != 0,
              "png and bmp info each refuse the other's file");
    }
}

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
    // Cursor images retain straight alpha; document decode stays white-backed.
    {
        struct sx_png_image img = {0, 0, 0};
        bool ok = check(sx_png_decode_argb(k_png_rgba_dynamic, sizeof(k_png_rgba_dynamic), &img) == 0,
                        "cursor PNG retains alpha");
        if (ok) {
            ok = check((img.pixels[0] >> 24) == 0, "cursor PNG transparent pixel") && ok;
            ok = check((img.pixels[7] >> 24) == 224, "cursor PNG partial alpha") && ok;
            ok = check((img.pixels[4*8+3] >> 24) == 96, "cursor PNG second partial alpha") && ok;
        }
        sx_png_free(&img);
        if (!ok) return 1;
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
    bmp_tests();
    bmp_extended_tests();
    png_into_tests();

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
