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

#ifdef __cplusplus
}
#endif
