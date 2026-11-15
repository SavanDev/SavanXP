#pragma once

#include <stddef.h>
#include <stdint.h>

/* System PNG decoder: noninterlaced 8-bit RGB/RGBA, all PNG row filters
 * and deflate block types. Zlib headers and Adler-32 are checked; chunk CRCs
 * are not. Palette, grayscale, tRNS, Adam7 and other depths are rejected.
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

/* System BMP decoder: BI_RGB at 24 or 32 bpp with a BITMAPINFOHEADER or a
 * larger header, rows bottom-up or top-down (negative height). The fourth
 * byte of a 32 bpp pixel is ignored. Palettes, 16 bpp, BI_BITFIELDS and RLE
 * are rejected. Like the PNG decoder it works on memory only; the caller
 * owns the pixels, so a program can decode into a section or a bitmap of
 * its own. Roadmap: docs/roadmaps/open/IMAGE_VIEWER_ROADMAP.md. */

#define SX_BMP_MAX_DIMENSION 4096u

/* Reads the size from the headers and checks that the whole image is in
 * the buffer and within the subset. Returns 0 or a negative error. */
int sx_bmp_info(const unsigned char *data, size_t size, uint32_t *width, uint32_t *height);

/* Decodes into `pixels`, top-down 0x00RRGGBB, `pixel_count` entries long
 * (at least width*height). Returns 0 or a negative error; on error the
 * buffer may hold part of the image. */
int sx_bmp_decode_into(const unsigned char *data, size_t size, uint32_t *pixels, size_t pixel_count);

#ifdef __cplusplus
}
#endif
