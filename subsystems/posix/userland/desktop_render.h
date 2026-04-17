#pragma once

#include "desktop_session.h"

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
void desktop_dirty_rect_add_cursor(struct desktop_dirty_rect *dirty, const struct savanxp_fb_info *info, int cursor_x, int cursor_y);
void desktop_dirty_rect_add_client(struct desktop_dirty_rect *dirty, const struct desktop_client *client);
int desktop_dirty_rect_valid(const struct desktop_dirty_rect *dirty);
size_t desktop_dirty_rect_count(const struct desktop_dirty_rect *dirty);
const struct sx_rect *desktop_dirty_rect_at(const struct desktop_dirty_rect *dirty, size_t index);

unsigned long desktop_current_clock_stamp(char *buffer);
void desktop_draw_desktop(
    struct desktop_session *session,
    int cursor_x,
    int cursor_y,
    int menu_open,
    int selected_index,
    const struct desktop_dirty_rect *dirty);
