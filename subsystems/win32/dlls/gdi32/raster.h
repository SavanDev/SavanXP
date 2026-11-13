/*
 * raster.h -- gdi32's pixel work, apart from its objects: fills under a
 * raster operation, lines, single pixels and glyphs, each clipped to a
 * region. Pure C, compiled into gdi32.dll and into the host test
 * (tests/host/win32_window_test.c).
 *
 * Pixels are 0x00RRGGBB, the layout of a 32-bit device-independent bitmap
 * and of SavanXP's surfaces. A COLORREF (0x00BBGGRR) is converted before it
 * gets here.
 */
#ifndef SAVANXP_WIN32_RASTER_H
#define SAVANXP_WIN32_RASTER_H

#include "region.h"

typedef struct sxw_target {
    unsigned int *pixels;
    int stride; /* pixels per row */
    int width;
    int height;
} sxw_target;

/* A ternary raster operation (the high word of SRCCOPY, PATCOPY, ...: its
 * index byte) applied to pattern, source and destination. */
unsigned int sxw_rop3(unsigned int index, unsigned int pattern, unsigned int source, unsigned int destination);
/* A binary one, R2_BLACK (1) to R2_WHITE (16), applied to pen and
 * destination. */
unsigned int sxw_rop2(int rop2, unsigned int pen, unsigned int destination);

/* Fills rect with a solid pattern under a ROP3 that reads no source. */
void sxw_raster_fill(const sxw_target *t, const sxw_region *clip, const sxw_rect *rect, unsigned int rop_index,
                     unsigned int pattern);

/* The points of a line from (x0, y0) towards (x1, y1), the end point left out
 * as Windows draws it, each passed to point(). The count. */
typedef void (*sxw_point_fn)(int x, int y, void *context);
int sxw_line_points(int x0, int y0, int x1, int y1, sxw_point_fn point, void *context);

/* Draws that line under a ROP2 with the pen's colour. */
void sxw_raster_line(const sxw_target *t, const sxw_region *clip, int x0, int y0, int x1, int y1, int rop2,
                     unsigned int color);

/* 1 when (x, y) is inside both the target and the clip. */
int sxw_raster_visible(const sxw_target *t, const sxw_region *clip, int x, int y);

/* One glyph whose top-left pixel lands at (x, y): rows x width coverage
 * bytes, each 0 (left alone) or not (drawn in color). */
void sxw_raster_glyph(const sxw_target *t, const sxw_region *clip, int x, int y, const unsigned char *coverage,
                      int width, int rows, unsigned int color);

#endif
