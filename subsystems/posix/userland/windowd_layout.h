#pragma once

#include "windowd_session.h"

#define WINDOWD_WINDOW_BORDER 3
#define WINDOWD_WINDOW_TITLEBAR_HEIGHT 26
#define WINDOWD_WINDOW_BUTTON_SIZE 16
#define WINDOWD_WINDOW_BUTTON_GAP 3

/* Redimensionado por bordes.
 *
 * La zona sensible es una franja hacia ADENTRO del marco. Se mantiene fina y se
 * consulta ANTES que el arrastre de la barra de titulo, para que la franja
 * superior redimensione en vez de mover; el resto de la barra sigue moviendo.
 *
 * El minimo tiene que dejar utilizable la barra de titulo: por debajo de eso los
 * botones de minimizar/maximizar/cerrar se pisan entre si. */
#define WINDOWD_RESIZE_BORDER 4
#define WINDOWD_WINDOW_MIN_WIDTH 200
#define WINDOWD_WINDOW_MIN_HEIGHT 140

#define WINDOWD_RESIZE_EDGE_NONE 0u
#define WINDOWD_RESIZE_EDGE_LEFT 1u
#define WINDOWD_RESIZE_EDGE_RIGHT 2u
#define WINDOWD_RESIZE_EDGE_TOP 4u
#define WINDOWD_RESIZE_EDGE_BOTTOM 8u

/* Mascara de bordes bajo el punto, o WINDOWD_RESIZE_EDGE_NONE. Las esquinas
 * devuelven dos bits. Devuelve NONE para ventanas sin marco, maximizadas o en
 * fullscreen: ahi el tamano no lo decide el usuario. */
uint32_t windowd_resize_edge_from_point(const struct windowd_client *client, int x, int y);

int windowd_clamp_int(int value, int minimum, int maximum);
int windowd_point_in_rect(int x, int y, int rect_x, int rect_y, int rect_w, int rect_h);
int windowd_rects_intersect(int left_x, int left_y, int left_w, int left_h, int right_x, int right_y, int right_w, int right_h);

void windowd_work_area_bounds(const struct savanxp_fb_info *info, int *x, int *y, int *width, int *height);

/* Alto de la franja de la barra de tareas, y el rectangulo que ocupa al pie del
 * display. La barra es un CLIENTE (ver savanxp/wm_shell_protocol.h): el WM le
 * dimensiona y posiciona la superficie, pero no dibuja su contenido. */
#define WINDOWD_TASKBAR_HEIGHT 28
int windowd_taskbar_height(void);
struct sx_rect windowd_taskbar_rect(const struct savanxp_fb_info *info);
void windowd_maximize_area_bounds(
    const struct savanxp_fb_info *info,
    int taskbar_present,
    int *x,
    int *y,
    int *width,
    int *height);
void windowd_fill_shell_surface_info(const struct savanxp_fb_info *display_info, struct savanxp_fb_info *client_info);
void windowd_fill_overlay_surface_info(const struct savanxp_fb_info *display_info, struct savanxp_fb_info *client_info);
void windowd_center_overlay_window(const struct savanxp_fb_info *display_info, const struct savanxp_fb_info *surface_info, int *x, int *y, int *width, int *height);
void windowd_place_overlay_window(const struct savanxp_fb_info *display_info, const struct savanxp_fb_info *surface_info, int cascade_index, int *x, int *y, int *width, int *height);

struct sx_rect windowd_client_surface_rect(const struct windowd_client *client);
struct sx_rect windowd_client_frame_rect(const struct windowd_client *client);
struct sx_rect windowd_client_titlebar_rect(const struct windowd_client *client);
struct sx_rect windowd_client_minimize_button_rect(const struct windowd_client *client);
struct sx_rect windowd_client_maximize_button_rect(const struct windowd_client *client);
struct sx_rect windowd_client_close_button_rect(const struct windowd_client *client);
int windowd_point_in_client(const struct windowd_client *client, int x, int y);
int windowd_point_in_frame(const struct windowd_client *client, int x, int y);
int windowd_point_in_titlebar(const struct windowd_client *client, int x, int y);
int windowd_point_in_minimize_button(const struct windowd_client *client, int x, int y);
int windowd_point_in_maximize_button(const struct windowd_client *client, int x, int y);
int windowd_point_in_close_button(const struct windowd_client *client, int x, int y);
void windowd_clamp_overlay_frame_position(const struct savanxp_fb_info *display_info, int frame_width, int frame_height, int *x, int *y);

int windowd_task_count(const struct windowd_session *session);
const struct windowd_client *windowd_task_client(const struct windowd_session *session, int index, int *is_shell, int *slot);
/* Task List (Ctrl+Esc): dialogo del WM con las ventanas abiertas, estilo NT 3.5.
 * Es UI del WM -- no chrome del shell --, igual que los marcos y botones de
 * ventana: windowd es quien tiene la lista de clientes y el z-order. Reemplaza
 * las dos funciones del taskbar (restaurar minimizadas y cambiar de ventana).
 * Botones: 0 = Switch To, 1 = End Task, 2 = Cancel. */
#define WINDOWD_TASKLIST_WIDTH 320
#define WINDOWD_TASKLIST_ITEM_HEIGHT 20
#define WINDOWD_TASKLIST_MAX_VISIBLE 8
#define WINDOWD_TASKLIST_TITLE_HEIGHT 22
#define WINDOWD_TASKLIST_BUTTON_HEIGHT 24
#define WINDOWD_TASKLIST_BUTTON_WIDTH 92
#define WINDOWD_TASKLIST_BUTTON_GAP 8
#define WINDOWD_TASKLIST_PADDING 10
#define WINDOWD_TASKLIST_BUTTON_COUNT 3

/* Cuantos items entran a la vez, y desde cual arranca la ventana visible para
 * que el seleccionado siempre quede a la vista. */
int windowd_tasklist_visible_count(int task_count);
int windowd_tasklist_first_visible(int task_count, int selected);
struct sx_rect windowd_tasklist_rect(const struct savanxp_fb_info *info, int task_count);
struct sx_rect windowd_tasklist_list_rect(const struct savanxp_fb_info *info, int task_count);
/* visible_index es relativo a windowd_tasklist_first_visible(), no absoluto. */
struct sx_rect windowd_tasklist_item_rect(const struct savanxp_fb_info *info, int task_count, int visible_index);
/* Devuelve el indice ABSOLUTO de tarea bajo el punto, o -1. */
int windowd_tasklist_item_from_point(const struct savanxp_fb_info *info, int task_count, int selected, int x, int y);
struct sx_rect windowd_tasklist_button_rect(const struct savanxp_fb_info *info, int task_count, int button_index);
int windowd_tasklist_button_from_point(const struct savanxp_fb_info *info, int task_count, int x, int y);

void windowd_cursor_bounds(int cursor_x, int cursor_y, int shape, int *x, int *y, int *width, int *height);
