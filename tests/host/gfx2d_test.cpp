/*
 * gfx2d_test.cpp -- Test de host del painter de SxGFX
 * (subsystems/posix/sdk/v1/runtime/gfx2d.c), corriendo el painter REAL contra
 * un bitmap en memoria y comparando pixel por pixel.
 *
 * Por que en el host y no en un smoke de QEMU: lo que hay que verificar son
 * pixeles concretos en posiciones concretas (que el origen se aplique UNA sola
 * vez, que la trama quede anclada al dispositivo, que el marco no deje residuo
 * al pintarse por fragmentos). Un smoke arriba de QEMU solo puede mirar que la
 * pantalla no explote; aca se afirma el pixel exacto.
 *
 * Las primitivas crudas de gfx_impl.inc (gfx_rect y el texto) estan stubbeadas
 * en este mismo TU: lo que esta bajo prueba es el painter -- clipping, origen,
 * brushes -- no el rasterizado de glifos.
 */

#define _CRT_SECURE_NO_WARNINGS 1

#include <stdint.h>

/* El include del SDK trae su propio <stdio.h>/<string.h> (los de userland, sin
 * guarda de C++) y tapan a los del host, que es de donde este test se linkea.
 * Envolverlos en extern "C" les devuelve el linkage correcto; sin esto el
 * enlazador pide un printf/strlen con nombre mangleado que no existe. */
extern "C" {
#include <stdio.h>
#include <string.h>
}

#include "savanxp/gfx2d.h"

namespace {

int g_failures = 0;
int g_checks = 0;

bool check(bool ok, const char* what) {
    ++g_checks;
    if (!ok) {
        ++g_failures;
        printf("  FAIL %s\n", what);
    } else {
        printf("  ok   %s\n", what);
    }
    return ok;
}

constexpr int kWidth = 32;
constexpr int kHeight = 24;

struct Canvas {
    uint32_t pixels[kWidth * kHeight];
    savanxp_fb_info info;
    sx_bitmap bitmap;
    sx_painter painter;

    Canvas() {
        memset(pixels, 0, sizeof(pixels));
        memset(&info, 0, sizeof(info));
        info.width = kWidth;
        info.height = kHeight;
        info.pitch = kWidth * sizeof(uint32_t);
        info.bpp = 32u;
        info.buffer_size = info.pitch * info.height;
        sx_bitmap_wrap(&bitmap, pixels, &info, SX_PIXEL_FORMAT_BGRX8888);
        sx_painter_init(&painter, &bitmap);
    }

    uint32_t at(int x, int y) const {
        if (x < 0 || y < 0 || x >= kWidth || y >= kHeight) {
            return 0xdeadbeefu;
        }
        return pixels[y * kWidth + x];
    }

