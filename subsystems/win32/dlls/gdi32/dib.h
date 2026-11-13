/*
 * dib.h -- Device-independent bitmaps as resources and files hold them: a
 * BITMAPCOREHEADER (12 bytes, three-byte palette entries) or a
 * BITMAPINFOHEADER (40 bytes, four-byte ones), then the palette, then the
 * rows, bottom-up unless the height is negative. Uncompressed only, at 1, 4,
 * 8, 24 and 32 bits per pixel: cards.dll holds 1, 4 and 24 in both headers.
 * Pure C, compiled into gdi32.dll and into the host test.
 */
#ifndef SAVANXP_WIN32_DIB_H
#define SAVANXP_WIN32_DIB_H

typedef struct sxw_dib {
    int width;
    int height;
    int bpp;
    int top_down;
    unsigned int stride;          /* bytes per row, a multiple of 4 */
    const unsigned char *bits;    /* the first row in memory */
    unsigned int palette[256];    /* 0x00RRGGBB */
    unsigned int palette_count;
} sxw_dib;

/* Reads the header and palette of a DIB of size bytes (no BITMAPFILEHEADER)
 * and checks its rows lie inside. 0, or -1 for anything else. */
int sxw_dib_parse(const void *data, unsigned int size, sxw_dib *out);

/* Row y, counted from the top, as 0x00RRGGBB pixels. */
void sxw_dib_row(const sxw_dib *dib, int y, unsigned int *out);

/* Whether LoadBitmap makes it a monochrome bitmap: one bit per pixel with a
 * black first and white second palette entry, as Windows decides. */
int sxw_dib_monochrome(const sxw_dib *dib);

#endif
