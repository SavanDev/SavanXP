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
/* El `stdout` del <stdio.h> del SDK no es el del CRT del host, asi que
 * setvbuf(stdout, ...) ni siquiera linkea. fflush(NULL) vacia todos los streams
 * sin nombrar ninguno: alcanza para que un crash no se lleve la salida ya
 * impresa, que es como se ubica el caso que rompio. */

#include "savanxp/gfx2d.h"

/* El decodificador real, el mismo .inc que compilan el SDK posix y el runtime
 * nativo. No hay copia en el test: si se rompe aca, se rompio en los dos. */
#include "../../subsystems/posix/sdk/v1/runtime/gfx_utf8.inc"

/* Y la tabla de glifos horneada, para poder afirmar QUE codepoints existen de
 * verdad. Es data + una busqueda inline; no arrastra syscalls. El painter que se
 * prueba mas abajo usa stubs, asi que no hay colision. */
#include "../../subsystems/posix/sdk/v1/runtime/gfx_font_noto.inc"

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
    fflush(0);
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
// Los stubs de blit los incrementan: asi el test ve QUE camino se tomo, no solo
// que pixeles quedaron.
int g_mono_blits = 0;
int g_ui_blits = 0;
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

/* ---- regiones ----------------------------------------------------------- */

/* Modelo de referencia: la misma secuencia de operaciones sobre una grilla de
 * booleanos. Es el juez -- si la region y la grilla discrepan en un solo pixel,
 * la region esta mal. Compararlas es mucho mas fuerte que afirmar conteos de
 * bandas, que dependen de la representacion. */
constexpr int kGridW = 40;
constexpr int kGridH = 30;

struct Grid {
    bool cell[kGridH][kGridW] = {};

    void op_rect(sx_rect r, int op) { // 0=union 1=subtract 2=intersect
        for (int y = 0; y < kGridH; ++y) {
            for (int x = 0; x < kGridW; ++x) {
                const bool in = x >= r.x && x < r.x + r.width && y >= r.y && y < r.y + r.height;
                if (op == 0) {
                    cell[y][x] = cell[y][x] || in;
                } else if (op == 1) {
                    cell[y][x] = cell[y][x] && !in;
                } else {
                    cell[y][x] = cell[y][x] && in;
                }
            }
        }
    }
    int area() const {
        int n = 0;
        for (int y = 0; y < kGridH; ++y)
            for (int x = 0; x < kGridW; ++x)
                if (cell[y][x]) ++n;
        return n;
    }
};

// Compara la region contra la grilla pixel por pixel. `superset_ok` acepta que
// la region cubra de mas (es lo que promete el desborde), nunca de menos.
bool region_matches(const sx_region& r, const Grid& g, bool superset_ok = false) {
    for (int y = 0; y < kGridH; ++y) {
        for (int x = 0; x < kGridW; ++x) {
            const bool in_region = sx_region_contains_point(&r, x, y) != 0;
            if (in_region == g.cell[y][x]) {
                continue;
            }
            if (superset_ok && in_region && !g.cell[y][x]) {
                continue;
            }
            printf("    discrepancia en (%d,%d): region=%d grilla=%d\n",
                   x, y, (int)in_region, (int)g.cell[y][x]);
            return false;
        }
    }
    return true;
}

// Verifica el invariante canonico: bandas ordenadas y disjuntas, tramos
// ordenados, disjuntos y no adyacentes, y sin bandas contiguas identicas.
bool region_is_canonical(const sx_region& r) {
    for (int b = 0; b < r.band_count; ++b) {
        const sx_region_band& band = r.bands[b];
        if (band.y0 >= band.y1 || band.span_count <= 0) {
            printf("    banda %d vacia o invertida\n", b);
            return false;
        }
        for (int s = 0; s < band.span_count; ++s) {
            if (band.spans[s].x0 >= band.spans[s].x1) {
                printf("    tramo %d de la banda %d vacio\n", s, b);
                return false;
            }
            if (s > 0 && band.spans[s].x0 <= band.spans[s - 1].x1) {
                printf("    tramos %d/%d de la banda %d se tocan o desordenados\n", s - 1, s, b);
                return false;
            }
        }
        if (b > 0) {
            if (r.bands[b].y0 < r.bands[b - 1].y1) {
                printf("    bandas %d/%d solapadas o desordenadas\n", b - 1, b);
                return false;
            }
            if (r.bands[b].y0 == r.bands[b - 1].y1) {
                bool same = r.bands[b].span_count == r.bands[b - 1].span_count;
                for (int s = 0; same && s < band.span_count; ++s) {
                    same = band.spans[s].x0 == r.bands[b - 1].spans[s].x0 &&
                           band.spans[s].x1 == r.bands[b - 1].spans[s].x1;
                }
                if (same) {
                    printf("    bandas %d/%d contiguas e identicas sin fusionar\n", b - 1, b);
                    return false;
                }
            }
        }
    }
    return true;
}