    // Cuantos pixeles del canvas tienen ese color. Sirve para afirmar que una
    // primitiva no pinto NADA de mas, que es donde estaba el bug de residuos.
    int count(uint32_t colour) const {
        int total = 0;
        for (int i = 0; i < kWidth * kHeight; ++i) {
            if (pixels[i] == colour) {
                ++total;
            }
        }
        return total;
    }
};

const uint32_t kInk = 0x00112233u;
const uint32_t kBack = 0x00445566u;

/* ---- origen ------------------------------------------------------------- */

void case_origin_shifts_drawing() {
    printf("caso: el origen corre el dibujo\n");
    Canvas c;

    sx_painter_push_origin(&c.painter, 5, 3);
    sx_painter_fill_rect(&c.painter, sx_rect_make(0, 0, 2, 2), kInk);
    sx_painter_pop_origin(&c.painter);

    check(c.at(5, 3) == kInk && c.at(6, 4) == kInk, "el rect cae en el origen");
    check(c.at(0, 0) != kInk, "no queda nada en el origen viejo");
    check(c.count(kInk) == 4, "pinto exactamente 4 pixeles");
}

void case_origin_applies_once() {
    printf("caso: el origen se aplica UNA sola vez\n");
    Canvas c;

    // draw_frame delega en el relleno de rects: si ambos tradujeran, el marco
    // saldria al doble del desplazamiento.
    sx_painter_push_origin(&c.painter, 4, 4);
    sx_painter_draw_frame(&c.painter, sx_rect_make(0, 0, 3, 3), kInk);
    sx_painter_pop_origin(&c.painter);

    check(c.at(4, 4) == kInk, "la esquina del marco esta en +4, no en +8");
    check(c.at(8, 8) != kInk, "no hay doble traduccion");
    check(c.count(kInk) == 8, "marco de 3x3 = 8 pixeles de borde");
}

void case_origin_nests_and_restores() {
    printf("caso: la pila de origenes anida y restaura\n");
    Canvas c;

    sx_painter_push_origin(&c.painter, 2, 2);
    sx_painter_push_origin(&c.painter, 3, 1);
    sx_painter_set_pixel(&c.painter, 0, 0, kInk);
    sx_painter_pop_origin(&c.painter);
    sx_painter_set_pixel(&c.painter, 0, 0, kBack);
    sx_painter_pop_origin(&c.painter);

    check(c.at(5, 3) == kInk, "el origen anidado suma (2+3, 2+1)");
    check(c.at(2, 2) == kBack, "el pop restaura el origen de afuera");
    sx_point origin = sx_painter_origin(&c.painter);
    check(origin.x == 0 && origin.y == 0, "vuelve a (0,0)");
}

void case_origin_stack_overflow_is_safe() {
    printf("caso: desbordar la pila de origenes no mueve nada\n");
    Canvas c;
    int pushed = 0;

    for (int i = 0; i < SX_PAINTER_ORIGIN_STACK_DEPTH + 4; ++i) {
        if (sx_painter_push_origin(&c.painter, 1, 0)) {
            ++pushed;
        }
    }
    check(pushed == SX_PAINTER_ORIGIN_STACK_DEPTH, "acepta exactamente la profundidad declarada");
    sx_point origin = sx_painter_origin(&c.painter);
    check(origin.x == SX_PAINTER_ORIGIN_STACK_DEPTH,
          "los push rechazados no corrieron el origen");
}

void case_clip_is_relative_to_origin() {
    printf("caso: el clip se expresa en coordenadas locales\n");
    Canvas c;

    sx_painter_push_origin(&c.painter, 10, 10);
    sx_painter_push_clip(&c.painter, sx_rect_make(0, 0, 2, 2));
    // Un rect grande en locales: el clip lo debe recortar a 2x2 en +10.
    sx_painter_fill_rect(&c.painter, sx_rect_make(0, 0, 8, 8), kInk);
    sx_painter_pop_clip(&c.painter);
    sx_painter_pop_origin(&c.painter);

    check(c.count(kInk) == 4, "el clip local recorto a 2x2");
    check(c.at(10, 10) == kInk && c.at(11, 11) == kInk, "y quedo en el origen");
    check(c.at(12, 12) != kInk, "nada fuera del clip");
}

void case_clip_bounds_round_trip() {
    printf("caso: clip_bounds devuelve locales que se pueden re-dibujar\n");
    Canvas c;

    sx_painter_push_origin(&c.painter, 6, 4);
    sx_painter_push_clip(&c.painter, sx_rect_make(1, 1, 3, 2));
    sx_rect bounds = sx_painter_clip_bounds(&c.painter);
    check(bounds.x == 1 && bounds.y == 1 && bounds.width == 3 && bounds.height == 2,
          "informa el clip en las mismas coordenadas que acepta");
    // Redibujar lo que informa tiene que cubrir el clip entero, ni mas ni menos.
    sx_painter_fill_rect(&c.painter, bounds, kInk);
    sx_painter_pop_clip(&c.painter);
    sx_painter_pop_origin(&c.painter);

    check(c.count(kInk) == 6, "el round-trip cubre exactamente el clip");
    check(c.at(7, 5) == kInk, "y en la posicion de dispositivo correcta");
}

void case_clip_bounds_without_clip_is_target() {
    printf("caso: sin clip, clip_bounds es el target en locales\n");
    Canvas c;

    sx_painter_push_origin(&c.painter, 3, 2);
    sx_rect bounds = sx_painter_clip_bounds(&c.painter);
    sx_painter_pop_origin(&c.painter);

    check(bounds.x == -3 && bounds.y == -2, "el target arranca en -origen");
    check(bounds.width == kWidth && bounds.height == kHeight, "y mide el target entero");
}

void case_fill_clears_whole_target() {
    printf("caso: fill() limpia el target entero pese al origen\n");
    Canvas c;

    sx_painter_push_origin(&c.painter, 7, 7);
    sx_painter_fill(&c.painter, kInk);
    sx_painter_pop_origin(&c.painter);

    check(c.count(kInk) == kWidth * kHeight, "fill es un clear, no respeta el origen");
}

/* ---- primitivas --------------------------------------------------------- */

void case_primitives_paint_expected_pixels() {
    printf("caso: pixel/hline/vline pintan lo justo\n");
    Canvas c;

    sx_painter_set_pixel(&c.painter, 1, 1, kInk);
    check(c.count(kInk) == 1 && c.at(1, 1) == kInk, "set_pixel pinta un solo pixel");

    Canvas h;
    sx_painter_hline(&h.painter, 2, 5, 4, kInk);
    check(h.count(kInk) == 4, "hline pinta width pixeles");
    check(h.at(2, 5) == kInk && h.at(5, 5) == kInk && h.at(6, 5) != kInk, "hline en la fila correcta");

    Canvas v;
    sx_painter_vline(&v.painter, 3, 2, 5, kInk);
    check(v.count(kInk) == 5, "vline pinta height pixeles");
    check(v.at(3, 2) == kInk && v.at(3, 6) == kInk && v.at(3, 7) != kInk, "vline en la columna correcta");
}

void case_primitives_respect_clip() {
    printf("caso: las primitivas nuevas respetan el clip\n");
    Canvas c;

    sx_painter_push_clip(&c.painter, sx_rect_make(4, 0, 3, kHeight));
    sx_painter_hline(&c.painter, 0, 5, kWidth, kInk);
    sx_painter_set_pixel(&c.painter, 0, 0, kInk);
    sx_painter_pop_clip(&c.painter);

    check(c.count(kInk) == 3, "solo sobrevive el tramo dentro del clip");
    check(c.at(4, 5) == kInk && c.at(3, 5) != kInk && c.at(7, 5) != kInk, "recortada a [4,7)");
}

void case_primitives_clip_to_bitmap() {
    printf("caso: las primitivas se recortan al bitmap\n");
    Canvas c;

    sx_painter_hline(&c.painter, -5, 0, kWidth + 20, kInk);
    check(c.count(kInk) == kWidth, "hline desbordada se recorta al ancho");

    Canvas d;
    sx_painter_set_pixel(&d.painter, -1, -1, kInk);
    sx_painter_set_pixel(&d.painter, kWidth, kHeight, kInk);
    check(d.count(kInk) == 0, "pixeles fuera del bitmap no escriben nada");
}

/* ---- marco -------------------------------------------------------------- */

void case_frame_has_no_fragment_residue() {
    printf("caso: el marco por fragmentos no deja residuo\n");
    Canvas whole;
    sx_painter_draw_frame(&whole.painter, sx_rect_make(2, 2, 10, 8), kInk);
    const int whole_count = whole.count(kInk);

    // El mismo marco, pintado en cuatro fragmentos de clip como hace el compose
    // parcial. Tiene que dar EXACTAMENTE los mismos pixeles: si draw_frame
    // enmarcara el rect ya clipeado, cada fragmento agregaria bordes espurios.
    Canvas pieces;
    const sx_rect fragments[4] = {
        sx_rect_make(0, 0, 7, 6),
        sx_rect_make(7, 0, 25, 6),
        sx_rect_make(0, 6, 7, 18),
        sx_rect_make(7, 6, 25, 18),
    };
    for (int i = 0; i < 4; ++i) {
        sx_painter_push_clip(&pieces.painter, fragments[i]);
        sx_painter_draw_frame(&pieces.painter, sx_rect_make(2, 2, 10, 8), kInk);
        sx_painter_pop_clip(&pieces.painter);
    }

    check(pieces.count(kInk) == whole_count, "misma cantidad de pixeles que de una");
    bool identical = true;
    for (int y = 0; y < kHeight && identical; ++y) {
        for (int x = 0; x < kWidth; ++x) {
            if (whole.at(x, y) != pieces.at(x, y)) {
                identical = false;
                break;
            }
        }
    }
    check(identical, "el canvas fragmentado es identico al de una sola pasada");
}

/* ---- brushes ------------------------------------------------------------ */

void case_pattern_is_anchored_to_device() {
    printf("caso: la trama se ancla al dispositivo, no al rect\n");
    Canvas c;
    sx_brush brush = sx_brush_pattern(sx_pattern_checker_50, kInk, kBack);

    // Dos rects vecinos que arrancan en paridades distintas: los puntos tienen
    // que seguir la MISMA grilla, que es lo que pedia el codigo a mano de sxgui.
    sx_painter_fill_rect_brush(&c.painter, sx_rect_make(0, 0, 4, 1), &brush);
    sx_painter_fill_rect_brush(&c.painter, sx_rect_make(5, 0, 4, 1), &brush);

    bool grid_ok = true;
    for (int x = 0; x < 9; ++x) {
        if (x == 4) {
            continue;
        }
        const uint32_t expected = ((x + 0) % 2 == 0) ? kInk : kBack;
        if (c.at(x, 0) != expected) {
            grid_ok = false;
        }
    }
    check(grid_ok, "ambos rects caen en la grilla (x+y) par");
}

void case_pattern_alternates_by_row() {
    printf("caso: la trama alterna por fila\n");
    Canvas c;
    sx_brush brush = sx_brush_pattern(sx_pattern_checker_50, kInk, kBack);

    sx_painter_fill_rect_brush(&c.painter, sx_rect_make(0, 0, 4, 4), &brush);

    bool ok = true;
    for (int y = 0; y < 4; ++y) {
        for (int x = 0; x < 4; ++x) {
            const uint32_t expected = ((x + y) % 2 == 0) ? kInk : kBack;
            if (c.at(x, y) != expected) {
                ok = false;
            }
        }
    }
    check(ok, "el damero es (x+y) par en todo el rect");
    check(c.count(kInk) == 8, "mitad de 4x4 en tinta");
}

void case_transparent_pattern_keeps_background() {
    printf("caso: la trama transparente deja pasar el fondo\n");
    Canvas c;
    sx_brush opaque = sx_brush_solid(kBack);
    sx_brush dotted = sx_brush_pattern_transparent(sx_pattern_checker_50, kInk);

    sx_painter_fill_rect_brush(&c.painter, sx_rect_make(0, 0, 4, 2), &opaque);
    sx_painter_fill_rect_brush(&c.painter, sx_rect_make(0, 0, 4, 2), &dotted);

    check(c.count(kInk) == 4, "solo la mitad marcada se pinta");
    check(c.count(kBack) == 4, "la otra mitad conserva el fondo previo");
}

void case_solid_brush_matches_plain_fill() {
    printf("caso: el brush solido es identico al relleno plano\n");
    Canvas plain;
    sx_painter_fill_rect(&plain.painter, sx_rect_make(3, 3, 6, 4), kInk);

    Canvas brushed;
    sx_brush brush = sx_brush_solid(kInk);
    sx_painter_fill_rect_brush(&brushed.painter, sx_rect_make(3, 3, 6, 4), &brush);

    check(memcmp(plain.pixels, brushed.pixels, sizeof(plain.pixels)) == 0,
          "mismo resultado por el camino de brush y por el plano");
}

void case_brush_frame_is_dotted_border_only() {
    printf("caso: el marco con brush puntea solo el borde\n");
    Canvas c;
    sx_brush brush = sx_brush_pattern_transparent(sx_pattern_checker_50, kInk);

    sx_painter_draw_frame_brush(&c.painter, sx_rect_make(0, 0, 8, 6), &brush);

    check(c.at(4, 3) != kInk, "el interior queda vacio");
    // Borde de 8x6 = 2*8 + 2*(6-2) = 24 celdas, la mitad marcada por la trama.
    check(c.count(kInk) == 12, "puntea la mitad de las 24 celdas del borde");
    check(c.at(0, 0) == kInk && c.at(1, 0) != kInk, "arranca la grilla en (0,0)");
}

/* ---- muestreo ----------------------------------------------------------- */

void case_scaled_blit_clamps_source_rect() {
    printf("caso: el blit escalado acota el source_rect al bitmap origen\n");
    // El origen es una franja de 4x4 rodeada de memoria que el test vigila: si
    // el muestreo se sale del bitmap, lee de kGuard y se nota en el destino.
    static uint32_t guard_before[64];
    static uint32_t source_pixels[16];
    static uint32_t guard_after[64];
    for (int i = 0; i < 64; ++i) {
        guard_before[i] = 0x00ff00ffu;
        guard_after[i] = 0x00ff00ffu;
    }
    for (int i = 0; i < 16; ++i) {
        source_pixels[i] = kInk;
    }

    savanxp_fb_info source_info;
    memset(&source_info, 0, sizeof(source_info));
    source_info.width = 4;
    source_info.height = 4;
    source_info.pitch = 4 * sizeof(uint32_t);
    source_info.bpp = 32u;
    source_info.buffer_size = source_info.pitch * source_info.height;

    sx_bitmap source;
    sx_bitmap_wrap(&source, source_pixels, &source_info, SX_PIXEL_FORMAT_BGRX8888);

    Canvas c;
    // source_rect el cuadruple del bitmap real: sin clamp, el bucle muestrea
    // fuera del buffer.
    sx_painter_draw_scaled_bitmap_nearest(
        &c.painter, &source, sx_rect_make(0, 0, 8, 8), sx_rect_make(0, 0, 16, 16));

    check(c.count(0x00ff00ffu) == 0, "no se colo ningun pixel de guarda al destino");
    check(c.count(kInk) > 0, "y aun asi dibujo algo del origen");

    bool guards_intact = true;
    for (int i = 0; i < 64; ++i) {
        if (guard_before[i] != 0x00ff00ffu || guard_after[i] != 0x00ff00ffu) {
            guards_intact = false;
        }
    }
    check(guards_intact, "las guardas quedaron intactas");
}

} // namespace

