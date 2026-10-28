#include "sxgfx/pixels.h"

/* Text rendering uses offline-baked fonts (tools/font/genfont.py), all 1-bit:
 *   - MS Sans Serif (Wine's 13px bitmap strike) for desktop/UI text. The
 *     tables keep their historical SX_NOTO / kNoto names.
 *   - RetroSans, proportional pixel face at 17px, for window captions.
 *   - UniFont, fixed 8x16 1-bit, for the monospace terminal path.
 * PixelSans and ImpactfulBits are baked as committed tables
 * (gfx_font_pixelsans.inc, gfx_font_impactfulbits.inc) but are not linked
 * into the graphics library yet.
 */
#include "gfx_font_noto.inc"
#include "gfx_font_title.inc"
#include "console_font_unifont.inc"
#include "gfx_utf8.inc"

uint32_t gfx_rgb(uint8_t red, uint8_t green, uint8_t blue) {
    return ((uint32_t)red << 16) | ((uint32_t)green << 8) | (uint32_t)blue;
}

uint32_t gfx_stride_pixels(const struct savanxp_fb_info* info) {
    if (info == 0 || info->pitch == 0) {
        return 0;
    }
    return info->pitch / 4u;
}

size_t gfx_buffer_pixels(const struct savanxp_fb_info* info) {
    if (info == 0 || info->pitch == 0 || info->height == 0) {
        return 0;
    }
    return (size_t)gfx_stride_pixels(info) * (size_t)info->height;
}

size_t gfx_buffer_bytes(const struct savanxp_fb_info* info) {
    return gfx_buffer_pixels(info) * sizeof(uint32_t);
}

int gfx_clip_rect(const struct savanxp_fb_info* info, int* x, int* y, int* width, int* height) {
    int clipped_x;
    int clipped_y;
    int clipped_width;
    int clipped_height;

    if (info == 0 || x == 0 || y == 0 || width == 0 || height == 0 || info->pitch == 0) {
        return 0;
    }

    clipped_x = *x;
    clipped_y = *y;
    clipped_width = *width;
    clipped_height = *height;

    if (clipped_width <= 0 || clipped_height <= 0) {
        return 0;
    }

    if (clipped_x < 0) {
        clipped_width += clipped_x;
        clipped_x = 0;
    }
    if (clipped_y < 0) {
        clipped_height += clipped_y;
        clipped_y = 0;
    }
    if (clipped_width <= 0 || clipped_height <= 0) {
        return 0;
    }
    if (clipped_x >= (int)info->width || clipped_y >= (int)info->height) {
        return 0;
    }
    if (clipped_x + clipped_width > (int)info->width) {
        clipped_width = (int)info->width - clipped_x;
    }
    if (clipped_y + clipped_height > (int)info->height) {
        clipped_height = (int)info->height - clipped_y;
    }
    if (clipped_width <= 0 || clipped_height <= 0) {
        return 0;
    }

    *x = clipped_x;
    *y = clipped_y;
    *width = clipped_width;
    *height = clipped_height;
    return 1;
}

static void gfx_fill_span(uint32_t* destination, int count, uint32_t colour) {
    int index;
    for (index = 0; index < count; ++index) {
        destination[index] = colour;
    }
}

void gfx_clear(uint32_t* pixels, const struct savanxp_fb_info* info, uint32_t colour) {
    uint32_t pitch_pixels;
    uint32_t y;

    if (pixels == 0 || info == 0 || info->pitch == 0) {
        return;
    }

    pitch_pixels = info->pitch / 4;
    for (y = 0; y < info->height; ++y) {
        gfx_fill_span(pixels + ((size_t)y * pitch_pixels), (int)pitch_pixels, colour);
    }
}

void gfx_pixel(uint32_t* pixels, const struct savanxp_fb_info* info, int x, int y, uint32_t colour) {
    uint32_t pitch_pixels;
    if (pixels == 0 || info == 0 || x < 0 || y < 0 || x >= (int)info->width || y >= (int)info->height || info->pitch == 0) {
        return;
    }

    pitch_pixels = info->pitch / 4;
    pixels[(uint32_t)y * pitch_pixels + (uint32_t)x] = colour;
}

/* Alpha-composite a colour over the destination pixel. colour is 0x00RRGGBB
 * (as produced by gfx_rgb); alpha 0 = transparent, 255 = opaque. */