void case_region_basics() {
    printf("caso: region -- rect, bounds, vacio\n");
    sx_region r;
    sx_region_clear(&r);
    check(sx_region_is_empty(&r), "arranca vacia");

    sx_region_set_rect(&r, sx_rect_make(3, 4, 5, 6));
    check(!sx_region_is_empty(&r), "con un rect deja de estar vacia");
    sx_rect b = sx_region_bounds(&r);
    check(b.x == 3 && b.y == 4 && b.width == 5 && b.height == 6, "bounds es el rect");
    check(sx_region_rect_count(&r) == 1, "un solo rect");

    sx_region_set_rect(&r, sx_rect_make(0, 0, 0, 5));
    check(sx_region_is_empty(&r), "un rect vacio deja la region vacia");
}

void case_region_union_keeps_the_L() {
    printf("caso: region -- la union en L NO es el bounding box\n");
    // Este es el caso que separa sx_region de sx_rect_set: sx_rect_set_add
    // fusionaria estos dos en el rect que los contiene.
    sx_region r;
    Grid g;
    sx_region_clear(&r);

    const sx_rect a = sx_rect_make(2, 2, 10, 4);
    const sx_rect c = sx_rect_make(2, 6, 4, 8);
    sx_region_union_rect(&r, a);
    sx_region_union_rect(&r, c);
    g.op_rect(a, 0);
    g.op_rect(c, 0);

    check(region_matches(r, g), "la region es exactamente la L");
    check(region_is_canonical(r), "y esta en forma canonica");

    // La prueba del delito: el bounding box tiene mas area que la L.
    sx_rect bb = sx_region_bounds(&r);
    check(bb.width * bb.height > g.area(), "el bounding box cubriria de mas");

    // Y el sx_rect_set equivalente efectivamente colapsa.
    sx_rect_set set;
    sx_rect_set_clear(&set);
    sx_rect_set_add(&set, a);
    sx_rect_set_add(&set, c);
    check(set.count == 1, "sx_rect_set fusiona los dos en uno (sobre-cubre)");
}

void case_region_subtract_hole() {
    printf("caso: region -- un agujero en el medio\n");
    sx_region r;
    Grid g;
    const sx_rect base = sx_rect_make(4, 4, 20, 16);
    const sx_rect hole = sx_rect_make(10, 8, 6, 6);

    sx_region_set_rect(&r, base);
    sx_region_subtract_rect(&r, hole);
    g.op_rect(base, 0);
    g.op_rect(hole, 1);

    check(region_matches(r, g), "el agujero queda exacto");
    check(region_is_canonical(r), "y la forma sigue canonica");
    check(sx_region_contains_point(&r, 5, 5) != 0, "adentro del marco");
    check(sx_region_contains_point(&r, 12, 10) == 0, "adentro del agujero");
}

void case_region_subtract_everything() {
    printf("caso: region -- restar todo la vacia\n");
    sx_region r;
    sx_region_set_rect(&r, sx_rect_make(5, 5, 10, 10));
    sx_region_subtract_rect(&r, sx_rect_make(0, 0, 40, 40));
    check(sx_region_is_empty(&r), "no queda nada");
    check(sx_region_rect_count(&r) == 0, "y no quedan rects que recorrer");
}

