#pragma once

#include "desktop_session.h"
#include "desktop_menu.h"
#include "desktop_shell.h"

struct desktop_dirty_rect
{
    struct sx_rect_set rects;
};

void desktop_set_backbuffer(uint32_t *pixels);
void desktop_dirty_rect_reset(struct desktop_dirty_rect *dirty);
void desktop_dirty_rect_add(struct desktop_dirty_rect *dirty, const struct savanxp_fb_info *info, int x, int y, int width, int height);
void desktop_dirty_rect_add_fullscreen(struct desktop_dirty_rect *dirty, const struct savanxp_fb_info *info);
void desktop_dirty_rect_add_taskbar(struct desktop_dirty_rect *dirty, const struct savanxp_fb_info *info);
void desktop_dirty_rect_add_menu(struct desktop_dirty_rect *dirty, const struct savanxp_fb_info *info);
void desktop_dirty_rect_add_shortcut(struct desktop_dirty_rect *dirty, const struct savanxp_fb_info *info, int shortcut_index);
void desktop_dirty_rect_add_context_menu(struct desktop_dirty_rect *dirty, const struct savanxp_fb_info *info, int menu_x, int menu_y);
void desktop_dirty_rect_add_cursor(struct desktop_dirty_rect *dirty, const struct savanxp_fb_info *info, int cursor_x, int cursor_y, int shape);
void desktop_dirty_rect_add_client(struct desktop_dirty_rect *dirty, const struct desktop_client *client);
int desktop_dirty_rect_valid(const struct desktop_dirty_rect *dirty);
size_t desktop_dirty_rect_count(const struct desktop_dirty_rect *dirty);
const struct sx_rect *desktop_dirty_rect_at(const struct desktop_dirty_rect *dirty, size_t index);

unsigned long desktop_current_clock_stamp(char *buffer);
/* Validates the sx_rect_set_subtract_rect region primitive the compositor
 * relies on for occlusion culling. Returns 0 on success, non-zero on failure. */
int desktop_region_selftest(void);
/* El estado de chrome a dibujar (menu, seleccion, confirm, welcome, menu
 * contextual) viaja en shell_state, igual que en el path de input. shell no
 * debe ser NULL; el menu contextual se dibuja solo si shell->context_menu.open.
 * Los harnesses headless pasan un shell_state armado ad hoc. */
void desktop_draw_desktop(
    struct desktop_session *session,
    int cursor_x,
    int cursor_y,
    const struct shell_state *shell,
    const struct desktop_dirty_rect *dirty);
