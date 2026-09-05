#pragma once

#include <stddef.h>
#include <stdint.h>

#include "savanxp/syscall.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SX_RECT_SET_CAPACITY 64

enum sx_pixel_format {
    SX_PIXEL_FORMAT_INVALID = 0,
    SX_PIXEL_FORMAT_BGRX8888 = 1,
    SX_PIXEL_FORMAT_BGRA8888 = 2,
};

struct sx_point {
    int x;
    int y;
};

struct sx_size {
    int width;
    int height;
};

struct sx_rect {
    int x;
    int y;
    int width;
    int height;
};

struct sx_bitmap {
    uint32_t* pixels;
    struct savanxp_fb_info info;
    uint32_t format;
};

#define SX_PAINTER_CLIP_STACK_DEPTH 16
#define SX_PAINTER_ORIGIN_STACK_DEPTH 16

/* Un brush al estilo GDI: color solido, o una trama de 8x8 de 1 bit. La trama
 * se ancla a coordenadas de DISPOSITIVO (no al rect que se pinta), asi dos
 * controles vecinos caen en la misma grilla en vez de bailar segun donde
 * arranque cada uno -- es la regla que los dibujos a mano de sxgui ya seguian.
 * Bit 7 de cada byte es la columna izquierda; fila i = pattern[i & 7]. */
struct sx_brush {
    uint32_t colour;
    uint32_t back_colour;
    uint8_t pattern[8];
    int has_pattern;
    /* Con trama: los bits en 0 no se pintan (deja pasar el fondo) en vez de
     * rellenarse con back_colour. */
    int transparent;
};

/* Trama al 50%: pinta donde (x + y) es par. Es la del focus rect punteado y la
 * del riel de las barras de scroll. */
extern const uint8_t sx_pattern_checker_50[8];

struct sx_painter {
    struct sx_bitmap* target;
    /* En coordenadas de dispositivo, igual que origin: las funciones publicas
     * traducen lo que recibe el llamador antes de tocar estos campos. */
    struct sx_rect clip_rect;
    int has_clip;
    struct sx_rect saved_clip[SX_PAINTER_CLIP_STACK_DEPTH];
    int saved_has_clip[SX_PAINTER_CLIP_STACK_DEPTH];
    int clip_depth;
    /* Desplazamiento sumado a toda coordenada que entra por la API publica
     * (analogo a SetViewportOrgEx de GDI). Arranca en (0,0). */
    struct sx_point origin;
    struct sx_point saved_origin[SX_PAINTER_ORIGIN_STACK_DEPTH];
    int origin_depth;
    /* Clip por region (analogo a SelectClipRgn). PRESTADA: el llamador es dueño
     * y tiene que mantenerla viva mientras este pusheada. Se interpreta en las
     * coordenadas del painter al momento del push; clip_region_origin guarda el
     * origen de entonces para poder traducir despues. clip_rect siempre sigue
     * siendo el bounding box del clip, region incluida, asi que el descarte
     * rapido no necesita mirar la region. */
    const struct sx_region* clip_region;
    struct sx_point clip_region_origin;
    const struct sx_region* saved_clip_region[SX_PAINTER_CLIP_STACK_DEPTH];
    struct sx_point saved_clip_region_origin[SX_PAINTER_CLIP_STACK_DEPTH];
};

struct sx_rect_set {
    size_t count;
    struct sx_rect rects[SX_RECT_SET_CAPACITY];
};

/* --- Regiones -----------------------------------------------------------
 *
 * Una region por BANDAS en Y, el modelo de HRGN de GDI: bandas ordenadas y
 * disjuntas, cada una con sus tramos en X ordenados, disjuntos y no adyacentes.
 * A diferencia de sx_rect_set, la forma es canonica y las operaciones son
 * EXACTAS: unir dos rects en L da la L, no el rectangulo que los contiene.
 *
 * Por que existiendo sx_rect_set: sx_rect_set_add fusiona por bounding box ante
 * cualquier solape, asi que sobre-cubre. Para acumular danio da igual (pintar de
 * mas no rompe), pero para clipear una capa contra lo que quedo visible, cada
 * pixel de mas es trabajo de repintado.
 *
 * DESBORDE: al pasarse de bandas o tramos la region colapsa a su bounding box y
 * queda marcada (sx_region_overflowed). Es un SUPERSET -- nunca pierde area --
 * igual que el desborde de sx_rect_set. Para el compose eso es seguro porque se
 * pinta de atras para adelante y lo pintado de mas lo tapa la capa de adelante;
 * para un clip cualquiera significa que se puede pintar de mas, asi que quien
 * necesite el clip exacto tiene que consultar la marca. */
#define SX_REGION_MAX_BANDS 32
#define SX_REGION_MAX_SPANS_PER_BAND 8

struct sx_region_span {
    int x0;
    int x1;
};

struct sx_region_band {
    int y0;
    int y1;
    int span_count;
    struct sx_region_span spans[SX_REGION_MAX_SPANS_PER_BAND];
};

struct sx_region {
    int band_count;
    int overflowed;
    struct sx_region_band bands[SX_REGION_MAX_BANDS];
};

