#pragma once

#include "desktop_session.h"

#define DESKTOP_TASKBAR_HEIGHT 28
#define DESKTOP_START_BUTTON_WIDTH 64
#define DESKTOP_CLOCK_BOX_WIDTH 58
#define DESKTOP_MENU_WIDTH 360
#define DESKTOP_MENU_ITEM_HEIGHT 42
#define DESKTOP_MENU_PADDING 12
#define DESKTOP_MENU_HEADER_HEIGHT 64
#define DESKTOP_MENU_STRIP_WIDTH 44
#define DESKTOP_MENU_FOOTER_HEIGHT 34
#define DESKTOP_TASKBAR_GAP 6
#define DESKTOP_TASKBAR_BUTTON_MIN_WIDTH 58
#define DESKTOP_TASKBAR_BUTTON_MAX_WIDTH 160
#define DESKTOP_WINDOW_BORDER 3
#define DESKTOP_WINDOW_TITLEBAR_HEIGHT 26
#define DESKTOP_WINDOW_BUTTON_SIZE 16
#define DESKTOP_WINDOW_BUTTON_GAP 3
#define DESKTOP_SHORTCUT_CELL_WIDTH 76
#define DESKTOP_SHORTCUT_CELL_HEIGHT 62
#define DESKTOP_SHORTCUT_GRID_GAP_X 10
#define DESKTOP_SHORTCUT_GRID_GAP_Y 10

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
struct sx_rect desktop_client_minimize_button_rect(const struct desktop_client *client);
struct sx_rect desktop_client_maximize_button_rect(const struct desktop_client *client);
struct sx_rect desktop_client_close_button_rect(const struct desktop_client *client);
int desktop_point_in_client(const struct desktop_client *client, int x, int y);
int desktop_point_in_frame(const struct desktop_client *client, int x, int y);
int desktop_point_in_titlebar(const struct desktop_client *client, int x, int y);
int desktop_point_in_minimize_button(const struct desktop_client *client, int x, int y);
int desktop_point_in_maximize_button(const struct desktop_client *client, int x, int y);
int desktop_point_in_close_button(const struct desktop_client *client, int x, int y);
void desktop_clamp_overlay_frame_position(const struct savanxp_fb_info *display_info, int frame_width, int frame_height, int *x, int *y);

int desktop_taskbar_button_count(const struct desktop_session *session);
const struct desktop_client *desktop_taskbar_button_client(const struct desktop_session *session, int index, int *is_shell, int *slot);
struct sx_rect desktop_taskbar_button_rect(const struct desktop_session *session, int index);
int desktop_taskbar_button_from_point(const struct desktop_session *session, int x, int y);

int desktop_start_menu_height(void);
int desktop_start_menu_content_x(int menu_x);
int desktop_start_menu_content_width(void);
int desktop_start_menu_items_y(int menu_y);
int desktop_start_menu_footer_y(int menu_y, int menu_height);
void desktop_start_menu_bounds(const struct savanxp_fb_info *info, int *x, int *y, int *width, int *height);
struct sx_rect desktop_power_button_rect(const struct savanxp_fb_info *info, int index);
int desktop_power_button_from_point(const struct savanxp_fb_info *info, int x, int y);
struct sx_rect desktop_confirm_dialog_rect(const struct savanxp_fb_info *info);
struct sx_rect desktop_confirm_yes_rect(const struct savanxp_fb_info *info);
struct sx_rect desktop_confirm_no_rect(const struct savanxp_fb_info *info);
struct sx_rect desktop_shortcut_rect(const struct savanxp_fb_info *info, int index);
int desktop_shortcut_from_point(const struct savanxp_fb_info *info, int x, int y);
void desktop_cursor_bounds(int cursor_x, int cursor_y, int *x, int *y, int *width, int *height);
int desktop_selected_item_from_cursor(const struct savanxp_gfx_context *gfx, int cursor_x, int cursor_y);