void case_region_intersect() {
    printf("caso: region -- interseccion\n");
    sx_region r;
    Grid g;
    const sx_rect a = sx_rect_make(2, 2, 20, 6);
    const sx_rect b = sx_rect_make(2, 10, 6, 10);
    const sx_rect win = sx_rect_make(4, 4, 10, 10);

    sx_region_clear(&r);
    sx_region_union_rect(&r, a);
    sx_region_union_rect(&r, b);
    sx_region_intersect_rect(&r, win);
    g.op_rect(a, 0);
    g.op_rect(b, 0);
    g.op_rect(win, 2);

    check(region_matches(r, g), "la interseccion es exacta");
    check(region_is_canonical(r), "y canonica");
}

void case_region_matches_grid_under_many_ops() {
    printf("caso: region -- secuencia larga contra el modelo de referencia\n");
    // Secuencia pseudoaleatoria determinista: es donde aparecen los casos que
    // uno no se le ocurren a mano (bandas que se parten y se vuelven a fusionar).
    sx_region r;
    Grid g;
    sx_region_clear(&r);

    uint32_t seed = 12345u;
    auto next = [&seed](int limit) {
        seed = seed * 1103515245u + 12345u;
        return (int)((seed >> 16) % (uint32_t)limit);
    };

    bool ok = true;
    bool canonical = true;
    int applied = 0;
    for (int i = 0; i < 200 && ok; ++i) {
        sx_rect rect = sx_rect_make(next(kGridW), next(kGridH), 1 + next(12), 1 + next(9));
        const int op = next(3);
        sx_region_union_rect(&r, sx_rect_make(0, 0, 0, 0)); // no-op: no debe alterar nada
        if (op == 0) {
            sx_region_union_rect(&r, rect);
        } else if (op == 1) {
            sx_region_subtract_rect(&r, rect);
        } else {
            sx_region_intersect_rect(&r, rect);
        }
        g.op_rect(rect, op);
        ++applied;

        // Una vez desbordada solo se puede exigir superset.
        const bool superset_ok = sx_region_overflowed(&r) != 0;
        if (!region_matches(r, g, superset_ok)) {
            printf("    fallo en la operacion %d (op=%d, desbordada=%d)\n", i, op, (int)superset_ok);
            ok = false;
        }
        if (!region_is_canonical(r)) {
            printf("    forma no canonica tras la operacion %d\n", i);
            canonical = false;
            ok = false;
        }
    }
    check(ok, "coincide con el modelo en las 200 operaciones");
    check(canonical, "y se mantiene canonica en todas");
    check(applied == 200, "se aplicaron las 200");
}

void case_region_overflow_is_a_superset() {
    printf("caso: region -- el desborde es superset, nunca pierde area\n");
    sx_region r;
    Grid g;
    sx_region_clear(&r);

    // Un peine: muchos tramos finos en la misma banda, garantizado a desbordar
    // SX_REGION_MAX_SPANS_PER_BAND.
    for (int i = 0; i < kGridW; i += 2) {
        sx_rect tooth = sx_rect_make(i, 5, 1, 10);
        sx_region_union_rect(&r, tooth);
        g.op_rect(tooth, 0);
    }
    check(sx_region_overflowed(&r), "el peine desborda");
    check(region_matches(r, g, true), "cubre todo lo que debia (superset)");
    check(region_is_canonical(r), "y aun desbordada la forma es valida");
}

/* ---- clip por region ---------------------------------------------------- */

void case_painter_clips_to_region() {
    printf("caso: el painter clipea contra la region\n");
    Canvas c;
    sx_region r;

    sx_region_clear(&r);
    sx_region_union_rect(&r, sx_rect_make(2, 2, 6, 3));
    sx_region_union_rect(&r, sx_rect_make(12, 8, 4, 4));

    check(sx_painter_push_clip_region(&c.painter, &r) != 0, "acepta el push");
    sx_painter_fill(&c.painter, kInk);
    sx_painter_pop_clip(&c.painter);

    check(c.count(kInk) == 6 * 3 + 4 * 4, "pinto exactamente el area de la region");
    check(c.at(2, 2) == kInk && c.at(13, 9) == kInk, "los dos trozos");
    check(c.at(9, 3) != kInk, "nada en el hueco entre ellos");
    check(c.at(8, 2) != kInk, "ni un pixel pasado del borde");
}

