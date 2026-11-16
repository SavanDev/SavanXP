/* Host tests for libsximg: construct images in memory and compare exact
 * pixels without QEMU. PNG covers stored/fixed/dynamic deflate, every row
 * filter, all colour types/depths, tRNS and Adam7; BMP covers palettes,
 * bitfields, alpha and RLE.
 * Guest scenarios prove that consumers load the library and read files. */
#include <stdint.h>

extern "C" {
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
}

#include <vector>
#include <utility>

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

/* The common entry point must preserve the per-format pixel contract. */
bool metadata_matches(const sx_image_metadata& a, const sx_image_metadata& b) {
    return a.format == b.format && a.source_width == b.source_width && a.source_height == b.source_height &&
           a.width == b.width && a.height == b.height && a.scale_denom == b.scale_denom;
}

bool common_pixels(const unsigned char* data, size_t size, uint32_t format,
                   uint32_t w, uint32_t h, const uint32_t* expected) {
    sx_image_metadata wanted = {format,w,h,w,h,1}, info = {}, decoded = {};
    sx_image_options zero = {}, exact = {w,h,w*h,1};
    if (sx_image_format(data,size) != format ||
        sx_image_info(data,size,nullptr,&info) != SX_IMAGE_OK || !metadata_matches(info,wanted) ||
        sx_image_info(data,size,&zero,&info) != SX_IMAGE_OK || !metadata_matches(info,wanted) ||
        sx_image_info(data,size,&exact,&info) != SX_IMAGE_OK || !metadata_matches(info,wanted)) return false;
    std::vector<uint32_t> pixels((size_t)w*h+2,0xdeadbeef);
    if (sx_image_decode_into(data,size,nullptr,pixels.data()+1,(size_t)w*h,&decoded) != SX_IMAGE_OK ||
        !metadata_matches(decoded,wanted) || pixels.front() != 0xdeadbeef || pixels.back() != 0xdeadbeef ||
        memcmp(pixels.data()+1,expected,(size_t)w*h*sizeof(uint32_t)) != 0) return false;
    return sx_image_decode_into(data,size,&exact,pixels.data()+1,(size_t)w*h,nullptr) == SX_IMAGE_OK &&
           pixels.front() == 0xdeadbeef && pixels.back() == 0xdeadbeef &&
           memcmp(pixels.data()+1,expected,(size_t)w*h*sizeof(uint32_t)) == 0;
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
    return common_pixels(bmp.bytes.data(),bmp.bytes.size(),SX_IMAGE_FORMAT_BMP,w,h,pixels.data());
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
           std::vector<uint32_t>(pixels.begin() + 1, pixels.end() - 1) == expected &&
           (argb || common_pixels(bmp.bytes.data(),bmp.bytes.size(),SX_IMAGE_FORMAT_BMP,w,h,expected.data()));
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

/* Build PNG image data independently of the decoder's pass geometry: this
 * tile labels the pass of each coordinate, as in the PNG specification. */
static const unsigned char png_pass_tile[8][8] = {
    {1,6,4,6,2,6,4,6}, {7,7,7,7,7,7,7,7},
    {5,6,5,6,5,6,5,6}, {7,7,7,7,7,7,7,7},
    {3,6,4,6,3,6,4,6}, {7,7,7,7,7,7,7,7},
    {5,6,5,6,5,6,5,6}, {7,7,7,7,7,7,7,7}
};

std::vector<unsigned char> png_stored_stream(const std::vector<unsigned char>& raw) {
    std::vector<unsigned char> stream = {0x78,0x01};
    size_t at = 0;
    do {
        size_t n = raw.size() - at;
        if (n > 65535u) n = 65535u;
        stream.push_back(at + n == raw.size() ? 1 : 0);
        stream.push_back((unsigned char)n); stream.push_back((unsigned char)(n >> 8));
        stream.push_back((unsigned char)~n); stream.push_back((unsigned char)(~n >> 8));
        stream.insert(stream.end(), raw.begin() + at, raw.begin() + at + n);
        at += n;
    } while (at < raw.size());
    uint32_t sum = adler(raw);
    for (int shift = 24; shift >= 0; shift -= 8) stream.push_back((unsigned char)(sum >> shift));
    return stream;
}

Png png_raw(uint32_t w, uint32_t h, unsigned type, unsigned depth, unsigned interlace,
            const std::vector<unsigned char>& raw, const std::vector<unsigned char>& palette = {},
            const std::vector<unsigned char>* transparency = nullptr) {
    Png png;
    png.header(w, h, (unsigned char)type, (unsigned char)depth, (unsigned char)interlace);
    if (!palette.empty()) png.chunk("PLTE", palette);
    if (transparency) png.chunk("tRNS", *transparency);
    png.chunk("IDAT", png_stored_stream(raw));
    png.end();
    return png;
}

std::vector<unsigned char> png_sample_rows(uint32_t w, uint32_t h, unsigned channels, unsigned depth,
                                          bool interlaced, unsigned filter,
                                          const std::vector<uint32_t>& samples) {
    std::vector<unsigned char> raw;
    for (unsigned pass = interlaced ? 1u : 0u; pass <= (interlaced ? 7u : 0u); ++pass) {
        std::vector<unsigned char> previous;
        for (uint32_t y = 0; y < h; ++y) {
            std::vector<uint32_t> row_samples;
            for (uint32_t x = 0; x < w; ++x) {
                if (interlaced && png_pass_tile[y % 8][x % 8] != pass) continue;
                for (unsigned c = 0; c < channels; ++c) row_samples.push_back(samples[((size_t)y * w + x)*channels + c]);
            }
            if (row_samples.empty()) continue;
            std::vector<unsigned char> row((row_samples.size() * depth + 7u) / 8u, 0);
            for (size_t i = 0; i < row_samples.size(); ++i) {
                uint32_t v = row_samples[i];
                if (depth == 16) { row[2*i] = (unsigned char)(v >> 8); row[2*i+1] = (unsigned char)v; }
                else if (depth == 8) row[i] = (unsigned char)v;
                else row[i * depth / 8] |= (unsigned char)(v << (8 - depth - (i * depth % 8)));
            }
            unsigned bpp = (channels * depth + 7u) / 8u;
            raw.push_back((unsigned char)filter);
            for (size_t i = 0; i < row.size(); ++i) {
                int left = i >= bpp ? row[i-bpp] : 0;
                int above = previous.empty() ? 0 : previous[i];
                int corner = !previous.empty() && i >= bpp ? previous[i-bpp] : 0;
                int prediction = filter == 1 ? left : filter == 2 ? above : filter == 3 ? (left+above)/2 :
                                 filter == 4 ? Png::paeth(left,above,corner) : 0;
                raw.push_back((unsigned char)(row[i] - prediction));
            }
            previous = row;
        }
    }
    return raw;
}

uint32_t png_white(uint32_t argb) {
    uint32_t a = argb >> 24, result = 0;
    for (unsigned shift : {0u,8u,16u}) {
        uint32_t colour = (argb >> shift) & 255;
        result |= ((colour*a + 255*(255-a) + 127)/255) << shift;
    }
    return result;
}

bool png_pixels(const Png& png, uint32_t width, uint32_t height, const std::vector<uint32_t>& argb) {
    uint32_t w = 0, h = 0;
    if (sx_png_info(png.bytes.data(), png.bytes.size(), &w, &h) != 0 || w != width || h != height ||
        argb.size() != (size_t)w*h) return false;
    sx_png_image rgb = {}, alpha = {};
    bool ok = sx_png_decode(png.bytes.data(), png.bytes.size(), &rgb) == 0 &&
              sx_png_decode_argb(png.bytes.data(), png.bytes.size(), &alpha) == 0;
    std::vector<uint32_t> into(argb.size()+2, 0xdeadbeef);
    ok = ok && rgb.width == w && rgb.height == h && alpha.width == w && alpha.height == h &&
         sx_png_decode_into(png.bytes.data(), png.bytes.size(), into.data()+1, argb.size()) == 0 &&
         into.front() == 0xdeadbeef && into.back() == 0xdeadbeef;
    for (size_t i = 0; ok && i < argb.size(); ++i) {
        ok = alpha.pixels[i] == argb[i] && rgb.pixels[i] == png_white(argb[i]) && into[i+1] == rgb.pixels[i];
    }
    if (ok) ok = common_pixels(png.bytes.data(),png.bytes.size(),SX_IMAGE_FORMAT_PNG,w,h,rgb.pixels);
    sx_png_free(&rgb); sx_png_free(&alpha);
    return ok;
}

bool png_refused(const Png& png) {
    sx_png_image rgb = {}, alpha = {};
    uint32_t into[258];
    for (auto& v : into) v = 0xdeadbeef;
    bool ok = sx_png_decode(png.bytes.data(), png.bytes.size(), &rgb) < 0 && !rgb.pixels && !rgb.width && !rgb.height;
    ok = (sx_png_decode_argb(png.bytes.data(), png.bytes.size(), &alpha) < 0 &&
          !alpha.pixels && !alpha.width && !alpha.height) && ok;
    ok = sx_png_decode_into(png.bytes.data(), png.bytes.size(), into+1, 256) < 0 &&
         into[0] == 0xdeadbeef && into[257] == 0xdeadbeef && ok;
    sx_png_free(&rgb); sx_png_free(&alpha);
    return ok;
}

void png_format_case(unsigned type, unsigned depth, bool interlaced, unsigned filter, uint32_t w, uint32_t h) {
    unsigned channels = type == 2 ? 3 : type == 4 ? 2 : type == 6 ? 4 : 1;
    uint32_t max = (1u << depth)-1;
    std::vector<unsigned char> palette, trns;
    unsigned count = type == 3 ? 1u << depth : 0;
    for (unsigned i = 0; i < count; ++i) {
        palette.push_back((unsigned char)(i*31)); palette.push_back((unsigned char)(i*71));
        palette.push_back((unsigned char)(i*113));
        if (i+1 < count) trns.push_back(i % 3 == 0 ? 0 : i % 3 == 1 ? 128 : 255);
    }
    std::vector<uint32_t> samples((size_t)w*h*channels), expected((size_t)w*h);
    for (size_t i = 0; i < samples.size(); ++i) samples[i] = (uint32_t)((i*12347u + (i*i/2u)*173u) & max);
    if (type == 0 || type == 2) trns.assign(channels*2, 0); // first pixel's samples are the key
    for (unsigned c = 0; (type == 0 || type == 2) && c < channels; ++c) {
        trns[2*c] = (unsigned char)(samples[c] >> 8); trns[2*c+1] = (unsigned char)samples[c];
    }
    for (size_t i = 0; i < expected.size(); ++i) {
        uint32_t r, g, b, a = 255;
        const uint32_t* v = samples.data() + i*channels;
        auto scale = [max](uint32_t x) { return (x*255u + max/2u)/max; };
        if (type == 3) {
            r = palette[v[0]*3]; g = palette[v[0]*3+1]; b = palette[v[0]*3+2];
            if (v[0] < trns.size()) a = trns[v[0]];
        } else {
            r = scale(v[0]); g = b = r;
            if (type == 2 || type == 6) { g = scale(v[1]); b = scale(v[2]); }
            if (type == 4 || type == 6) a = scale(v[channels-1]);
            if ((type == 0 || type == 2) && v[0] == samples[0] &&
                (type == 0 || (v[1] == samples[1] && v[2] == samples[2]))) a = 0;
        }
        expected[i] = (a << 24) | (r << 16) | (g << 8) | b;
    }
    auto raw = png_sample_rows(w,h,channels,depth,interlaced,filter,samples);
    auto png = png_raw(w,h,type,depth,interlaced,raw,palette,
                       type == 0 || type == 2 || type == 3 ? &trns : nullptr);
    char label[128];
    snprintf(label,sizeof(label),"png type %u depth %u interlace %u filter %u %ux%u exact RGB/ARGB/into",
             type,depth,(unsigned)interlaced,filter,w,h);
    check(png_pixels(png,w,h,expected), label);
}

void png_extended_tests() {
    for (unsigned type : {0u,2u,3u,4u,6u}) for (unsigned depth : {1u,2u,4u,8u,16u}) {
        if ((type == 3 && depth == 16) || (type != 0 && type != 3 && depth < 8)) continue;
        for (bool interlaced : {false,true}) for (unsigned filter = 0; filter < 5; ++filter)
            png_format_case(type,depth,interlaced,filter,17,13);
        // Narrow/short images leave several Adam7 passes empty, including their filter bytes.
        for (const auto& size : {std::pair<unsigned,unsigned>{1,1},{1,9},{9,1},{2,3},{3,2},{5,5},{8,8}})
            png_format_case(type,depth,true,4,size.first,size.second);
    }
    {
        std::vector<unsigned char> key = {0x12,0x34};
        auto grey = png_raw(3,1,0,16,0,{0,0x12,0x34,0x12,0x35,0xff,0xff},{},&key);
        check(png_pixels(grey,3,1,{0x00121212,0xff121212,0xffffffff}), "png grey16 tRNS compares full precision before scaling");
        key = {0x12,0x34,0x56,0x78,0x9a,0xbc};
        auto rgb = png_raw(2,1,2,16,0,{0,0x12,0x34,0x56,0x78,0x9a,0xbc,0x12,0x35,0x56,0x78,0x9a,0xbc},{},&key);
        check(png_pixels(rgb,2,1,{0x0012569a,0xff12569a}), "png RGB16 tRNS compares low bytes too");
        auto ga = png_raw(3,1,4,16,0,{0,0x80,0x00,0x80,0x00,0xff,0xff,0,0,0,0,0xff,0xff});
        check(png_pixels(ga,3,1,{0x80808080,0x00ffffff,0xff000000}), "png grey-alpha16 endpoints and half alpha");
        auto rgba = png_raw(2,1,6,16,0,{0,0,0x80,0,0x81,0xff,0x7f,0x80,0, 0xff,0xff,0,0,0,0,0xff,0xff});
        check(png_pixels(rgba,2,1,{0x800001ff,0xffff0000}), "png RGBA16 uses both bytes and nearest sample rounding");
    }
    check(png_pixels(png_raw(7,1,0,1,0,{0,0xa5}),7,1,
                     {0xffffffff,0xff000000,0xffffffff,0xff000000,0xff000000,0xffffffff,0xff000000}),
          "png grey1 reads MSB first and ignores padding bits");
    check(png_pixels(png_raw(4,1,0,2,0,{0,0x1b}),4,1,
                     {0xff000000,0xff555555,0xffaaaaaa,0xffffffff}), "png grey2 expands all four levels");
    check(png_pixels(png_raw(5,1,0,4,0,{0,0x01,0x78,0xff}),5,1,
                     {0xff000000,0xff111111,0xff777777,0xff888888,0xffffffff}),
          "png grey4 expands nibbles and ignores the unused nibble");
    for (unsigned depth : {1u,2u,4u,8u}) {
        std::vector<unsigned char> key = {0xff,0xfe};
        uint32_t value = ((1u << depth)-1u) & 0xfe;
        auto raw = png_sample_rows(1,1,1,depth,false,0,{value});
        auto grey = png_raw(1,1,0,depth,0,raw,{},&key);
        uint32_t channel = value * 255u / ((1u << depth)-1u);
        check(png_pixels(grey,1,1,{channel*0x010101u}), "png tRNS ignores bits above the declared sample depth");
    }
    {
        const std::vector<unsigned char> palette = {12,34,56,78,90,123};
        auto rgb = png_raw(1,1,2,8,0,{0,11,22,33},palette);
        check(png_pixels(rgb,1,1,{0xff0b1621}), "png accepts optional truecolour PLTE without changing pixels");
        auto rgba = png_raw(1,1,6,8,0,{0,11,22,33,128},palette);
        check(png_pixels(rgba,1,1,{0x800b1621}), "png accepts optional RGBA PLTE");
        auto indexed = png_raw(1,1,3,2,0,{0,0x40},palette);
        check(png_pixels(indexed,1,1,{0xff4e5a7b}), "png indexed without tRNS defaults to opaque");
        std::vector<unsigned char> empty;
        indexed = png_raw(1,1,3,2,0,{0,0x40},palette,&empty);
        check(png_pixels(indexed,1,1,{0xff4e5a7b}), "png empty palette tRNS keeps entries opaque");
    }
    for (unsigned type : {0u,2u,3u,4u,6u}) for (unsigned depth : {0u,1u,2u,3u,4u,8u,12u,16u,32u}) {
        bool valid = (depth == 1 || depth == 2 || depth == 4 || depth == 8 || depth == 16) &&
                     (type != 3 || depth != 16) && (type == 0 || type == 3 || depth >= 8);
        if (valid) continue;
        Png png; png.header(1,1,(unsigned char)type,(unsigned char)depth);
        check(sx_png_info(png.bytes.data(),png.bytes.size(),nullptr,nullptr) < 0 && png_refused(png),
              "png rejects invalid colour type/depth pairs in info and decode");
    }
    for (unsigned type : {1u,5u,7u,255u}) {
        Png png; png.header(1,1,(unsigned char)type);
        check(sx_png_info(png.bytes.data(),png.bytes.size(),nullptr,nullptr) < 0 && png_refused(png),
              "png rejects unknown colour types");
    }
    auto base = png_raw(1,1,2,8,0,{0,12,34,56});
    for (size_t at : {size_t{26},size_t{27},size_t{28}}) {
        auto bad = base; bad.bytes[at] = 2;
        check(sx_png_info(bad.bytes.data(),bad.bytes.size(),nullptr,nullptr) < 0 && png_refused(bad),
              "png rejects unsupported compression/filter/interlace methods");
    }
    for (uint32_t dimension : {0u,4097u,0xffffffffu}) for (size_t at : {size_t{16},size_t{20}}) {
        auto bad = base;
        for (int i = 0; i < 4; ++i) bad.bytes[at+i] = (unsigned char)(dimension >> (24-8*i));
        check(sx_png_info(bad.bytes.data(),bad.bytes.size(),nullptr,nullptr) < 0 && png_refused(bad),
              "png rejects invalid and oversized dimensions");
    }
    for (unsigned type : {0u,2u,3u,4u,6u}) {
        unsigned channels = type == 2 ? 3 : type == 4 ? 2 : type == 6 ? 4 : 1;
        std::vector<unsigned char> raw(1+channels,0);
        raw[0] = 9;
        auto bad = png_raw(1,1,type,8,0,raw,type == 3 ? std::vector<unsigned char>{0,0,0} :
                                                                   std::vector<unsigned char>{});
        check(png_refused(bad), "png rejects invalid filter with otherwise valid data for every colour type");
    }
    const std::vector<unsigned char> palette = {0,0,0,255,0,0};
    for (auto bad_palette : {std::vector<unsigned char>{}, {0,0}, {0,0,0,0}, std::vector<unsigned char>(771,0),
                             std::vector<unsigned char>(9,0)}) {
        Png png; png.header(1,1,3,1); png.chunk("PLTE",bad_palette);
        png.chunk("IDAT",png_stored_stream({0,0})); png.end();
        check(png_refused(png), "png rejects empty, malformed or excessive palettes");
    }
    for (unsigned type : {0u,4u}) {
        auto png = png_raw(1,1,type,8,0,type == 0 ? std::vector<unsigned char>{0,0} : std::vector<unsigned char>{0,0,255},palette);
        check(png_refused(png), "png rejects a palette on greyscale images");
    }
    for (unsigned type : {0u,2u,3u,4u,6u}) for (size_t length : {0u,1u,2u,3u,6u,7u}) {
        bool valid = type == 0 ? length == 2 : type == 2 ? length == 6 : type == 3 ? length <= 2 : false;
        if (valid) continue;
        std::vector<unsigned char> trns(length,0);
        unsigned channels = type == 2 ? 3 : type == 4 ? 2 : type == 6 ? 4 : 1;
        auto png = png_raw(1,1,type,8,0,std::vector<unsigned char>(1+channels,0),
                           type == 3 ? palette : std::vector<unsigned char>{},&trns);
        check(png_refused(png), "png rejects invalid tRNS size or tRNS on an alpha colour type");
    }
    for (unsigned depth : {1u,2u,4u,8u}) for (bool interlaced : {false,true}) {
        auto raw = png_sample_rows(3,2,1,depth,interlaced,0,std::vector<uint32_t>(6,1));
        auto png = png_raw(3,2,3,depth,interlaced,raw,{1,2,3});
        check(png_refused(png), "png rejects indices outside PLTE, including packed/Adam7 pixels");
    }
    for (auto raw : {std::vector<unsigned char>{0,0,0}, {0,0,0,0,0}, {9,0,0,0}}) {
        check(png_refused(png_raw(1,1,2,8,0,raw)), "png rejects short/long scanlines and invalid row filters");
    }
    {
        // Split the zlib stream inside its header, blocks and Adler checksum.
        auto stream = png_stored_stream({0,12,34,56});
        Png png; png.header(1,1,2);
        for (auto byte : stream) png.chunk("IDAT",{byte});
        png.chunk("IDAT",{}); png.chunk("tEXt",{'x',0,'y'}); png.end();
        check(png_pixels(png,1,1,{0xff0c2238}), "png accepts consecutive split/empty IDAT and ignored ancillary chunks");
        png = {}; png.header(1,1,2); png.chunk("IDAT",{stream[0]}); png.chunk("tEXt",{});
        png.chunk("IDAT",std::vector<unsigned char>(stream.begin()+1,stream.end())); png.end();
        check(png_refused(png), "png rejects nonconsecutive IDAT chunks");
        png = {}; png.header(1,1,2); png.chunk("ABCD",{}); png.chunk("IDAT",stream); png.end();
        check(png_refused(png), "png rejects unknown critical chunks");
    }
    {
        Png png; png.header(1,1,3); png.chunk("IDAT",png_stored_stream({0,0})); png.end();
        check(png_refused(png), "png indexed data requires an earlier PLTE");
        png = {}; png.header(1,1,3); png.chunk("tRNS",{0}); png.chunk("PLTE",palette);
        png.chunk("IDAT",png_stored_stream({0,0})); png.end();
        check(png_refused(png), "png tRNS requires an earlier PLTE for indexed images");
        for (const char* type : {"PLTE","tRNS"}) {
            auto body = type[0] == 'P' ? palette : std::vector<unsigned char>{0};
            png = {}; png.header(1,1,3); png.chunk("PLTE",palette);
            if (type[0] == 't') png.chunk("tRNS",body);
            png.chunk(type,body); png.chunk("IDAT",png_stored_stream({0,0})); png.end();
            check(png_refused(png), "png rejects duplicate PLTE/tRNS");
            png = {}; png.header(1,1,3); png.chunk("PLTE",palette);
            png.chunk("IDAT",png_stored_stream({0,0})); png.chunk(type,body); png.end();
            check(png_refused(png), "png rejects PLTE/tRNS after image data");
        }
        png = {}; png.header(1,1,2); png.header(1,1,2); png.chunk("IDAT",png_stored_stream({0,0,0,0})); png.end();
        check(png_refused(png), "png rejects a duplicate IHDR");
        png = {}; png.chunk("IDAT",png_stored_stream({0,0,0,0})); png.end();
        check(png_refused(png), "png requires IHDR first");
    }
    std::vector<Png> complete = {
        base, png_raw(1,1,3,1,0,{0,0},palette),
        png_raw(1,1,4,16,1,{0,0xff,0xff,0x80,0}),
        png_raw(1,1,6,16,1,{0,0,0,0,0,0,0,0,0})
    };
    for (const auto& png : complete) {
        bool ok = true;
        for (size_t length = 0; length < png.bytes.size(); ++length) {
            auto short_png = png; short_png.bytes.resize(length);
            ok = png_refused(short_png) && ok;
        }
        check(ok,"png rejects every truncated prefix, including a missing IEND");
        std::vector<uint32_t> into(2,0xdeadbeef);
        check(sx_png_decode_into(png.bytes.data(),png.bytes.size(),nullptr,1) < 0 &&
              sx_png_decode_into(png.bytes.data(),png.bytes.size(),into.data()+1,0) < 0 &&
              into[0] == 0xdeadbeef && into[1] == 0xdeadbeef,
              "png new formats preserve destination bounds on null/short buffers");
    }
    check(sx_png_info(nullptr,0,nullptr,nullptr) < 0 && sx_png_decode(nullptr,0,nullptr) < 0,
          "png null inputs and output are rejected");
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
            ok = ok && common_pixels(fixture.data,fixture.size,SX_IMAGE_FORMAT_PNG,w,h,img.pixels);
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
        check(sx_png_info(palette.bytes.data(), palette.bytes.size(), &w, &h) == 0 &&
                  sx_png_info(grey16.bytes.data(), grey16.bytes.size(), &w, &h) == 0 &&
                  sx_png_info(interlaced.bytes.data(), interlaced.bytes.size(), &w, &h) == 0 && w == 4 && h == 4,
              "png info accepts palette, 16-bit grey and Adam7 headers without image data");
        check(sx_png_info(rgb.bytes.data(), rgb.bytes.size(), &w, &h) == 0 && w == 5 && h == 3 &&
                  sx_png_info(rgb.bytes.data(), rgb.bytes.size() - 1u, &w, &h) != 0,
              "png info reads IHDR alone and rejects a short one");
        Bmp bmp(4, 4, 24);
        check(sx_png_info(bmp.bytes.data(), bmp.bytes.size(), &w, &h) != 0 &&
                  sx_bmp_info(k_png_rgb_dynamic, sizeof(k_png_rgb_dynamic), &w, &h) != 0,
              "png and bmp info each refuse the other's file");
    }
}

