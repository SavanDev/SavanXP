#pragma once

#include "desktop_session.h"

#define DESKTOP_TASKBAR_HEIGHT 40
#define DESKTOP_START_BUTTON_WIDTH 96
#define DESKTOP_CLOCK_BOX_WIDTH 88
#define DESKTOP_MENU_WIDTH 392
#define DESKTOP_MENU_ITEM_HEIGHT 46
#define DESKTOP_MENU_PADDING 16
#define DESKTOP_MENU_HEADER_HEIGHT 68
#define DESKTOP_MENU_STRIP_WIDTH 52
#define DESKTOP_MENU_FOOTER_HEIGHT 44
#define DESKTOP_TASKBAR_GAP 8

int desktop_client_surface_height_for_display(const struct savanxp_fb_info *info);
void desktop_fill_client_surface_info(const struct savanxp_fb_info *display_info, struct savanxp_fb_info *client_info);
const struct savanxp_fb_info *desktop_client_surface_info(const struct desktop_session *session);
int desktop_point_in_client_area(const struct desktop_session *session, int x, int y);
int desktop_clamp_int(int value, int minimum, int maximum);
int desktop_point_in_rect(int x, int y, int rect_x, int rect_y, int rect_w, int rect_h);
int desktop_rects_intersect(int left_x, int left_y, int left_w, int left_h, int right_x, int right_y, int right_w, int right_h);
int desktop_start_menu_height(void);
int desktop_start_menu_content_x(int menu_x);
int desktop_start_menu_content_width(void);
int desktop_start_menu_items_y(int menu_y);
int desktop_start_menu_footer_y(int menu_y, int menu_height);
void desktop_start_menu_bounds(const struct savanxp_fb_info *info, int *x, int *y, int *width, int *height);
void desktop_cursor_bounds(int cursor_x, int cursor_y, int *x, int *y, int *width, int *height);
int desktop_selected_item_from_cursor(const struct savanxp_gfx_context *gfx, int cursor_x, int cursor_y);