void case_painter_region_clip_with_hole() {
    printf("caso: clip por region con agujero\n");
    Canvas c;
    sx_region r;

    sx_region_set_rect(&r, sx_rect_make(1, 1, 20, 14));
    sx_region_subtract_rect(&r, sx_rect_make(6, 5, 5, 5));

    sx_painter_push_clip_region(&c.painter, &r);
    sx_painter_fill_rect(&c.painter, sx_rect_make(0, 0, kWidth, kHeight), kInk);
    sx_painter_pop_clip(&c.painter);

    check(c.count(kInk) == 20 * 14 - 5 * 5, "el agujero queda sin pintar");
    check(c.at(8, 7) != kInk, "centro del agujero limpio");
    check(c.at(5, 7) == kInk && c.at(11, 7) == kInk, "y los costados pintados");
}

void case_painter_region_clip_respects_origin() {
    printf("caso: el clip por region se interpreta en coordenadas locales\n");
    Canvas c;
    sx_region r;
    sx_region_set_rect(&r, sx_rect_make(0, 0, 3, 3));

    sx_painter_push_origin(&c.painter, 10, 5);
    sx_painter_push_clip_region(&c.painter, &r);
    sx_painter_fill_rect(&c.painter, sx_rect_make(-50, -50, 200, 200), kInk);
    sx_painter_pop_clip(&c.painter);
    sx_painter_pop_origin(&c.painter);

    check(c.count(kInk) == 9, "3x3 pintados");
    check(c.at(10, 5) == kInk, "la region cayo en el origen activo");
    check(c.at(0, 0) != kInk, "no en el origen de dispositivo");
}

void case_painter_empty_region_draws_nothing() {
    printf("caso: una region vacia no deja pintar nada\n");
    Canvas c;
    sx_region r;
    sx_region_clear(&r);

    sx_painter_push_clip_region(&c.painter, &r);
    sx_painter_fill(&c.painter, kInk);
    sx_painter_draw_text(&c.painter, 0, 0, "hola", kInk);
    sx_painter_pop_clip(&c.painter);

    check(c.count(kInk) == 0, "no se pinto un solo pixel");
}

void case_painter_region_clip_pops_cleanly() {
    printf("caso: el pop restaura el estado previo al clip por region\n");
    Canvas c;
    sx_region r;
    sx_region_set_rect(&r, sx_rect_make(0, 0, 2, 2));

    sx_painter_push_clip_region(&c.painter, &r);
    sx_painter_pop_clip(&c.painter);
    sx_painter_fill(&c.painter, kInk);

    check(c.count(kInk) == kWidth * kHeight, "sin clip vuelve a pintar todo");
}

void case_region_clip_equals_rect_clip_for_a_rect() {
    printf("caso: region de un rect == clip por rect\n");
    // Si la region degenera en un rect, el resultado tiene que ser identico al
    // camino de siempre. Es la red que impide que el camino nuevo derive.
    Canvas viaRect;
    sx_painter_push_clip(&viaRect.painter, sx_rect_make(3, 4, 9, 7));
    sx_painter_fill(&viaRect.painter, kInk);
    sx_painter_draw_text(&viaRect.painter, 2, 5, "abcdefgh", kBack);
    sx_painter_pop_clip(&viaRect.painter);

    Canvas viaRegion;
    sx_region r;
    sx_region_set_rect(&r, sx_rect_make(3, 4, 9, 7));
    sx_painter_push_clip_region(&viaRegion.painter, &r);
    sx_painter_fill(&viaRegion.painter, kInk);
    sx_painter_draw_text(&viaRegion.painter, 2, 5, "abcdefgh", kBack);
    sx_painter_pop_clip(&viaRegion.painter);

    check(memcmp(viaRect.pixels, viaRegion.pixels, sizeof(viaRect.pixels)) == 0,
          "los dos caminos dan el mismo canvas");
}