struct sx_rect sx_rect_make(int x, int y, int width, int height);
int sx_rect_is_empty(struct sx_rect rect);
int sx_rect_right(struct sx_rect rect);
int sx_rect_bottom(struct sx_rect rect);
struct sx_rect sx_rect_translate(struct sx_rect rect, int dx, int dy);
struct sx_rect sx_rect_intersect(struct sx_rect left, struct sx_rect right);
struct sx_rect sx_rect_union(struct sx_rect left, struct sx_rect right);
int sx_rect_contains_point(struct sx_rect rect, int x, int y);

void sx_bitmap_wrap(struct sx_bitmap* bitmap, uint32_t* pixels, const struct savanxp_fb_info* info, uint32_t format);

struct sx_brush sx_brush_solid(uint32_t colour);
struct sx_brush sx_brush_pattern(const uint8_t pattern[8], uint32_t colour, uint32_t back_colour);
struct sx_brush sx_brush_pattern_transparent(const uint8_t pattern[8], uint32_t colour);

void sx_painter_init(struct sx_painter* painter, struct sx_bitmap* bitmap);
void sx_painter_clear_clip(struct sx_painter* painter);
void sx_painter_add_clip_rect(struct sx_painter* painter, struct sx_rect rect);
int sx_painter_push_clip(struct sx_painter* painter, struct sx_rect rect);
/* Igual que push_clip pero contra una region. La region es PRESTADA: tiene que
 * seguir viva y sin cambios hasta el pop. Comparte pila con push_clip, asi que
 * se emparejan con el mismo sx_painter_pop_clip. Una region desbordada clipea
 * por su bounding box (pinta de mas, nunca de menos). */
int sx_painter_push_clip_region(struct sx_painter* painter, const struct sx_region* region);
void sx_painter_pop_clip(struct sx_painter* painter);

/* Corre el origen dx/dy relativo al actual. Devuelve 0 si la pila esta llena
 * (origen sin cambios); emparejar cada push exitoso con un pop. */
int sx_painter_push_origin(struct sx_painter* painter, int dx, int dy);
void sx_painter_pop_origin(struct sx_painter* painter);
struct sx_point sx_painter_origin(const struct sx_painter* painter);
/* El clip activo en coordenadas LOCALES (las mismas que aceptan las funciones
 * de dibujo). Sin clip devuelve el target entero. Es la forma soportada de
 * consultarlo: leer painter->clip_rect a mano da coordenadas de dispositivo. */
struct sx_rect sx_painter_clip_bounds(const struct sx_painter* painter);

void sx_painter_fill(struct sx_painter* painter, uint32_t colour);
void sx_painter_fill_rect(struct sx_painter* painter, struct sx_rect rect, uint32_t colour);
void sx_painter_draw_frame(struct sx_painter* painter, struct sx_rect rect, uint32_t colour);
void sx_painter_set_pixel(struct sx_painter* painter, int x, int y, uint32_t colour);
void sx_painter_hline(struct sx_painter* painter, int x, int y, int width, uint32_t colour);
void sx_painter_vline(struct sx_painter* painter, int x, int y, int height, uint32_t colour);
void sx_painter_fill_rect_brush(struct sx_painter* painter, struct sx_rect rect, const struct sx_brush* brush);
void sx_painter_draw_frame_brush(struct sx_painter* painter, struct sx_rect rect, const struct sx_brush* brush);
void sx_painter_blit_bitmap(struct sx_painter* painter, const struct sx_bitmap* source, int dst_x, int dst_y);
void sx_painter_draw_scaled_bitmap_nearest(
    struct sx_painter* painter,
    const struct sx_bitmap* source,
    struct sx_rect destination,
    struct sx_rect source_rect);
void sx_painter_draw_text(struct sx_painter* painter, int x, int y, const char* text, uint32_t colour);

void sx_region_clear(struct sx_region* region);
void sx_region_set_rect(struct sx_region* region, struct sx_rect rect);
void sx_region_copy(struct sx_region* destination, const struct sx_region* source);
void sx_region_union_rect(struct sx_region* region, struct sx_rect rect);
void sx_region_subtract_rect(struct sx_region* region, struct sx_rect rect);
void sx_region_intersect_rect(struct sx_region* region, struct sx_rect rect);
int sx_region_is_empty(const struct sx_region* region);
int sx_region_overflowed(const struct sx_region* region);
struct sx_rect sx_region_bounds(const struct sx_region* region);
int sx_region_contains_point(const struct sx_region* region, int x, int y);
/* Cantidad de rectangulos que hace falta recorrer para cubrir la region. Sirve
 * para comparar contra el conteo de un sx_rect_set equivalente. */
size_t sx_region_rect_count(const struct sx_region* region);

void sx_rect_set_clear(struct sx_rect_set* set);
int sx_rect_set_add(struct sx_rect_set* set, struct sx_rect rect);
int sx_rect_set_subtract_rect(struct sx_rect_set* set, struct sx_rect hole);
int sx_rect_set_add_translated(struct sx_rect_set* set, struct sx_rect rect, int dx, int dy);
struct sx_rect sx_rect_set_bounds(const struct sx_rect_set* set);
int sx_rect_set_valid(const struct sx_rect_set* set);

#ifdef __cplusplus
}
#endif