/* Errors before decode leave pixels intact; a failed decode may be partial. */
bool common_error(const std::vector<unsigned char>& data, const sx_image_options* options, int expected,
                  size_t count = 32, bool info_error = true, bool pixels_unchanged = true) {
    sx_image_metadata info = {9,9,9,9,9,9}, decoded = info, zero = {};
    uint32_t pixels[34];
    for (auto& p : pixels) p = 0xdeadbeef;
    int header_result = sx_image_info(data.data(),data.size(),options,&info);
    bool ok = info_error ? header_result == expected && metadata_matches(info,zero) : header_result == SX_IMAGE_OK;
    ok = sx_image_decode_into(data.data(),data.size(),options,pixels+1,count,&decoded) == expected &&
         metadata_matches(decoded,zero) && pixels[0] == 0xdeadbeef && pixels[33] == 0xdeadbeef && ok;
    if (pixels_unchanged) for (auto p : pixels) ok = p == 0xdeadbeef && ok;
    return ok;
}

void common_image_tests() {
    const unsigned char png_sig[] = {137,80,78,71,13,10,26,10};
    for (size_t length = 0; length <= sizeof(png_sig); ++length) {
        check(sx_image_format(png_sig,length) == (length == 8 ? SX_IMAGE_FORMAT_PNG : SX_IMAGE_FORMAT_UNKNOWN),
              "common format detection respects PNG signature extent");
    }
    check(sx_image_format(nullptr,100) == SX_IMAGE_FORMAT_UNKNOWN &&
          sx_image_format((const unsigned char*)"BM",1) == SX_IMAGE_FORMAT_UNKNOWN &&
          sx_image_format((const unsigned char*)"BM",2) == SX_IMAGE_FORMAT_BMP,
          "common format detection handles null input and short BMP signatures");
    check(common_error({'B','M'},nullptr,SX_IMAGE_ERROR_HEADER) &&
          common_error(std::vector<unsigned char>(png_sig,png_sig+8),nullptr,SX_IMAGE_ERROR_HEADER),
          "recognized signatures do not imply valid headers");
    check(common_error({0,1,2,3},nullptr,SX_IMAGE_ERROR_FORMAT), "unknown image format has its own error");
    check(sx_image_format((const unsigned char*)"\xff\xd8",2) == SX_IMAGE_FORMAT_JPEG &&
          common_error({0xff,0xd8,0xff,0xd9},nullptr,SX_IMAGE_ERROR_UNSUPPORTED),
          "JPEG is recognized but not advertised as decodable");
    Png png = png_raw(3,2,2,8,0,{0,1,2,3,4,5,6,7,8,9,0,9,8,7,6,5,4,3,2,1});
    Bmp bmp(3,2,24,true);
    for (const auto& bytes : {png.bytes,bmp.bytes}) {
        for (uint32_t denominator : {2u,4u,8u}) {
            sx_image_options options = {0,0,0,denominator};
            check(common_error(bytes,&options,SX_IMAGE_ERROR_SCALE), "BMP/PNG exact reduction is explicitly unsupported");
        }
        for (uint32_t denominator : {3u,16u,0xffffffffu}) {
            sx_image_options options = {0,0,0,denominator};
            check(common_error(bytes,&options,SX_IMAGE_ERROR_ARGUMENT), "invalid scale denominators are argument errors");
        }
        for (auto limits : {sx_image_options{2,0,0,0},{0,1,0,0},{0,0,5,0},{1,1,1,0}}) {
            check(common_error(bytes,&limits,SX_IMAGE_ERROR_LIMIT), "width, height and pixel budgets are enforced before writes");
        }
        check(common_error(bytes,nullptr,SX_IMAGE_ERROR_BUFFER,5,false), "common decode rejects one-pixel-short buffers before writes");
        check(common_error(bytes,nullptr,SX_IMAGE_ERROR_BUFFER,0,false), "common decode rejects zero destination capacity");
        sx_image_metadata info = {9,9,9,9,9,9}, zero = {};
        check(sx_image_decode_into(bytes.data(),bytes.size(),nullptr,nullptr,6,&info) == SX_IMAGE_ERROR_ARGUMENT &&
              metadata_matches(info,zero), "common decode rejects null pixels and clears metadata");
        check(sx_image_info(bytes.data(),bytes.size(),nullptr,nullptr) == SX_IMAGE_ERROR_ARGUMENT,
              "common info requires output metadata");
        sx_image_options large = {0xffffffffu,0xffffffffu,0xffffffffu,0};
        check(sx_image_info(bytes.data(),bytes.size(),&large,&info) == SX_IMAGE_OK && info.width == 3 &&
              info.height == 2 && info.scale_denom == 1, "larger option limits retain the normal dimensions");
    }
    sx_image_metadata info = {9,9,9,9,9,9}, zero = {};
    uint32_t pixel = 0xdeadbeef;
    check(sx_image_info(nullptr,0,nullptr,&info) == SX_IMAGE_ERROR_ARGUMENT && metadata_matches(info,zero) &&
          sx_image_decode_into(nullptr,0,nullptr,&pixel,1,nullptr) == SX_IMAGE_ERROR_ARGUMENT && pixel == 0xdeadbeef,
          "null data is an argument error without pixel writes");
    // Header-only PNG is a valid plan; missing image data is a decode failure.
    auto header_only = png; header_only.bytes.resize(33);
    check(common_error(header_only.bytes,nullptr,SX_IMAGE_ERROR_DECODE,6,false),
          "a successful header plan does not promise decodable pixel data");
    auto malformed_png = png;
    malformed_png.bytes[25] = 5;
    check(common_error(malformed_png.bytes,nullptr,SX_IMAGE_ERROR_HEADER), "unsupported PNG header reports HEADER");
    auto bad_palette = png_raw(3,1,3,8,0,{0,0,0,1},{0,0,0});
    check(common_error(bad_palette.bytes,nullptr,SX_IMAGE_ERROR_DECODE,3,false,false),
          "common decode propagates late PNG data failure with reset metadata");
    auto bad_rle = bmp_fixture(3,1,8,1,{2,0,2,1,0,1},1);
    check(common_error(bad_rle.bytes,nullptr,SX_IMAGE_ERROR_DECODE,3,false,false),
          "common decode propagates late BMP data failure with reset metadata");
    for (size_t dimension_at : {size_t{16},size_t{20}}) {
        auto huge = png;
        uint32_t dimension = SX_IMAGE_MAX_DIMENSION+1u;
        for (unsigned i=0; i<4; ++i) huge.bytes[dimension_at+i] = (unsigned char)(dimension >> (24u-8u*i));
        sx_image_options large = {0xffffffffu,0xffffffffu,0xffffffffu,0};
        check(common_error(huge.bytes,&large,SX_IMAGE_ERROR_LIMIT), "PNG global dimension limit cannot be raised by options");
        check(sx_png_info(huge.bytes.data(),huge.bytes.size(),nullptr,nullptr) < 0,
              "legacy PNG dimension limit remains intact");
    }
    for (size_t dimension_at : {size_t{18},size_t{22}}) {
        auto huge = bmp; huge.le32(dimension_at,SX_IMAGE_MAX_DIMENSION+1u);
        sx_image_options large = {0xffffffffu,0xffffffffu,0xffffffffu,0};
        check(common_error(huge.bytes,&large,SX_IMAGE_ERROR_LIMIT), "BMP global dimension limit cannot be raised by options");
        check(sx_bmp_info(huge.bytes.data(),huge.bytes.size(),nullptr,nullptr) < 0,
              "legacy BMP dimension limit remains intact");
    }
    // JPEG is not inspected beyond its signature until the next delivery.
    sx_image_options exact = {0,0,0,8};
    check(common_error({0xff,0xd8},&exact,SX_IMAGE_ERROR_UNSUPPORTED), "JPEG scale requests do not fake JPEG support");
    const struct {int code; const char* message;} messages[] = {
        {SX_IMAGE_OK,"Success"}, {SX_IMAGE_ERROR_ARGUMENT,"Invalid image argument"},
        {SX_IMAGE_ERROR_FORMAT,"Unrecognized image format"}, {SX_IMAGE_ERROR_UNSUPPORTED,"Unsupported image format"},
        {SX_IMAGE_ERROR_HEADER,"Invalid or unsupported image header"}, {SX_IMAGE_ERROR_LIMIT,"Image exceeds decode limits"},
        {SX_IMAGE_ERROR_SCALE,"Image decoder does not support the requested scale"},
        {SX_IMAGE_ERROR_BUFFER,"Image buffer is too small"}, {SX_IMAGE_ERROR_DECODE,"Image decoding failed"},
        {12345,"Unknown image error"}
    };
    for (const auto& test : messages) check(strcmp(sx_image_error_string(test.code),test.message)==0,
                                          "common errors have deterministic English diagnostics");
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
        /* An Adam7 header with ordinary noninterlaced rows is malformed. */
        {
            Png png;
            png.header(4, 4, 2, 8, 1);
            png.idat_stored_rows(4, 4, 3);
            png.end();
            check(decode(png.bytes.data(), png.bytes.size(), &img) != 0, "rejects noninterlaced data under an Adam7 header");
            sx_png_free(&img);
        }
        /* Indexed PNG without its required PLTE. */
        {
            Png png;
            png.header(4, 4, 3);
            png.idat_stored_rows(4, 4, 1);
            png.end();
            check(decode(png.bytes.data(), png.bytes.size(), &img) != 0, "rejects indexed PNG without PLTE");
            sx_png_free(&img);
        }
        /* RGB16 requires twice as many sample bytes as RGB8. */
        {
            Png png;
            png.header(4, 4, 2, 16);
            png.idat_stored_rows(4, 4, 3);
            png.end();
            check(decode(png.bytes.data(), png.bytes.size(), &img) != 0, "rejects truncated RGB16 samples");
            sx_png_free(&img);
        }
        /* RGB colour-key transparency is supported. */
        {
            Png png;
            png.header(4, 4, 2);
            std::vector<unsigned char> trns(6, 0);
            png.chunk("tRNS", trns);
            png.idat_stored_rows(4, 4, 3);
            png.end();
            check(decode(png.bytes.data(), png.bytes.size(), &img) == 0, "accepts RGB tRNS");
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
    png_extended_tests();
    common_image_tests();

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
