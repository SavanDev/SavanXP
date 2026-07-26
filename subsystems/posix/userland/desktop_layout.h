#pragma once

#include "desktop_session.h"

#define DESKTOP_WINDOW_BORDER 3
#define DESKTOP_WINDOW_TITLEBAR_HEIGHT 26
#define DESKTOP_WINDOW_BUTTON_SIZE 16
#define DESKTOP_WINDOW_BUTTON_GAP 3

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

int desktop_task_count(const struct desktop_session *session);
const struct desktop_client *desktop_task_client(const struct desktop_session *session, int index, int *is_shell, int *slot);
/* Task List (Ctrl+Esc): dialogo del WM con las ventanas abiertas, estilo NT 3.5.
 * Es UI del WM -- no chrome del shell --, igual que los marcos y botones de
 * ventana: windowd es quien tiene la lista de clientes y el z-order. Reemplaza
 * las dos funciones del taskbar (restaurar minimizadas y cambiar de ventana).
 * Botones: 0 = Switch To, 1 = End Task, 2 = Cancel. */
#define DESKTOP_TASKLIST_WIDTH 320
#define DESKTOP_TASKLIST_ITEM_HEIGHT 20
#define DESKTOP_TASKLIST_MAX_VISIBLE 8
#define DESKTOP_TASKLIST_TITLE_HEIGHT 22
#define DESKTOP_TASKLIST_BUTTON_HEIGHT 24
#define DESKTOP_TASKLIST_BUTTON_WIDTH 92
#define DESKTOP_TASKLIST_BUTTON_GAP 8
#define DESKTOP_TASKLIST_PADDING 10
#define DESKTOP_TASKLIST_BUTTON_COUNT 3

/* Cuantos items entran a la vez, y desde cual arranca la ventana visible para
 * que el seleccionado siempre quede a la vista. */
int desktop_tasklist_visible_count(int task_count);
int desktop_tasklist_first_visible(int task_count, int selected);
struct sx_rect desktop_tasklist_rect(const struct savanxp_fb_info *info, int task_count);
struct sx_rect desktop_tasklist_list_rect(const struct savanxp_fb_info *info, int task_count);
/* visible_index es relativo a desktop_tasklist_first_visible(), no absoluto. */
struct sx_rect desktop_tasklist_item_rect(const struct savanxp_fb_info *info, int task_count, int visible_index);
/* Devuelve el indice ABSOLUTO de tarea bajo el punto, o -1. */
int desktop_tasklist_item_from_point(const struct savanxp_fb_info *info, int task_count, int selected, int x, int y);
struct sx_rect desktop_tasklist_button_rect(const struct savanxp_fb_info *info, int task_count, int button_index);
int desktop_tasklist_button_from_point(const struct savanxp_fb_info *info, int task_count, int x, int y);

void desktop_cursor_bounds(int cursor_x, int cursor_y, int shape, int *x, int *y, int *width, int *height);
