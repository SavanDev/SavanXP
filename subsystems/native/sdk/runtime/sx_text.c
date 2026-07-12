/*
 * SavanXP - render de texto para el subsistema nativo (Fase 3, port del
 * escritorio a Haxe).
 *
 * Reutiliza la fuente Noto horneada del SDK posix (fuente de verdad:
 * tools/font/genfont.py) via include relativo, para no duplicar los ~78 KB de
 * datos ni que las dos copias se desincronicen. Blit de glifos antialiased
 * (alpha coverage) sobre un buffer de pixeles XRGB contiguo con stride dado --
 * el mismo layout que usa la clase Painter de Haxe (Array<Int>). Sin syscalls:
 * es puro pixel-pushing, encaja en el runtime nativo.
 */
#include "savanxp_native.h"

/* Datos de la fuente: struct sx_noto_glyph, kNotoGlyphs[], kNotoCoverage[] y
 * los #define SX_NOTO_* (FIRST/LAST/ASCENT/LINE_HEIGHT). */
#include "../../../posix/sdk/v1/runtime/gfx_font_noto.inc"

static const struct sx_noto_glyph* sxn_glyph(unsigned char c) {
    if (c < SX_NOTO_FIRST || c > SX_NOTO_LAST) {
        return &kNotoGlyphs[0]; /* espacio: avance seguro para caracteres fuera de rango */
    }
    return &kNotoGlyphs[c - SX_NOTO_FIRST];
}

int sxn_text_width(const char* text) {
    int width = 0;
    if (text == 0) {
        return 0;
    }
    while (*text != '\0') {
        width += sxn_glyph((unsigned char)*text)->advance;
        ++text;
    }
    return width;
}

int sxn_text_height(void) {
    return SX_NOTO_LINE_HEIGHT;
}

/* Compositing alpha entero: color sobre el pixel destino con cobertura 0..255. */
static void sxn_blend(unsigned int* pixel, unsigned int color, unsigned int alpha) {
    unsigned int dst, sr, sg, sb, dr, dg, db, inv;
    if (alpha == 0) {
        return;
    }
    if (alpha >= 255) {
        *pixel = color;
        return;
    }
    dst = *pixel;
    sr = (color >> 16) & 0xffu;
    sg = (color >> 8) & 0xffu;
    sb = color & 0xffu;
    dr = (dst >> 16) & 0xffu;
    dg = (dst >> 8) & 0xffu;
    db = dst & 0xffu;
    inv = 255u - alpha;
    *pixel = (((sr * alpha + dr * inv) / 255u) << 16)
           | (((sg * alpha + dg * inv) / 255u) << 8)
           | ((sb * alpha + db * inv) / 255u);
}

/* Dibuja `text` con esquina superior-izquierda en (x, y) sobre `pixels`
 * (buffer XRGB contiguo, `stride` pixeles por fila, recortado a width x height).
 * Los glifos se apoyan en una baseline SX_NOTO_ASCENT debajo de y. */
void sxn_text_draw(unsigned int* pixels, int stride, int width, int height,
                   int x, int y, const char* text, unsigned int color) {
    int pen_x = x;
    int baseline = y + SX_NOTO_ASCENT;
    if (pixels == 0 || text == 0) {
        return;
    }
    while (*text != '\0') {
        const struct sx_noto_glyph* g = sxn_glyph((unsigned char)*text);
        int row;
        for (row = 0; row < g->rows; ++row) {
            int py = baseline - g->top + row;
            int column;
            if (py < 0 || py >= height) {
                continue;
            }
            for (column = 0; column < g->width; ++column) {
                int px = pen_x + g->left + column;
                unsigned int alpha;
                if (px < 0 || px >= width) {
                    continue;
                }
                alpha = kNotoCoverage[g->offset + (unsigned int)row * (unsigned int)g->width + (unsigned int)column];
                if (alpha != 0) {
                    sxn_blend(&pixels[(unsigned int)py * (unsigned int)stride + (unsigned int)px], color, alpha);
                }
            }
        }
        pen_x += g->advance;
        ++text;
    }
}
