#pragma once

/*
 * sxchrome - the system 3D edges and the disabled treatment, shared.
 *
 * WHY THIS IS NOT PART OF SXGUI-C: the widget toolkit is not the only thing
 * that paints Win9x chrome. `windowd` draws window frames and title bar
 * buttons, and an app that paints its own content (a board of cells, a meter,
 * a calculator display) draws boxes that have to match the toolkit's. Before
 * this module each of them kept a private copy of the same eight lines --
 * docs/SYSTEM_LAYERING.md already states the rule this breaks ("SXGUI-C is the
 * canonical toolkit, and an app never reimplements a piece of it"), and
 * `sxgui_draw_raised_edge()` was the first patch on it.
 *
 * The toolkit could not simply be linked by `windowd`: the edges live in the
 * monolithic sxgui.c, so the window manager would have dragged menus, listbox
 * and textedit into its binary to get eight lines of bevel. So the edges come
 * DOWN here instead -- above SXGFX (it is the painter that draws them) and
 * below SXGUI-C (which now consumes them, like everyone else).
 *
 * It is small enough to sit in the base runtime of every userland binary,
 * next to gfx2d.c, so nothing has to opt in.
 */

#include "savanxp/gfx2d.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SXCHROME_RGB(r, g, b) (((uint32_t)(r) << 16) | ((uint32_t)(g) << 8) | (uint32_t)(b))

/* Classic 3D system palette.
 *
 * Son los cuatro tonos del esquema "Windows Standard" y en ese orden se leen:
 * DARK y SHADOW hunden, BEVEL y LIGHT levantan. Un borde 3D de la epoca usa
 * los CUATRO -- dos pixeles por lado, no uno -- y BEVEL es el que faltaba: sin
 * el, el bisel queda de un solo pixel y los controles se ven planos. */
#define SXCHROME_COLOR_FACE          SXCHROME_RGB(192, 192, 192)  /* 3DFACE */
#define SXCHROME_COLOR_SHADOW        SXCHROME_RGB(128, 128, 128)  /* 3DSHADOW */
#define SXCHROME_COLOR_DARK          SXCHROME_RGB(0, 0, 0)        /* 3DDKSHADOW */
#define SXCHROME_COLOR_BEVEL         SXCHROME_RGB(223, 223, 223)  /* 3DLIGHT */
#define SXCHROME_COLOR_LIGHT         SXCHROME_RGB(255, 255, 255)  /* 3DHILIGHT */
#define SXCHROME_COLOR_TEXT          SXCHROME_RGB(0, 0, 0)
#define SXCHROME_COLOR_DISABLED_TEXT SXCHROME_RGB(128, 128, 128)
#define SXCHROME_COLOR_FIELD         SXCHROME_RGB(255, 255, 255)
#define SXCHROME_COLOR_SELECT        SXCHROME_RGB(0, 0, 128)
#define SXCHROME_COLOR_SELECT_TEXT   SXCHROME_RGB(255, 255, 255)
#define SXCHROME_COLOR_WINDOW        SXCHROME_RGB(192, 192, 192)

/* Windows Standard caption palette.  These are the base system colors: the
 * active title bar runs from navy to blue, while the inactive one runs from
 * dark to light gray.  The WM may blend an application's accent into the active
 * endpoints, but it never replaces this shared scheme. */
#define SXCHROME_COLOR_CAPTION_ACTIVE            SXCHROME_RGB(0, 0, 128)
#define SXCHROME_COLOR_CAPTION_ACTIVE_GRADIENT   SXCHROME_RGB(16, 132, 208)
#define SXCHROME_COLOR_CAPTION_INACTIVE          SXCHROME_RGB(128, 128, 128)
#define SXCHROME_COLOR_CAPTION_INACTIVE_GRADIENT SXCHROME_RGB(181, 181, 181)
#define SXCHROME_COLOR_CAPTION_TEXT              SXCHROME_RGB(255, 255, 255)
#define SXCHROME_COLOR_CAPTION_INACTIVE_TEXT     SXCHROME_RGB(192, 192, 192)