static void gfx_pixel_blend(uint32_t* pixels, const struct savanxp_fb_info* info, int x, int y, uint32_t colour, unsigned int alpha) {
    uint32_t pitch_pixels;
    uint32_t dst;
    unsigned int sr, sg, sb, dr, dg, db, inv;
    if (pixels == 0 || info == 0 || x < 0 || y < 0 || x >= (int)info->width || y >= (int)info->height || info->pitch == 0) {
        return;
    }
    if (alpha == 0) {
        return;
    }
    pitch_pixels = info->pitch / 4;
    if (alpha >= 255) {
        pixels[(uint32_t)y * pitch_pixels + (uint32_t)x] = colour;
        return;
    }
    dst = pixels[(uint32_t)y * pitch_pixels + (uint32_t)x];
    sr = (colour >> 16) & 0xFF; sg = (colour >> 8) & 0xFF; sb = colour & 0xFF;
    dr = (dst >> 16) & 0xFF; dg = (dst >> 8) & 0xFF; db = dst & 0xFF;
    inv = 255 - alpha;
    pixels[(uint32_t)y * pitch_pixels + (uint32_t)x] =
        (((sr * alpha + dr * inv) / 255) << 16) |
        (((sg * alpha + dg * inv) / 255) << 8) |
        ((sb * alpha + db * inv) / 255);
}

void gfx_hline(uint32_t* pixels, const struct savanxp_fb_info* info, int x, int y, int width, uint32_t colour) {
    uint32_t pitch_pixels;
    uint32_t* row;
    int height = 1;
    if (pixels == 0 || !gfx_clip_rect(info, &x, &y, &width, &height)) {
        return;
    }

    pitch_pixels = info->pitch / 4u;
    row = pixels + ((size_t)y * pitch_pixels) + (uint32_t)x;
    gfx_fill_span(row, width, colour);
}

void gfx_vline(uint32_t* pixels, const struct savanxp_fb_info* info, int x, int y, int height, uint32_t colour) {
    uint32_t pitch_pixels;
    int row;
    int width = 1;
    if (pixels == 0 || !gfx_clip_rect(info, &x, &y, &width, &height)) {
        return;
    }

    pitch_pixels = info->pitch / 4u;
    for (row = 0; row < height; ++row) {
        pixels[(size_t)(y + row) * pitch_pixels + (uint32_t)x] = colour;
    }
}

void gfx_rect(uint32_t* pixels, const struct savanxp_fb_info* info, int x, int y, int width, int height, uint32_t colour) {
    uint32_t pitch_pixels;
    int row;
    if (pixels == 0 || !gfx_clip_rect(info, &x, &y, &width, &height)) {
        return;
    }

    pitch_pixels = info->pitch / 4u;
    for (row = 0; row < height; ++row) {
        uint32_t* destination = pixels + ((size_t)(y + row) * pitch_pixels) + (uint32_t)x;
        gfx_fill_span(destination, width, colour);
    }
}

void gfx_frame(uint32_t* pixels, const struct savanxp_fb_info* info, int x, int y, int width, int height, uint32_t colour) {
    if (width <= 0 || height <= 0) {
        return;
    }
    gfx_hline(pixels, info, x, y, width, colour);
    if (height > 1) {
        gfx_hline(pixels, info, x, y + height - 1, width, colour);
    }
    if (height > 2) {
        gfx_vline(pixels, info, x, y + 1, height - 2, colour);
        if (width > 1) {
            gfx_vline(pixels, info, x + width - 1, y + 1, height - 2, colour);
        }
    }
}

/* --- Proportional UI face (default desktop/UI text) --------------------- */

int gfx_text_width(const char* text) {
    int width = 0;
    if (text == 0) {
        return 0;
    }
    while (*text != '\0') {
        width += sx_noto_glyph(sx_utf8_next(&text))->advance;
    }
    return width;
}

int gfx_text_height(void) {
    return SX_NOTO_LINE_HEIGHT;
}

/* y is the top of the text box; glyphs sit on a baseline SX_NOTO_ASCENT below.
 * has_clip restricts blended pixels to [clip_x0, clip_x1) x [clip_y0, clip_y1)
 * so a glyph that only partially overlaps the caller's clip rect does not get
 * fully re-blended outside it (matters when a layer is repainted once per
 * occlusion fragment, which would otherwise double-blend the same glyph). */
