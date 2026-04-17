#pragma once

#include "desktop_session.h"

#define DESKTOP_TASKBAR_HEIGHT 40
#define DESKTOP_START_BUTTON_WIDTH 96
#define DESKTOP_CLOCK_BOX_WIDTH 88
#define DESKTOP_MENU_WIDTH 440
#define DESKTOP_MENU_ITEM_HEIGHT 46
#define DESKTOP_MENU_PADDING 16
#define DESKTOP_MENU_HEADER_HEIGHT 84
#define DESKTOP_MENU_STRIP_WIDTH 56
#define DESKTOP_MENU_FOOTER_HEIGHT 44
#define DESKTOP_TASKBAR_GAP 8
#define DESKTOP_WINDOW_BORDER 3
#define DESKTOP_WINDOW_TITLEBAR_HEIGHT 28
#define DESKTOP_WINDOW_BUTTON_SIZE 18

int desktop_clamp_int(int value, int minimum, int maximum);
int desktop_point_in_rect(int x, int y, int rect_x, int rect_y, int rect_w, int rect_h);
int desktop_rects_intersect(int left_x, int left_y, int left_w, int left_h, int right_x, int right_y, int right_w, int right_h);

void desktop_work_area_bounds(const struct savanxp_fb_info *info, int *x, int *y, int *width, int *height);
void desktop_fill_shell_surface_info(const struct savanxp_fb_info *display_info, struct savanxp_fb_info *client_info);
void desktop_fill_overlay_surface_info(const struct savanxp_fb_info *display_info, struct savanxp_fb_info *client_info);
void desktop_center_overlay_window(const struct savanxp_fb_info *display_info, const struct savanxp_fb_info *surface_info, int *x, int *y, int *width, int *height);
void desktop_place_overlay_window(const struct savanxp_fb_info *display_info, const struct savanxp_fb_info *surface_info, int cascade_index, int *x, int *y, int *width, int *height);

struct sx_rect desktop_client_surface_rect(const struct desktop_client *client);
struct sx_rect desktop_client_frame_rect(const struct desktop_client *client);
struct sx_rect desktop_client_titlebar_rect(const struct desktop_client *client);
struct sx_rect desktop_client_close_button_rect(const struct desktop_client *client);
int desktop_point_in_client(const struct desktop_client *client, int x, int y);
int desktop_point_in_frame(const struct desktop_client *client, int x, int y);
int desktop_point_in_titlebar(const struct desktop_client *client, int x, int y);
int desktop_point_in_close_button(const struct desktop_client *client, int x, int y);
void desktop_clamp_overlay_frame_position(const struct savanxp_fb_info *display_info, int frame_width, int frame_height, int *x, int *y);

int desktop_start_menu_height(void);
int desktop_start_menu_content_x(int menu_x);
int desktop_start_menu_content_width(void);
int desktop_start_menu_items_y(int menu_y);
int desktop_start_menu_footer_y(int menu_y, int menu_height);
void desktop_start_menu_bounds(const struct savanxp_fb_info *info, int *x, int *y, int *width, int *height);
void desktop_cursor_bounds(int cursor_x, int cursor_y, int *x, int *y, int *width, int *height);
int desktop_selected_item_from_cursor(const struct savanxp_gfx_context *gfx, int cursor_x, int cursor_y);
