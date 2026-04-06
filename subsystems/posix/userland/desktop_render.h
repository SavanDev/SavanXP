#pragma once

#include "desktop_session.h"

struct desktop_dirty_rect
{
    int valid;
    int x;
    int y;
    int width;
    int height;
};

void desktop_set_backbuffer(uint32_t *pixels);
void desktop_dirty_rect_reset(struct desktop_dirty_rect *dirty);
void desktop_dirty_rect_add(struct desktop_dirty_rect *dirty, const struct savanxp_fb_info *info, int x, int y, int width, int height);
void desktop_dirty_rect_add_fullscreen(struct desktop_dirty_rect *dirty, const struct savanxp_fb_info *info);
void desktop_dirty_rect_add_taskbar(struct desktop_dirty_rect *dirty, const struct savanxp_fb_info *info);
void desktop_dirty_rect_add_menu(struct desktop_dirty_rect *dirty, const struct savanxp_fb_info *info);
void desktop_dirty_rect_add_cursor(struct desktop_dirty_rect *dirty, const struct savanxp_fb_info *info, int cursor_x, int cursor_y);
unsigned long desktop_current_clock_stamp(char *buffer);
void desktop_draw_desktop_region(
    struct desktop_session *session,
    int cursor_x,
    int cursor_y,
    int menu_open,
    int selected_index,
    const struct desktop_dirty_rect *dirty);
