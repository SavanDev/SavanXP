#pragma once

/* The platform painter (sxgfx/gfx2d.h) plus what only a POSIX program has:
 * the scaled presenter, which presents onto a POSIX window. */

#include "sxgfx/gfx2d.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Presenter de pixel art de resolucion fija: escala entera con vecino mas
 * cercano, centra en la superficie cliente y presenta solo la banda minima de
 * filas fuente que cambio. El SDK mantiene la copia anterior y la fila expandida;
 * la app solo entrega su frame original. */
struct savanxp_gfx_context;
struct sx_scaled_presenter {
    struct savanxp_gfx_context* gfx;
    struct sx_bitmap source;
    struct sx_bitmap target;
    struct sx_rect destination;
    uint32_t scale;
    uint32_t* expanded_row;
    uint32_t* previous;
    size_t expanded_row_capacity;
    size_t source_pixel_count;
    int previous_valid;
    int force_surface_present;
};

/* Inicializa un presenter 320x200-style. `source_pixels` es un frame BGRX
 * contiguo de `source_width * source_height` pixeles. Devuelve 0 o un error
 * negativo. El presenter no adquiere ni libera el contexto gfx. */
int sx_scaled_presenter_init(
    struct sx_scaled_presenter* presenter,
    struct savanxp_gfx_context* gfx,
    uint32_t source_width,
    uint32_t source_height,
    uint32_t* source_pixels,
    uint32_t background);
/* Reaplica el layout tras un RESIZED, limpia el fondo y fuerza el proximo frame
 * completo. El buffer destino conservado por el runtime gfx puede ser mayor que
 * el nuevo width/height; por eso se informa el rect visible, no su capacidad. */
int sx_scaled_presenter_retarget(
    struct sx_scaled_presenter* presenter,
    uint32_t background);
/* 1 si hubo damage y present, 0 si el frame no cambio, negativo si el present
 * fallo. Solo se confirma la copia anterior despues de un present exitoso. */
long sx_scaled_presenter_present(
    struct sx_scaled_presenter* presenter,
    const uint32_t* source_pixels);
void sx_scaled_presenter_destroy(struct sx_scaled_presenter* presenter);

#ifdef __cplusplus
}
#endif
