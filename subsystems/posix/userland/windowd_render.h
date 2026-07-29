#pragma once

#include "windowd_session.h"
#include "windowd_appinfo.h"

struct windowd_dirty_rect
{
    struct sx_rect_set rects;
};

void windowd_set_backbuffer(uint32_t *pixels);
void windowd_dirty_rect_reset(struct windowd_dirty_rect *dirty);
void windowd_dirty_rect_add(struct windowd_dirty_rect *dirty, const struct savanxp_fb_info *info, int x, int y, int width, int height);
void windowd_dirty_rect_add_fullscreen(struct windowd_dirty_rect *dirty, const struct savanxp_fb_info *info);
void windowd_dirty_rect_add_cursor(struct windowd_dirty_rect *dirty, const struct savanxp_fb_info *info, int cursor_x, int cursor_y, int shape);
void windowd_dirty_rect_add_client(struct windowd_dirty_rect *dirty, const struct windowd_client *client);
int windowd_dirty_rect_valid(const struct windowd_dirty_rect *dirty);
size_t windowd_dirty_rect_count(const struct windowd_dirty_rect *dirty);
const struct sx_rect *windowd_dirty_rect_at(const struct windowd_dirty_rect *dirty, size_t index);

unsigned long windowd_current_clock_stamp(char *buffer);
/* Validates the sx_rect_set_subtract_rect region primitive the compositor
 * relies on for occlusion culling. Returns 0 on success, non-zero on failure. */
int windowd_region_selftest(void);
/* Compone el frame: fondo (del cliente shellui, o dibujado aca como fallback),
 * superficies de clientes, Task List y cursor. Sin estado de chrome: con el
 * chrome Win95 retirado (A2.4c) todo lo que compone windowd es del WM. */
void windowd_draw_desktop(
    struct windowd_session *session,
    int cursor_x,
    int cursor_y,
    const struct windowd_dirty_rect *dirty);