void case_region_clip_blit_and_frame() {
    printf("caso: blit y marco tambien respetan la region\n");
    static uint32_t src[16 * 16];
    for (int i = 0; i < 16 * 16; ++i) {
        src[i] = kInk;
    }
    savanxp_fb_info si;
    memset(&si, 0, sizeof(si));
    si.width = 16; si.height = 16; si.pitch = 16 * sizeof(uint32_t); si.bpp = 32u;
    si.buffer_size = si.pitch * si.height;
    sx_bitmap sb;
    sx_bitmap_wrap(&sb, src, &si, SX_PIXEL_FORMAT_BGRX8888);

    sx_region r;
    sx_region_set_rect(&r, sx_rect_make(0, 0, 20, 20));
    sx_region_subtract_rect(&r, sx_rect_make(4, 4, 4, 4));

    Canvas c;
    sx_painter_push_clip_region(&c.painter, &r);
    sx_painter_blit_bitmap(&c.painter, &sb, 0, 0);
    sx_painter_pop_clip(&c.painter);
    check(c.count(kInk) == 16 * 16 - 4 * 4, "el blit deja el agujero");
    check(c.at(5, 5) != kInk, "agujero limpio");

    Canvas f;
    sx_painter_push_clip_region(&f.painter, &r);
    sx_painter_draw_frame(&f.painter, sx_rect_make(2, 2, 8, 8), kInk);
    sx_painter_pop_clip(&f.painter);
    // El marco de 8x8 son 28 celdas; el agujero (4,4)-(8,8) se come las que
    // caen en el borde: (4,2),(5,2),(6,2),(7,2) no -- esas estan en y=2, fuera.
    // Las del borde dentro del agujero son (4,9)? no. Verificamos por posicion.
    check(f.at(2, 2) == kInk, "esquina del marco presente");
    check(f.at(4, 4) != kInk, "y lo que cae en el agujero, no");
}

int region_area(const sx_region& r) {
    int area = 0;
    for (int b = 0; b < r.band_count; ++b) {
        for (int s = 0; s < r.bands[b].span_count; ++s) {
            area += (r.bands[b].spans[s].x1 - r.bands[b].spans[s].x0) * (r.bands[b].y1 - r.bands[b].y0);
        }
    }
    return area;
}

int rect_set_area(const sx_rect_set& s) {
    int area = 0;
    for (size_t i = 0; i < s.count; ++i) {
        area += s.rects[i].width * s.rects[i].height;
    }
    return area;
}

void case_subtract_alone_does_not_over_cover() {
    printf("caso: restar sola NO es donde sx_rect_set pierde\n");
    // Vale dejarlo asentado porque es contraintuitivo: sx_rect_set_subtract_rect
    // usa push_raw, que NO fusiona, asi que la resta ya era exacta. Con un solo
    // rect de danio (pantalla entera) la region no gana area, solo exactitud de
    // representacion. La ganancia esta en el caso de abajo.
    const sx_rect bounds = sx_rect_make(0, 0, kGridW, kGridH);
    const sx_rect front_a = sx_rect_make(6, 0, 12, 14);
    const sx_rect front_b = sx_rect_make(24, 10, 14, 20);

    sx_region region;
    sx_region_set_rect(&region, bounds);
    sx_region_subtract_rect(&region, front_a);
    sx_region_subtract_rect(&region, front_b);

    sx_rect_set set;
    sx_rect_set_clear(&set);
    sx_rect_set_add(&set, bounds);
    sx_rect_set_subtract_rect(&set, front_a);
    sx_rect_set_subtract_rect(&set, front_b);

    Grid g;
    g.op_rect(bounds, 0);
    g.op_rect(front_a, 1);
    g.op_rect(front_b, 1);
    const int exact = g.area();

    printf("    exacto=%d  region=%d px  sx_rect_set=%d px\n",
           exact, region_area(region), rect_set_area(set));
    check(region_matches(region, g), "la region es exactamente lo visible");
    check(region_area(region) == exact, "la region no cubre de mas");
    check(rect_set_area(set) == exact, "y con una sola fuente de danio, el set tampoco");
}

