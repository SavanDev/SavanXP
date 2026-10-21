#pragma once

#include "windowd_session.h"
#include "windowd_presentation.h"

/* Danio acumulado del frame, como region EXACTA. Fue un sx_rect_set hasta que
 * se midio lo que costaba: sx_rect_set_add fusiona por bounding box en cuanto
 * dos rects se tocan o se solapan aunque sea un pixel, asi que arrastrar una
 * ventana -- danio = marco viejo U marco nuevo -- repintaba y presentaba el
 * rectangulo que contiene a los dos en vez del anillo que de verdad cambio. */
struct windowd_dirty_rect
{
    struct sx_region region;
};

/* Background colour when the desktop has not drawn one: before its first
 * frame, or while it is being restarted. */
#define WINDOWD_FALLBACK_BACKGROUND 0x00008080u

/* windowd's own surface (compositor_protocol.h SAVANXP_COMPOSITOR_SURFACE_WM),
 * which it paints in screen coordinates with `pitch`. */
void windowd_set_wm_surface(uint32_t *pixels, uint32_t pitch);
void windowd_dirty_rect_reset(struct windowd_dirty_rect *dirty);
void windowd_dirty_rect_add(struct windowd_dirty_rect *dirty, const struct savanxp_fb_info *info, int x, int y, int width, int height);
void windowd_dirty_rect_add_fullscreen(struct windowd_dirty_rect *dirty, const struct savanxp_fb_info *info);
void windowd_dirty_rect_add_cursor(struct windowd_dirty_rect *dirty, const struct savanxp_fb_info *info, int cursor_x, int cursor_y, int shape);
void windowd_dirty_rect_add_client(struct windowd_dirty_rect *dirty, const struct windowd_client *client);
int windowd_dirty_rect_valid(const struct windowd_dirty_rect *dirty);
size_t windowd_dirty_rect_count(const struct windowd_dirty_rect *dirty);
/* Copia el rect numero `index` en `out`. Devuelve 0 si el indice se paso. No
 * devuelve un puntero como antes porque la region guarda bandas y tramos, no
 * rects: el rect se arma al leerlo. */
int windowd_dirty_rect_at(const struct windowd_dirty_rect *dirty, size_t index, struct sx_rect *out);

/* Validates the region primitives the compositor relies on: the
 * sx_rect_set_subtract_rect occlusion culling and the exactness of the dirty
 * region it accumulates damage into. Returns 0 on success, non-zero on
 * failure. */
int windowd_region_selftest(void);
/* Whether a window is in the scene: it exists, is not minimized, and has
 * published its first frame. */
int windowd_client_in_scene(const struct windowd_client *client);
/* The layers of the scene, back to front (docs/desktop/DESKTOP_ARCHITECTURE.md#
 * the-scene). Returns how many it wrote. */
uint32_t windowd_build_scene(
    const struct windowd_session *session,
    struct savanxp_compositor_layer *layers,
    uint32_t capacity);
/* Repaints windowd's surface over `dirty`: fallback background, the frames of
 * the windows in the scene, the switcher. */
void windowd_paint_wm_surface(struct windowd_session *session, const struct windowd_dirty_rect *dirty);