static void gfx_blit_text_impl(uint32_t* pixels, const struct savanxp_fb_info* info, int x, int y, const char* text, uint32_t colour,
                                int has_clip, int clip_x0, int clip_y0, int clip_x1, int clip_y1) {
    int pen_x = x;
    int baseline = y + SX_NOTO_ASCENT;
    while (text != 0 && *text != '\0') {
        const struct sx_noto_glyph* g = sx_noto_glyph(sx_utf8_next(&text));
        int row;
        int column;
        for (row = 0; row < g->rows; ++row) {
            int py = baseline - g->top + row;
            if (has_clip && (py < clip_y0 || py >= clip_y1)) {
                continue;
            }
            for (column = 0; column < g->width; ++column) {
                int px = pen_x + g->left + column;
                if (has_clip && (px < clip_x0 || px >= clip_x1)) {
                    continue;
                }
                unsigned int alpha = kNotoCoverage[g->offset + (unsigned int)row * g->width + column];
                if (alpha != 0) {
                    gfx_pixel_blend(pixels, info, px, py, colour, alpha);
                }
            }
        }
        pen_x += g->advance;
    }
}

void gfx_blit_text(uint32_t* pixels, const struct savanxp_fb_info* info, int x, int y, const char* text, uint32_t colour) {
    gfx_blit_text_impl(pixels, info, x, y, text, colour, 0, 0, 0, 0, 0);
}

void gfx_blit_text_clip(uint32_t* pixels, const struct savanxp_fb_info* info, int x, int y, const char* text, uint32_t colour,
                         int clip_x0, int clip_y0, int clip_x1, int clip_y1) {
    gfx_blit_text_impl(pixels, info, x, y, text, colour, 1, clip_x0, clip_y0, clip_x1, clip_y1);
}

/* --- 18px RetroSans caption variant ------------------------------------- */

int gfx_text_width_title(const char* text) {
    int pen_x = 0;
    int right = 0;
    if (text == 0) {
        return 0;
    }
    while (*text != '\0') {
        const struct sx_title_glyph* g = sx_title_glyph(sx_utf8_next(&text));
        int glyph_right = pen_x + g->left + g->width;
        if (glyph_right > right) {
            right = glyph_right;
        }
        pen_x += g->advance;
    }
    /* The pixel atlas can extend a final stem past its advance. Keep that
     * pixel in the painter's clip box instead of shaving it off at the edge. */
    return right > pen_x ? right : pen_x;
}

int gfx_text_height_title(void) {
    return SX_TITLE_LINE_HEIGHT;
}

/* The title face has the same baseline contract as the regular UI face, just
 * with a smaller pixel coverage atlas. Keeping the clipped path here
 * means the painter's region/clip behavior is identical for every font. */
static void gfx_blit_text_title_impl(uint32_t* pixels, const struct savanxp_fb_info* info, int x, int y, const char* text, uint32_t colour,
                                     int has_clip, int clip_x0, int clip_y0, int clip_x1, int clip_y1) {
    int pen_x = x;
    int baseline = y + SX_TITLE_ASCENT;
    while (text != 0 && *text != '\0') {
        const struct sx_title_glyph* g = sx_title_glyph(sx_utf8_next(&text));
        int row;
        int column;
        for (row = 0; row < g->rows; ++row) {
            int py = baseline - g->top + row;
            if (has_clip && (py < clip_y0 || py >= clip_y1)) {
                continue;
            }
            for (column = 0; column < g->width; ++column) {
                int px = pen_x + g->left + column;
                if (has_clip && (px < clip_x0 || px >= clip_x1)) {
                    continue;
                }
                unsigned int alpha = kTitleCoverage[g->offset + (unsigned int)row * g->width + column];
                if (alpha != 0) {
                    gfx_pixel_blend(pixels, info, px, py, colour, alpha);
                }
            }
        }
        pen_x += g->advance;
    }
}

void gfx_blit_text_title(uint32_t* pixels, const struct savanxp_fb_info* info, int x, int y, const char* text, uint32_t colour) {
    gfx_blit_text_title_impl(pixels, info, x, y, text, colour, 0, 0, 0, 0, 0);
}