void case_region_beats_rect_set_on_multi_rect_damage() {
    printf("caso: region vs sx_rect_set con danio en varios rects\n");
    // ESTE es el caso donde windowd pierde hoy: cuando el frame trae varios
    // rects sucios, el compose los mete uno por uno con sx_rect_set_add, que
    // fusiona por bounding box en cuanto dos se tocan o se solapan.
    const sx_rect bounds = sx_rect_make(0, 0, kGridW, kGridH);
    const sx_rect damage[3] = {
        sx_rect_make(0, 0, 10, 6),    // arriba a la izquierda
        sx_rect_make(8, 4, 6, 20),    // baja por el medio, TOCA al anterior
        sx_rect_make(30, 24, 8, 5),   // suelto abajo a la derecha
    };
    const sx_rect occluder = sx_rect_make(9, 8, 3, 6);

    sx_region region;
    sx_rect_set set;
    Grid g;

    sx_region_clear(&region);
    sx_rect_set_clear(&set);
    for (int i = 0; i < 3; ++i) {
        sx_rect clipped = sx_rect_intersect(damage[i], bounds);
        sx_region_union_rect(&region, clipped);
        sx_rect_set_add(&set, clipped);
        g.op_rect(clipped, 0);
    }
    sx_region_subtract_rect(&region, occluder);
    sx_rect_set_subtract_rect(&set, occluder);
    g.op_rect(occluder, 1);

    const int exact = g.area();
    printf("    exacto=%d  region=%d px en %d rects  sx_rect_set=%d px en %d rects\n",
           exact, region_area(region), (int)sx_region_rect_count(&region),
           rect_set_area(set), (int)set.count);

    check(region_matches(region, g), "la region es exactamente el area a repintar");
    check(region_area(region) == exact, "la region no cubre un pixel de mas");
    check(rect_set_area(set) > exact, "sx_rect_set cubre de mas al fusionar el danio");
}

/* ---- UTF-8 -------------------------------------------------------------- */

void case_utf8_decodes_codepoints() {
    printf("caso: UTF-8 -- decodifica los cuatro largos\n");
    struct { const char* text; unsigned int cp; int consumed; } cases[] = {
        {"A",                0x41u,    1}, // ASCII
        {"\xC3\xB3",         0xF3u,    2}, // o con tilde
        {"\xC3\x91",         0xD1u,    2}, // N con virgulilla
        {"\xE2\x80\xA6",     0x2026u,  3}, // puntos suspensivos
        {"\xE2\x82\xAC",     0x20ACu,  3}, // euro
        {"\xF0\x9F\x99\x82", 0x1F642u, 4}, // fuera de las tablas, pero decodifica
    };
    bool ok = true;
    for (auto& c : cases) {
        const char* cursor = c.text;
        const unsigned int got = sx_utf8_next(&cursor);
        if (got != c.cp || (int)(cursor - c.text) != c.consumed) {
            printf("    '%s': cp=0x%X (esperado 0x%X), consumio %d (esperado %d)\n",
                   c.text, got, c.cp, (int)(cursor - c.text), c.consumed);
            ok = false;
        }
    }
    check(ok, "los seis casos dan codepoint y avance correctos");
}

void case_utf8_walks_a_whole_string() {
    printf("caso: UTF-8 -- recorre una cadena entera\n");
    // "Configuración…" tal como saldria de un .c en UTF-8.
    const char* text = "Configuraci\xC3\xB3n\xE2\x80\xA6";
    const char* cursor = text;
    int glyphs = 0;
    unsigned int last = 0;
    while (*cursor != '\0') {
        last = sx_utf8_next(&cursor);
        ++glyphs;
    }
    check(glyphs == 14, "14 caracteres, no 17 bytes");
    check(last == 0x2026u, "el ultimo es U+2026");
}