/* Espesor de cada bisel. Entra en la cuenta de cualquiera que reparta el area
 * util de un control: el interior es el rect menos esto de cada lado. */
#define SXCHROME_BORDER_RAISED 2 /* botones, cabeceras, popups */
#define SXCHROME_BORDER_SUNKEN 2 /* campos, listas, editores */
#define SXCHROME_BORDER_INSET  1 /* paneles de barra de estado */

/*
 * El primitivo: DOS anillos de un pixel, cada uno con su tono arriba-izquierda
 * y abajo-derecha. Cuatro colores en total. El anillo externo separa el control
 * del fondo y el interno le da el espesor.
 *
 * Todo el resto del chrome sale de aca cambiando el ORDEN de los cuatro, que es
 * lo que hace que hundido y levantado sean exactamente el reverso uno del otro
 * y no dos dibujos parecidos.
 *
 * Se exporta con los cuatro tonos abiertos, y no solo con los envoltorios de
 * abajo, porque el WM compone superficies que pueden necesitar ordenar los tonos
 * de otra manera. Compartir la primitiva permite que toda superficie del sistema
 * conserve la misma construccion de dos pixeles sin copiarla.
 *
 * No dibuja nada si el rect es vacio; con ancho o alto <= 2 dibuja solo el
 * anillo externo, que es donde el interno ya no entra.
 */
void sxchrome_draw_edge(
    struct sx_painter *painter,
    struct sx_rect rect,
    uint32_t outer_light,
    uint32_t outer_dark,
    uint32_t inner_light,
    uint32_t inner_dark);

/* Borde levantado (botones, cara de ventana) con la paleta del sistema. */
void sxchrome_draw_raised(struct sx_painter *painter, struct sx_rect rect);

/* Su reverso exacto: lo comparten el boton apretado y los campos, que es lo que
 * hace que un boton hundido y una caja de texto tengan el mismo espesor. */
void sxchrome_draw_sunken(struct sx_painter *painter, struct sx_rect rect);

/* Bisel de UN pixel, para lo que no lleva espesor: los paneles de la barra de
 * estado, los medidores y las cajas que solo separan del fondo. */
void sxchrome_draw_inset(struct sx_painter *painter, struct sx_rect rect);

/* Linea "grabada": el marco oscuro y el claro corridos un pixel en diagonal.
 * Es el borde del group box y el de los separadores de menu. */
void sxchrome_draw_etched(struct sx_painter *painter, struct sx_rect rect);

/* Cara + borde de un control: rellena con `face` y le pone el bisel levantado o
 * el hundido. Es el boton entero menos su rotulo. */
void sxchrome_fill_raised(struct sx_painter *painter, struct sx_rect rect, uint32_t face, int pressed);

/*
 * Rotulo de un control deshabilitado: gris con una copia blanca corrida un
 * pixel abajo a la derecha. Es el texto "grabado" de la epoca -- se lee apagado
 * sin desaparecer, que es justo lo que un gris plano sobre gris no logra.
 */
void sxchrome_draw_text_disabled(struct sx_painter *painter, int x, int y, const char *text);

/*
 * La misma copia blanca corrida, para un glifo que no es texto: la flecha de un
 * combo, el cuadrado de un maximizar. El llamador pasa la funcion que lo dibuja
 * y se la llama DOS veces, primero el realce y despues el gris.
 *
 * Existe porque el relieve grabado es lo que distingue un control apagado de uno
 * mal pintado, y hasta ahora solo lo tenia el texto: cualquier glifo que
 * quisiera apagarse tenia que reimplementarlo, que es el problema que este
 * archivo cierra.
 */
typedef void (*sxchrome_glyph_fn)(struct sx_painter *painter, struct sx_rect rect, uint32_t colour);
void sxchrome_draw_glyph_disabled(struct sx_painter *painter, struct sx_rect rect, sxchrome_glyph_fn draw);

#ifdef __cplusplus
}
#endif
