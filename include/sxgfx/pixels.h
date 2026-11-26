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

/* Faces at run time (docs/desktop/UI_FONTS.md, "Faces at run time"). Each of
 * the three faces above is a slot. A slot draws with its baked table until a
 * program installs another face in it, for instance a TrueType face from
 * libsxfont (sx_font_gfx_face); installing NULL brings the baked face back.
 * The slots are the process's: install before drawing, from the thread that
 * draws, and keep what the face's context points at alive while it is
 * installed. The values match the painter's sx_font_id. */
#define GFX_FACE_UI 0
#define GFX_FACE_MONO 1
#define GFX_FACE_TITLE 2
#define GFX_FACE_SLOTS 3

/* One glyph as a face hands it over: coverage rows*pitch bytes, 0..255 per
 * pixel, from the top row; `top` is how far that row is above the baseline.
 * It stays valid until the face's next call. */
struct gfx_glyph {
    int width;
    int rows;
    int left;
    int top;
    int advance;
    int pitch;
    const unsigned char* coverage;
};

struct gfx_face {
    int ascent;                 /* the baseline, below the top of the line */
    int line_height;            /* what gfx_text_height and the others answer */
    int cell_width;             /* GFX_FACE_MONO only: the grid's advance */
    /* Fills `out` for a codepoint; a codepoint the face lacks gets whatever
     * it draws for one (Windows' box, the baked tables' blank). */
    void (*glyph)(void* context, unsigned int codepoint, struct gfx_glyph* out);
    /* The advance alone, for measuring; NULL to measure with glyph(). */
    int (*advance)(void* context, unsigned int codepoint);
    void* context;
};

/* 0, or -1 for a slot out of range or a face without glyph() or with a
 * line height below 1. */
int gfx_face_install(int slot, const struct gfx_face* face);
/* 1 when the slot draws with an installed face, 0 with its baked table. */
int gfx_face_installed(int slot);

#ifdef __cplusplus
}
#endif