void case_utf8_invalid_always_advances() {
    printf("caso: UTF-8 -- lo invalido avanza siempre (nunca cuelga)\n");
    // Continuador suelto, secuencia truncada y byte prohibido: lo que importa no
    // es que devuelva algo lindo, es que el cursor SIEMPRE avance.
    const char* bad[] = { "\x80", "\xC3", "\xFF", "\xE2\x80", "\xC3\x28" };
    bool advances = true;
    bool replaces = true;
    for (auto& b : bad) {
        const char* cursor = b;
        const char* before = cursor;
        const unsigned int cp = sx_utf8_next(&cursor);
        if (cursor <= before) {
            advances = false;
        }
        if (cp != 0xFFFDu) {
            replaces = false;
        }
    }
    check(advances, "el cursor avanza en los cinco casos malos");
    check(replaces, "y todos dan U+FFFD");

    // El caso que importa de verdad: un bucle sobre basura termina.
    const char* junk = "\xFF\xFE\xC3\x80\x80ok";
    const char* cursor = junk;
    int guard = 0;
    while (*cursor != '\0' && guard < 100) {
        (void)sx_utf8_next(&cursor);
        ++guard;
    }
    check(guard < 100, "un bucle sobre basura termina");
}

void case_font_table_has_the_codepoints_that_matter() {
    printf("caso: la tabla horneada tiene los codepoints que importan\n");
    // El fallback (slot 0) es el espacio: ancho 0 de bitmap pero con avance.
    // En C el nombre de la funcion y el del struct conviven; en C++ la funcion
    // tapa al tag, asi que aca hay que decir struct.
    const struct sx_noto_glyph* fallback = sx_noto_glyph(0x1F642u); // emoji, fuera de rango
    check(fallback->rows == 0 && fallback->advance > 0,
          "un codepoint fuera de rango cae al espacio, con avance");

    // Lo que el lote 2.2 vino a habilitar: acentos y puntuacion tipografica.
    struct { unsigned int cp; const char* name; } wanted[] = {
        {0x00F3u, "o con tilde"},
        {0x00D1u, "N con virgulilla"},
        {0x00BFu, "signo de apertura de interrogacion"},
        {0x00AAu, "ordinal femenino"},
        {0x2014u, "raya"},
        {0x2026u, "puntos suspensivos"},
        {0x2022u, "bullet"},
        {0x20ACu, "euro"},
    };
    bool all = true;
    for (auto& w : wanted) {
        const struct sx_noto_glyph* g = sx_noto_glyph(w.cp);
        if (g == fallback || g->rows == 0 || g->advance <= 0) {
            printf("    falta U+%04X (%s)\n", w.cp, w.name);
            all = false;
        }
    }
    check(all, "los ocho estan horneados y son dibujables");

    // Y el ancho de una cadena con acento tiene que contar CARACTERES.
    const char* accented = "Configuraci\xC3\xB3n";
    const char* plain = "Configuracion";
    int wa = 0, wp = 0;
    for (const char* c = accented; *c;) { wa += sx_noto_glyph(sx_utf8_next(&c))->advance; }
    for (const char* c = plain; *c;) { wp += sx_noto_glyph(sx_utf8_next(&c))->advance; }
    printf("    ancho con acento=%d  sin acento=%d\n", wa, wp);
    check(wa > 0 && wa == wp, "'Configuracion' mide igual con y sin tilde (misma cantidad de glifos)");
}

/* ---- objeto fuente ------------------------------------------------------ */

void case_font_selection_round_trips() {
    printf("caso: fuente -- seleccionar y restaurar\n");
    Canvas c;
    check(sx_painter_font(&c.painter) == SX_FONT_UI, "arranca en la fuente de UI");

    const int previous = sx_painter_set_font(&c.painter, SX_FONT_MONO);
    check(previous == SX_FONT_UI, "set_font devuelve la anterior");
    check(sx_painter_font(&c.painter) == SX_FONT_MONO, "y quedo la mono");

    sx_painter_set_font(&c.painter, previous);
    check(sx_painter_font(&c.painter) == SX_FONT_UI, "restaurar funciona");

    sx_painter_set_font(&c.painter, 99);
    check(sx_painter_font(&c.painter) == SX_FONT_UI, "un id invalido no cambia nada");
}