void gfx_blit_text_title_clip(uint32_t* pixels, const struct savanxp_fb_info* info, int x, int y, const char* text, uint32_t colour,
                              int clip_x0, int clip_y0, int clip_x1, int clip_y1) {
    gfx_blit_text_title_impl(pixels, info, x, y, text, colour, 1, clip_x0, clip_y0, clip_x1, clip_y1);
}

/* --- Fixed 8x16 UniFont (monospace terminal text) ----------------------- */

int gfx_cell_width(void) {
    return SX_CONSOLE_GLYPH_W;
}

int gfx_cell_height(void) {
    return SX_CONSOLE_GLYPH_H;
}

int gfx_text_width_mono(const char* text) {
    int count = 0;
    if (text == 0) {
        return 0;
    }
    /* Cells, not bytes: in UTF-8 an accented character takes two bytes but a
     * single cell of the monospace grid. */
    while (*text != '\0') {
        (void)sx_utf8_next(&text);
        ++count;
    }
    return count * SX_CONSOLE_GLYPH_W;
}

/* The effective clip is the caller's clip intersected with the surface:
 * computed once, so the inner loop validates nothing again. The previous
 * version called gfx_pixel() for every lit pixel, and gfx_pixel rechecks the
 * four edges and recomputes info->pitch / 4 -- a division -- on each one. An
 * 80x24 grid is ~245k iterations per repaint, so that per-pixel division was
 * the dominant cost of drawing monospace text. */
static void gfx_blit_text_mono_impl(uint32_t* pixels, const struct savanxp_fb_info* info, int x, int y, const char* text, uint32_t colour,
                                     int has_clip, int clip_x0, int clip_y0, int clip_x1, int clip_y1) {
    int clip_left = 0;
    int clip_top = 0;
    int clip_right;
    int clip_bottom;
    uint32_t pitch_pixels;
    int pen_x = x;

    if (pixels == 0 || info == 0 || info->pitch == 0) {
        return;
    }

    clip_right = (int)info->width;
    clip_bottom = (int)info->height;
    if (has_clip) {
        if (clip_x0 > clip_left) {
            clip_left = clip_x0;
        }
        if (clip_y0 > clip_top) {
            clip_top = clip_y0;
        }
        if (clip_x1 < clip_right) {
            clip_right = clip_x1;
        }
        if (clip_y1 < clip_bottom) {
            clip_bottom = clip_y1;
        }
    }
    if (clip_left >= clip_right || clip_top >= clip_bottom) {
        return;
    }

    pitch_pixels = info->pitch / 4;

    while (text != 0 && *text != '\0') {
        const unsigned char* glyph = sx_console_glyph(sx_utf8_next(&text));
        int column_begin = clip_left - pen_x;
        int column_end = clip_right - pen_x;
        int row;

        if (column_begin < 0) {
            column_begin = 0;
        }
        if (column_end > SX_CONSOLE_GLYPH_W) {
            column_end = SX_CONSOLE_GLYPH_W;
        }
        /* The whole cell fell outside the clip: the pen still advances, because
         * the grid has to stay aligned with the text that is drawn. */
        if (column_begin >= column_end) {
            pen_x += SX_CONSOLE_GLYPH_W;
            continue;
        }

        for (row = 0; row < SX_CONSOLE_GLYPH_H; ++row) {
            const int py = y + row;
            const unsigned int bits = glyph[row];
            uint32_t* row_pixels;
            int column;

            if (bits == 0 || py < clip_top || py >= clip_bottom) {
                continue;
            }

            row_pixels = pixels + ((size_t)py * pitch_pixels);
            for (column = column_begin; column < column_end; ++column) {
                if ((bits & (0x80u >> column)) != 0) {
                    row_pixels[pen_x + column] = colour;
                }
            }
        }
        pen_x += SX_CONSOLE_GLYPH_W;
    }
}

void gfx_blit_text_mono(uint32_t* pixels, const struct savanxp_fb_info* info, int x, int y, const char* text, uint32_t colour) {
    gfx_blit_text_mono_impl(pixels, info, x, y, text, colour, 0, 0, 0, 0, 0);
}

void gfx_blit_text_mono_clip(uint32_t* pixels, const struct savanxp_fb_info* info, int x, int y, const char* text, uint32_t colour,
                              int clip_x0, int clip_y0, int clip_x1, int clip_y1) {
    gfx_blit_text_mono_impl(pixels, info, x, y, text, colour, 1, clip_x0, clip_y0, clip_x1, clip_y1);
}
