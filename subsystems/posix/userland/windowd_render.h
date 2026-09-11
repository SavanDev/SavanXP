#pragma once

#include "windowd_session.h"
#include "windowd_appinfo.h"

/* Danio acumulado del frame, como region EXACTA. Fue un sx_rect_set hasta que
 * se midio lo que costaba: sx_rect_set_add fusiona por bounding box en cuanto
 * dos rects se tocan o se solapan aunque sea un pixel, asi que arrastrar una
 * ventana -- danio = marco viejo U marco nuevo -- repintaba y presentaba el
 * rectangulo que contiene a los dos en vez del anillo que de verdad cambio. */
struct windowd_dirty_rect
{
    struct sx_region region;
};

void windowd_set_backbuffer(uint32_t *pixels);
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

unsigned long windowd_current_clock_stamp(char *buffer);
/* Validates the region primitives the compositor relies on: the
 * sx_rect_set_subtract_rect occlusion culling and the exactness of the dirty
 * region it accumulates damage into. Returns 0 on success, non-zero on
 * failure. */
int windowd_region_selftest(void);
/* Compone el frame: fondo (del cliente shellui, o dibujado aca como fallback),
 * superficies de clientes, Task List y cursor. Sin estado de chrome: con el
 * chrome Win95 retirado (A2.4c) todo lo que compone windowd es del WM. */
void windowd_draw_desktop(
    struct windowd_session *session,
    int cursor_x,
    int cursor_y,
    const struct windowd_dirty_rect *dirty);