void case_font_drives_metrics_and_blit() {
    printf("caso: fuente -- manda en metricas y en que blit se usa\n");
    Canvas c;
    g_mono_blits = 0;
    g_ui_blits = 0;

    // Los stubs dan anchos distintos por fuente (1 px/char vs 8 px/char), asi
    // que las metricas delatan cual se consulto.
    const int ui_width = sx_painter_text_width(&c.painter, "abcd");
    sx_painter_set_font(&c.painter, SX_FONT_MONO);
    const int mono_width = sx_painter_text_width(&c.painter, "abcd");
    check(ui_width == 4 && mono_width == 32, "text_width sigue a la fuente activa");
    check(sx_painter_text_height(&c.painter) == 16, "y text_height tambien");

    sx_painter_draw_text(&c.painter, 0, 0, "abcd", kInk);
    check(g_mono_blits == 1 && g_ui_blits == 0, "con mono activa se usa el blit mono");

    sx_painter_set_font(&c.painter, SX_FONT_UI);
    sx_painter_draw_text(&c.painter, 0, 0, "abcd", kInk);
    check(g_ui_blits == 1, "y con la de UI, el de UI");
}

void case_font_survives_clip_and_region() {
    printf("caso: fuente -- el clip no la pisa\n");
    Canvas c;
    sx_region r;
    sx_region_set_rect(&r, sx_rect_make(0, 0, 10, 10));

    sx_painter_set_font(&c.painter, SX_FONT_MONO);
    g_mono_blits = 0;
    g_ui_blits = 0;
    sx_painter_push_clip_region(&c.painter, &r);
    sx_painter_draw_text(&c.painter, 0, 0, "ab", kInk);
    sx_painter_pop_clip(&c.painter);

    check(g_mono_blits >= 1 && g_ui_blits == 0, "el camino por region respeta la fuente");
    check(sx_painter_font(&c.painter) == SX_FONT_MONO, "y el pop del clip no la toca");
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
    ++g_ui_blits;
    if (text == nullptr) {
        return;
    }
    gfx_rect(pixels, info, x, y, (int)strlen(text), 1, colour);
}

/* La fuente mono en el stub mide 8x16 por celda, como la UniFont real: los
 * anchos distintos son justamente lo que delata cual fuente se consulto. */
int gfx_text_width_mono(const char* text) {
    return text == nullptr ? 0 : (int)strlen(text) * 8;
}

int gfx_cell_height(void) {
    return 16;
}

void gfx_blit_text_mono(uint32_t* pixels, const struct savanxp_fb_info* info, int x, int y, const char* text, uint32_t colour) {
    ++g_mono_blits;
    (void)pixels; (void)info; (void)x; (void)y; (void)text; (void)colour;
}

void gfx_blit_text_mono_clip(uint32_t* pixels, const struct savanxp_fb_info* info, int x, int y, const char* text, uint32_t colour,
                             int clip_x0, int clip_y0, int clip_x1, int clip_y1) {
    ++g_mono_blits;
    (void)pixels; (void)info; (void)x; (void)y; (void)text; (void)colour;
    (void)clip_x0; (void)clip_y0; (void)clip_x1; (void)clip_y1;
}

void gfx_blit_text_clip(uint32_t* pixels, const struct savanxp_fb_info* info, int x, int y, const char* text, uint32_t colour,
                        int clip_x0, int clip_y0, int clip_x1, int clip_y1) {
    ++g_ui_blits;
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

    case_region_basics();
    case_region_union_keeps_the_L();
    case_region_subtract_hole();
    case_region_subtract_everything();
    case_region_intersect();
    case_region_matches_grid_under_many_ops();
    case_region_overflow_is_a_superset();
    case_subtract_alone_does_not_over_cover();
    case_region_beats_rect_set_on_multi_rect_damage();

    case_utf8_decodes_codepoints();
    case_utf8_walks_a_whole_string();
    case_utf8_invalid_always_advances();
    case_font_table_has_the_codepoints_that_matter();
    case_font_selection_round_trips();
    case_font_drives_metrics_and_blit();
    case_font_survives_clip_and_region();

    case_painter_clips_to_region();
    case_painter_region_clip_with_hole();
    case_painter_region_clip_respects_origin();
    case_painter_empty_region_draws_nothing();
    case_painter_region_clip_pops_cleanly();
    case_region_clip_equals_rect_clip_for_a_rect();
    case_region_clip_blit_and_frame();

    printf("%s (%d checks, %d fallas)\n",
           g_failures == 0 ? "GFX2D TEST PASS" : "GFX2D TEST FAIL",
           g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