/* ---- stubs de las primitivas crudas ------------------------------------- */

extern "C" {

uint32_t gfx_stride_pixels(const struct savanxp_fb_info* info) {
    return info->pitch / (uint32_t)sizeof(uint32_t);
}

void gfx_rect(uint32_t* pixels, const struct savanxp_fb_info* info, int x, int y, int width, int height, uint32_t colour) {
    const uint32_t stride = gfx_stride_pixels(info);
    for (int row = 0; row < height; ++row) {
        for (int column = 0; column < width; ++column) {
            pixels[(size_t)(y + row) * stride + (size_t)(x + column)] = colour;
        }
    }
}

/* El painter solo consulta metricas y delega el rasterizado: para este test
 * alcanza con una caja de 1x1 por caracter. */
int gfx_text_width(const char* text) {
    return text == nullptr ? 0 : (int)strlen(text);
}

int gfx_text_height(void) {
    return 1;
}

void gfx_blit_text(uint32_t* pixels, const struct savanxp_fb_info* info, int x, int y, const char* text, uint32_t colour) {
    if (text == nullptr) {
        return;
    }
    gfx_rect(pixels, info, x, y, (int)strlen(text), 1, colour);
}

void gfx_blit_text_clip(uint32_t* pixels, const struct savanxp_fb_info* info, int x, int y, const char* text, uint32_t colour,
                        int clip_x0, int clip_y0, int clip_x1, int clip_y1) {
    if (text == nullptr) {
        return;
    }
    const uint32_t stride = gfx_stride_pixels(info);
    const int length = (int)strlen(text);
    for (int column = 0; column < length; ++column) {
        const int px = x + column;
        if (px < clip_x0 || px >= clip_x1 || y < clip_y0 || y >= clip_y1) {
            continue;
        }
        pixels[(size_t)y * stride + (size_t)px] = colour;
    }
}

} // extern "C"

int main() {
    printf("GFX2D TEST START\n");

    case_origin_shifts_drawing();
    case_origin_applies_once();
    case_origin_nests_and_restores();
    case_origin_stack_overflow_is_safe();
    case_clip_is_relative_to_origin();
    case_clip_bounds_round_trip();
    case_clip_bounds_without_clip_is_target();
    case_fill_clears_whole_target();

    case_primitives_paint_expected_pixels();
    case_primitives_respect_clip();
    case_primitives_clip_to_bitmap();

    case_frame_has_no_fragment_residue();

    case_pattern_is_anchored_to_device();
    case_pattern_alternates_by_row();
    case_transparent_pattern_keeps_background();
    case_solid_brush_matches_plain_fill();
    case_brush_frame_is_dotted_border_only();

    case_scaled_blit_clamps_source_rect();

    printf("%s (%d checks, %d fallas)\n",
           g_failures == 0 ? "GFX2D TEST PASS" : "GFX2D TEST FAIL",
           g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
