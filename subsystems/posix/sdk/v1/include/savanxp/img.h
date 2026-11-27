#pragma once

#include <stddef.h>
#include <stdint.h>

/* System PNG decoder: greyscale, RGB, indexed, greyscale-alpha and RGBA at
 * every PNG-defined depth (1/2/4/8/16 as applicable), tRNS, Adam7, all row
 * filters and deflate block types. Zlib headers and Adler-32 are checked;
 * chunk CRCs are not. Samples are rescaled to 8 bits with nearest rounding,
 * after full-depth tRNS comparison. No gamma/profile conversion is applied.
 * Uses only malloc/free/memcpy/memset, with no system calls or global state.
 * The complete input is decoded in memory; there is no encoder. */

#define SX_PNG_MAX_DIMENSION 4096u

struct sx_png_image {
    uint32_t *pixels; /* width*height, top-down; NULL when empty */
    uint32_t width;
    uint32_t height;
};

#ifdef __cplusplus
extern "C" {
#endif

/* Decode onto white, returning top-down 0x00RRGGBB for document images.
 * Returns 0 or a negative error. Free successful output with sx_png_free. */
int sx_png_decode(const unsigned char *data, size_t size, struct sx_png_image *out);

/* Decode to straight-alpha 0xAARRGGBB, retaining transparent cursor pixels. */
int sx_png_decode_argb(const unsigned char *data, size_t size, struct sx_png_image *out);

/* Free the pixels and reset the structure. An empty image is accepted. */
void sx_png_free(struct sx_png_image *image);

/* Reads the size from IHDR and checks the colour type, depth and methods.
 * The rest of the file is not looked at, so a decode may still fail.
 * Returns 0 or a negative error. */
int sx_png_info(const unsigned char *data, size_t size, uint32_t *width, uint32_t *height);

/* As sx_png_decode (onto white, 0x00RRGGBB), into `pixels`, which the caller
 * owns and which holds at least width*height entries. Returns 0 or a
 * negative error; on error the buffer may hold part of the image. */
int sx_png_decode_into(const unsigned char *data, size_t size, uint32_t *pixels, size_t pixel_count);

/* System BMP decoder: INFO or larger headers; BI_RGB at 1/4/8/16/24/32
 * bpp, BI_BITFIELDS at 16/32 bpp (including V4/V5 alpha), BI_RLE4/BI_RLE8.
 * Uncompressed rows may be top-down. BI_RGB's high byte and palette reserved
 * bytes are ignored. Works on memory only; the caller owns the pixels.
 * Roadmap: docs/roadmaps/open/IMAGE_VIEWER_ROADMAP.md. */

#define SX_BMP_MAX_DIMENSION 4096u

/* Reads the size, validates headers, masks, palette and pixel-data extent.
 * Pixel indices and RLE instructions are checked only by decode, which may
 * still fail. Returns 0 or a negative error. */
int sx_bmp_info(const unsigned char *data, size_t size, uint32_t *width, uint32_t *height);

/* Decodes onto white into `pixels`, top-down 0x00RRGGBB, `pixel_count` entries long
 * (at least width*height). Returns 0 or a negative error; on error the
 * buffer may hold part of the image. */
int sx_bmp_decode_into(const unsigned char *data, size_t size, uint32_t *pixels, size_t pixel_count);

/* Same buffer contract, retaining straight alpha as 0xAARRGGBB. Without an
 * explicit alpha mask pixels are opaque. RLE skips use palette entry zero. */
int sx_bmp_decode_argb_into(const unsigned char *data, size_t size, uint32_t *pixels, size_t pixel_count);

/* Common image API. These structures use fixed-width fields in both SUN and
 * POSIX. Detection uses only signatures; successful detection is not validation.
 * Data, options, output metadata and pixels must not overlap. */
#define SX_IMAGE_MAX_DIMENSION 4096u
#define SX_IMAGE_MAX_PIXELS (SX_IMAGE_MAX_DIMENSION * SX_IMAGE_MAX_DIMENSION)

enum sx_image_format_id {
    SX_IMAGE_FORMAT_UNKNOWN = 0,
    SX_IMAGE_FORMAT_BMP = 1,
    SX_IMAGE_FORMAT_PNG = 2,
    SX_IMAGE_FORMAT_JPEG = 3
};

enum sx_image_result {
    SX_IMAGE_OK = 0,
    SX_IMAGE_ERROR_ARGUMENT = -1,
    SX_IMAGE_ERROR_FORMAT = -2,
    SX_IMAGE_ERROR_UNSUPPORTED = -3,
    SX_IMAGE_ERROR_HEADER = -4,
    SX_IMAGE_ERROR_LIMIT = -5,
    SX_IMAGE_ERROR_SCALE = -6,
    SX_IMAGE_ERROR_BUFFER = -7,
    SX_IMAGE_ERROR_DECODE = -8,
    SX_IMAGE_ERROR_JPEG_PROGRESSIVE = -9,
    SX_IMAGE_ERROR_JPEG_ARITHMETIC = -10,
    SX_IMAGE_ERROR_JPEG_PRECISION = -11,
    SX_IMAGE_ERROR_JPEG_LOSSLESS = -12,
    SX_IMAGE_ERROR_JPEG_COLOR = -13,
    SX_IMAGE_ERROR_JPEG_SAMPLING = -14
};

struct sx_image_options {
    uint32_t max_width;   /* zero: global limit; larger values cannot raise it */
    uint32_t max_height;
    uint32_t max_pixels;  /* zero: global pixel budget */
    uint32_t scale_denom; /* 0: auto; otherwise exactly 1, 2, 4 or 8 */
};

struct sx_image_metadata {
    uint32_t format;      /* sx_image_format_id */
    uint32_t source_width;
    uint32_t source_height;
    uint32_t width;       /* decoded dimensions, used to allocate pixels */
    uint32_t height;
    uint32_t scale_denom; /* actual denominator, never zero on success */
};

uint32_t sx_image_format(const unsigned char *data, size_t size);

/* Plan a decode using the same options as decode_into. NULL options means
 * all zeroes. BMP/PNG support denominator 1 only: auto selects 1, an explicit
 * 2/4/8 returns ERROR_SCALE, and an image exceeding any limit returns
 * ERROR_LIMIT. JPEG auto selects the smallest fitting denominator in 1/2/4/8;
 * explicit selection is exact. Output dimensions round up; limits apply to
 * output, so a larger JPEG source can fit at a reduced scale.
 * `out` is required and is zeroed on error. Header validation does not prove
 * that pixel data can be decoded. No allocation or system calls. */
int sx_image_info(const unsigned char *data, size_t size,
                  const struct sx_image_options *options, struct sx_image_metadata *out);

/* Decodes top-down 0x00RRGGBB onto white, exactly as the per-format functions.
 * Options have the same meaning as in info; pixel_count counts uint32_t entries
 * and must hold width*height. `out` is optional, zeroed on error and filled on
 * success. Arguments, headers, limits and buffer capacity are checked before
 * any pixel write. ERROR_DECODE covers corrupt/unsupported pixel data or a
 * decoder resource failure; the buffer may then contain part of the image.
 * Inputs, options, output metadata and destination must not overlap. */
int sx_image_decode_into(const unsigned char *data, size_t size,
                         const struct sx_image_options *options, uint32_t *pixels,
                         size_t pixel_count, struct sx_image_metadata *out);

/* Sequential (SOF0/SOF1) and progressive (SOF2) Huffman JPEG: 8-bit
 * greyscale, RGB or YCbCr, sampling factors 1/2, interleaved/separate scans and
 * restart intervals; progressive adds spectral selection and successive
 * approximation. JFIF and Adobe APP14 select colour interpretation. IDCT
 * reduction supports denominators 1/2/4/8. Options, metadata, errors and
 * buffer contract match the common interface. Info validates through the
 * first SOS without allocating. Decode validates every scan, padding and EOI,
 * using up to three byte planes for colour (at most 3*width*height workspace
 * bytes, using decoded dimensions); sequential greyscale needs no heap. A
 * progressive frame also keeps every coefficient until EOI (128 bytes per
 * MCU-padded block, at most 3*4096*4096 coefficients, else ERROR_LIMIT).
 * SX_IMAGE_ERROR_JPEG_PROGRESSIVE names differential (hierarchical)
 * progressive frames only. Neither call uses floating point. Inputs and
 * outputs must not overlap. */
int sx_jpeg_info(const unsigned char *data, size_t size,
                 const struct sx_image_options *options, struct sx_image_metadata *out);
int sx_jpeg_decode_into(const unsigned char *data, size_t size,
                        const struct sx_image_options *options, uint32_t *pixels,
                        size_t pixel_count, struct sx_image_metadata *out);

/* Static English diagnostic, including a fallback for unknown result codes. */
const char *sx_image_error_string(int result);

#ifdef __cplusplus
}
#endif
