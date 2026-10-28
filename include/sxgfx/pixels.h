#pragma once

/* Pixel and text primitives over a 32-bpp buffer described by a
 * savanxp_fb_info: fills, lines, frames and the three 1-bit faces (UI,
 * caption, monospace). Platform code: no system calls, usable from any
 * subsystem (docs/desktop/PLATFORM_GRAPHICS.md). */

#include <stddef.h>
#include <stdint.h>

#include "abi/savanxp_abi.h"

#ifdef __cplusplus
extern "C" {
#endif

uint32_t gfx_rgb(uint8_t red, uint8_t green, uint8_t blue);
uint32_t gfx_stride_pixels(const struct savanxp_fb_info* info);
size_t gfx_buffer_pixels(const struct savanxp_fb_info* info);
size_t gfx_buffer_bytes(const struct savanxp_fb_info* info);
void gfx_clear(uint32_t* pixels, const struct savanxp_fb_info* info, uint32_t colour);
void gfx_pixel(uint32_t* pixels, const struct savanxp_fb_info* info, int x, int y, uint32_t colour);
void gfx_hline(uint32_t* pixels, const struct savanxp_fb_info* info, int x, int y, int width, uint32_t colour);
void gfx_vline(uint32_t* pixels, const struct savanxp_fb_info* info, int x, int y, int height, uint32_t colour);
void gfx_rect(uint32_t* pixels, const struct savanxp_fb_info* info, int x, int y, int width, int height, uint32_t colour);
void gfx_frame(uint32_t* pixels, const struct savanxp_fb_info* info, int x, int y, int width, int height, uint32_t colour);
int gfx_text_width(const char* text);
int gfx_text_height(void);
void gfx_blit_text(uint32_t* pixels, const struct savanxp_fb_info* info, int x, int y, const char* text, uint32_t colour);
/* Same as gfx_blit_text, but blended pixels are restricted to [clip_x0, clip_x1) x [clip_y0, clip_y1). */
void gfx_blit_text_clip(uint32_t* pixels, const struct savanxp_fb_info* info, int x, int y, const char* text, uint32_t colour,
                         int clip_x0, int clip_y0, int clip_x1, int clip_y1);
/* RetroSans 18px caption variant used by window titles. */
int gfx_text_width_title(const char* text);
int gfx_text_height_title(void);
void gfx_blit_text_title(uint32_t* pixels, const struct savanxp_fb_info* info, int x, int y, const char* text, uint32_t colour);
void gfx_blit_text_title_clip(uint32_t* pixels, const struct savanxp_fb_info* info, int x, int y, const char* text, uint32_t colour,
                              int clip_x0, int clip_y0, int clip_x1, int clip_y1);
/* Monospace UniFont path (fixed 8x16 cell), used for terminal-style output. */
int gfx_cell_width(void);
int gfx_cell_height(void);
int gfx_text_width_mono(const char* text);
void gfx_blit_text_mono(uint32_t* pixels, const struct savanxp_fb_info* info, int x, int y, const char* text, uint32_t colour);
/* Same as gfx_blit_text_mono, but restricted to [clip_x0, clip_x1) x [clip_y0, clip_y1). */
void gfx_blit_text_mono_clip(uint32_t* pixels, const struct savanxp_fb_info* info, int x, int y, const char* text, uint32_t colour,
                             int clip_x0, int clip_y0, int clip_x1, int clip_y1);
/* Clips a rectangle to the buffer; 0 when nothing is left. */
int gfx_clip_rect(const struct savanxp_fb_info* info, int* x, int* y, int* width, int* height);

#ifdef __cplusplus
}
#endif
