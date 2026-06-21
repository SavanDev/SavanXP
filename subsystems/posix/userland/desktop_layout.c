#include "libc.h"
#include "desktop_menu.h"
#include "desktop_layout.h"
#include "cursor_asset.h"
#include "shared/version.h"

int desktop_clamp_int(int value, int minimum, int maximum)
{
    if (value < minimum)
    {
        return minimum;
    }
    if (value > maximum)
    {
        return maximum;
    }
    return value;
}

int desktop_point_in_rect(int x, int y, int rect_x, int rect_y, int rect_w, int rect_h)
{
    return x >= rect_x && y >= rect_y && x < rect_x + rect_w && y < rect_y + rect_h;
}

int desktop_rects_intersect(int left_x, int left_y, int left_w, int left_h, int right_x, int right_y, int right_w, int right_h)
{
    return left_x < right_x + right_w && right_x < left_x + left_w &&
           left_y < right_y + right_h && right_y < left_y + left_h;
}

void desktop_work_area_bounds(const struct savanxp_fb_info *info, int *x, int *y, int *width, int *height)
{
    if (info == 0 || x == 0 || y == 0 || width == 0 || height == 0)
    {
        return;
    }
    *x = 0;
    *y = 0;
    *width = (int)info->width;
    *height = (int)info->height > DESKTOP_TASKBAR_HEIGHT ? (int)info->height - DESKTOP_TASKBAR_HEIGHT : (int)info->height;
}

void desktop_fill_shell_surface_info(const struct savanxp_fb_info *display_info, struct savanxp_fb_info *client_info)
{
    int area_x = 0;
    int area_y = 0;
    int area_width = 0;
    int area_height = 0;

    if (display_info == 0 || client_info == 0)
    {
        return;
    }

    desktop_work_area_bounds(display_info, &area_x, &area_y, &area_width, &area_height);
    *client_info = *display_info;
    client_info->width = (uint32_t)area_width;
    client_info->height = (uint32_t)area_height;
    client_info->pitch = (uint32_t)(area_width * (int)sizeof(uint32_t));
    client_info->buffer_size = client_info->pitch * client_info->height;
}

void desktop_fill_overlay_surface_info(const struct savanxp_fb_info *display_info, struct savanxp_fb_info *client_info)
{
    int area_x = 0;
    int area_y = 0;
    int area_width = 0;
    int area_height = 0;
    int target_width = 0;
    int target_height = 0;

    if (display_info == 0 || client_info == 0)
    {
        return;
    }

    desktop_work_area_bounds(display_info, &area_x, &area_y, &area_width, &area_height);
    target_width = area_width - 160;
    target_height = area_height - 140;
    if (target_width > 1280)
    {
        target_width = 1280;
    }
    if (target_height > 900)
    {
        target_height = 900;
    }
    if (target_width < 480)
    {
        target_width = area_width > 480 ? 480 : area_width;
    }
    if (target_height < 320)
    {
        target_height = area_height > 320 ? 320 : area_height;
    }

    memset(client_info, 0, sizeof(*client_info));
    client_info->width = (uint32_t)target_width;
    client_info->height = (uint32_t)target_height;
    client_info->pitch = (uint32_t)(area_width * (int)sizeof(uint32_t));
    client_info->bpp = 32u;
    client_info->buffer_size = client_info->pitch * (uint32_t)area_height;
}

void desktop_center_overlay_window(const struct savanxp_fb_info *display_info, const struct savanxp_fb_info *surface_info, int *x, int *y, int *width, int *height)
{
    int area_x = 0;
    int area_y = 0;
    int area_width = 0;
    int area_height = 0;
    int frame_width = 0;
    int frame_height = 0;

    if (display_info == 0 || surface_info == 0 || x == 0 || y == 0 || width == 0 || height == 0)
    {
        return;
    }

    desktop_work_area_bounds(display_info, &area_x, &area_y, &area_width, &area_height);
    frame_width = (int)surface_info->width + (DESKTOP_WINDOW_BORDER * 2);
    frame_height = (int)surface_info->height + DESKTOP_WINDOW_TITLEBAR_HEIGHT + DESKTOP_WINDOW_BORDER;
    *x = area_x + ((area_width - frame_width) / 2);
    *y = area_y + ((area_height - frame_height) / 2);
    if (*x < area_x + 24)
    {
        *x = area_x + 24;
    }
    if (*y < area_y + 24)
    {
        *y = area_y + 24;
    }
    *width = frame_width;
    *height = frame_height;
}

void desktop_place_overlay_window(const struct savanxp_fb_info *display_info, const struct savanxp_fb_info *surface_info, int cascade_index, int *x, int *y, int *width, int *height)
{
    int area_x = 0;
    int area_y = 0;
    int area_width = 0;
    int area_height = 0;
    int offset_x = 0;
    int offset_y = 0;

    desktop_center_overlay_window(display_info, surface_info, x, y, width, height);
    if (display_info == 0 || surface_info == 0 || x == 0 || y == 0 || width == 0 || height == 0)
    {
        return;
    }

    desktop_work_area_bounds(display_info, &area_x, &area_y, &area_width, &area_height);
    if (cascade_index < 0)
    {
        cascade_index = 0;
    }

    offset_x = (cascade_index % DESKTOP_MAX_OVERLAY_CLIENTS) * 28;
    offset_y = (cascade_index % DESKTOP_MAX_OVERLAY_CLIENTS) * 24;
    *x += offset_x;
    *y += offset_y;

    if (*x + *width > area_x + area_width - 16)
    {
        *x = area_x + area_width - *width - 16;
    }
    if (*y + *height > area_y + area_height - 16)
    {
        *y = area_y + area_height - *height - 16;
    }
    if (*x < area_x + 12)
    {
        *x = area_x + 12;
    }
    if (*y < area_y + 12)
    {
        *y = area_y + 12;
    }
}

struct sx_rect desktop_client_surface_rect(const struct desktop_client *client)
{
    if (client == 0 || client->pid <= 0)
    {
        return sx_rect_make(0, 0, 0, 0);
    }
    if (!client->frame_visible)
    {
        return sx_rect_make(client->window_x, client->window_y, (int)client->surface_info.width, (int)client->surface_info.height);
    }
    return sx_rect_make(
        client->window_x + DESKTOP_WINDOW_BORDER,
        client->window_y + DESKTOP_WINDOW_TITLEBAR_HEIGHT,
        (int)client->surface_info.width,
        (int)client->surface_info.height);
}

struct sx_rect desktop_client_frame_rect(const struct desktop_client *client)
{
    if (client == 0 || client->pid <= 0)
    {
        return sx_rect_make(0, 0, 0, 0);
    }
    if (!client->frame_visible)
    {
        return desktop_client_surface_rect(client);
    }
    return sx_rect_make(client->window_x, client->window_y, client->window_width, client->window_height);
}

struct sx_rect desktop_client_titlebar_rect(const struct desktop_client *client)
{
    struct sx_rect frame_rect;

    if (client == 0 || client->pid <= 0 || !client->frame_visible)
    {
        return sx_rect_make(0, 0, 0, 0);
    }

    frame_rect = desktop_client_frame_rect(client);
    return sx_rect_make(
        frame_rect.x + 2,
        frame_rect.y + 2,
        frame_rect.width - 4,
        DESKTOP_WINDOW_TITLEBAR_HEIGHT - 4);
}

struct sx_rect desktop_client_minimize_button_rect(const struct desktop_client *client)
{
    struct sx_rect titlebar_rect;
    int button_x;
    int button_y;

    if (client == 0 || client->pid <= 0 || !client->frame_visible)
    {
        return sx_rect_make(0, 0, 0, 0);
    }

    titlebar_rect = desktop_client_titlebar_rect(client);
    if (titlebar_rect.width < (DESKTOP_WINDOW_BUTTON_SIZE * 3) + (DESKTOP_WINDOW_BUTTON_GAP * 2) + 8 ||
        titlebar_rect.height < DESKTOP_WINDOW_BUTTON_SIZE)
    {
        return sx_rect_make(0, 0, 0, 0);
    }

    button_x = titlebar_rect.x + titlebar_rect.width - (DESKTOP_WINDOW_BUTTON_SIZE * 3) - (DESKTOP_WINDOW_BUTTON_GAP * 2) - 5;
    button_y = titlebar_rect.y + ((titlebar_rect.height - DESKTOP_WINDOW_BUTTON_SIZE) / 2);
    return sx_rect_make(button_x, button_y, DESKTOP_WINDOW_BUTTON_SIZE, DESKTOP_WINDOW_BUTTON_SIZE);
}

struct sx_rect desktop_client_maximize_button_rect(const struct desktop_client *client)
{
    struct sx_rect minimize_rect = desktop_client_minimize_button_rect(client);

    if (sx_rect_is_empty(minimize_rect))
    {
        return sx_rect_make(0, 0, 0, 0);
    }
    return sx_rect_make(
        minimize_rect.x + DESKTOP_WINDOW_BUTTON_SIZE + DESKTOP_WINDOW_BUTTON_GAP,
        minimize_rect.y,
        DESKTOP_WINDOW_BUTTON_SIZE,
        DESKTOP_WINDOW_BUTTON_SIZE);
}

struct sx_rect desktop_client_close_button_rect(const struct desktop_client *client)
{
    struct sx_rect titlebar_rect;
    int button_x;
    int button_y;

    if (client == 0 || client->pid <= 0 || !client->frame_visible)
    {
        return sx_rect_make(0, 0, 0, 0);
    }

    titlebar_rect = desktop_client_titlebar_rect(client);
    if (titlebar_rect.width < DESKTOP_WINDOW_BUTTON_SIZE || titlebar_rect.height < DESKTOP_WINDOW_BUTTON_SIZE)
    {
        return sx_rect_make(0, 0, 0, 0);
    }

    button_x = titlebar_rect.x + titlebar_rect.width - DESKTOP_WINDOW_BUTTON_SIZE - 5;
    button_y = titlebar_rect.y + ((titlebar_rect.height - DESKTOP_WINDOW_BUTTON_SIZE) / 2);
    return sx_rect_make(button_x, button_y, DESKTOP_WINDOW_BUTTON_SIZE, DESKTOP_WINDOW_BUTTON_SIZE);
}

int desktop_point_in_client(const struct desktop_client *client, int x, int y)
{
    struct sx_rect rect = desktop_client_surface_rect(client);
    return sx_rect_contains_point(rect, x, y);
}

int desktop_point_in_frame(const struct desktop_client *client, int x, int y)
{
    struct sx_rect rect = desktop_client_frame_rect(client);
    return sx_rect_contains_point(rect, x, y);
}

int desktop_point_in_titlebar(const struct desktop_client *client, int x, int y)
{
    struct sx_rect rect = desktop_client_titlebar_rect(client);
    return sx_rect_contains_point(rect, x, y);
}

int desktop_point_in_minimize_button(const struct desktop_client *client, int x, int y)
{
    struct sx_rect rect = desktop_client_minimize_button_rect(client);
    return sx_rect_contains_point(rect, x, y);
}

int desktop_point_in_maximize_button(const struct desktop_client *client, int x, int y)
{
    struct sx_rect rect = desktop_client_maximize_button_rect(client);
    return sx_rect_contains_point(rect, x, y);
}

int desktop_point_in_close_button(const struct desktop_client *client, int x, int y)
{
    struct sx_rect rect = desktop_client_close_button_rect(client);
    return sx_rect_contains_point(rect, x, y);
}

void desktop_clamp_overlay_frame_position(const struct savanxp_fb_info *display_info, int frame_width, int frame_height, int *x, int *y)
{
    int area_x = 0;
    int area_y = 0;
    int area_width = 0;
    int area_height = 0;
    int max_x = 0;
    int max_y = 0;

    if (display_info == 0 || x == 0 || y == 0)
    {
        return;
    }

    desktop_work_area_bounds(display_info, &area_x, &area_y, &area_width, &area_height);
    max_x = area_x + area_width - frame_width;
    max_y = area_y + area_height - frame_height;
    if (max_x < area_x)
    {
        max_x = area_x;
    }
    if (max_y < area_y)
    {
        max_y = area_y;
    }

    *x = desktop_clamp_int(*x, area_x, max_x);
    *y = desktop_clamp_int(*y, area_y, max_y);
}

int desktop_taskbar_button_count(const struct desktop_session *session)
{
    int count = 0;
    int index;

    if (session == 0)
    {
        return 0;
    }
    if (session->shell_client.pid > 0)
    {
        count += 1;
    }
    for (index = 0; index < session->overlay_count; ++index)
    {
        int slot = session->overlay_order[index];
        if (slot >= 0 && slot < DESKTOP_MAX_OVERLAY_CLIENTS && session->overlay_clients[slot].pid > 0)
        {
            count += 1;
        }
    }
    return count;
}

const struct desktop_client *desktop_taskbar_button_client(const struct desktop_session *session, int index, int *is_shell, int *slot)
{
    int taskbar_index = 0;
    int order_index;

    if (is_shell != 0)
    {
        *is_shell = 0;
    }
    if (slot != 0)
    {
        *slot = -1;
    }
    if (session == 0 || index < 0)
    {
        return 0;
    }

    if (session->shell_client.pid > 0)
    {
        if (index == 0)
        {
            if (is_shell != 0)
            {
                *is_shell = 1;
            }
            return &session->shell_client;
        }
        taskbar_index = 1;
    }

    for (order_index = 0; order_index < session->overlay_count; ++order_index)
    {
        int overlay_slot = session->overlay_order[order_index];
        if (overlay_slot < 0 || overlay_slot >= DESKTOP_MAX_OVERLAY_CLIENTS || session->overlay_clients[overlay_slot].pid <= 0)
        {
            continue;
        }
        if (taskbar_index == index)
        {
            if (slot != 0)
            {
                *slot = overlay_slot;
            }
            return &session->overlay_clients[overlay_slot];
        }
        taskbar_index += 1;
    }

    return 0;
}

struct sx_rect desktop_taskbar_button_rect(const struct desktop_session *session, int index)
{
    const int taskbar_y = session != 0 ? (int)session->gfx.info.height - DESKTOP_TASKBAR_HEIGHT : 0;
    const int panel_y = taskbar_y + 5;
    const int panel_height = DESKTOP_TASKBAR_HEIGHT - 9;
    const int clock_x = session != 0 ? (int)session->gfx.info.width - DESKTOP_CLOCK_BOX_WIDTH - DESKTOP_TASKBAR_GAP : 0;
    const int version_width = gfx_text_width(SAVANXP_VERSION_STRING) + 16;
    const int version_x = clock_x - version_width - DESKTOP_TASKBAR_GAP;
    const int buttons_x = DESKTOP_START_BUTTON_WIDTH + 12;
    const int buttons_width = version_x - buttons_x - DESKTOP_TASKBAR_GAP;
    const int count = desktop_taskbar_button_count(session);
    int button_width = 0;
    int clamped_width = 0;

    if (session == 0 || index < 0 || index >= count || buttons_width <= 0)
    {
        return sx_rect_make(0, 0, 0, 0);
    }

    button_width = buttons_width / count;
    clamped_width = desktop_clamp_int(button_width, DESKTOP_TASKBAR_BUTTON_MIN_WIDTH, DESKTOP_TASKBAR_BUTTON_MAX_WIDTH);
    if (clamped_width * count > buttons_width)
    {
        clamped_width = button_width > 0 ? button_width : buttons_width;
    }

    return sx_rect_make(buttons_x + (index * clamped_width), panel_y, clamped_width - 4, panel_height);
}

int desktop_taskbar_button_from_point(const struct desktop_session *session, int x, int y)
{
    int index;
    int count = desktop_taskbar_button_count(session);

    for (index = 0; index < count; ++index)
    {
        struct sx_rect rect = desktop_taskbar_button_rect(session, index);
        if (sx_rect_contains_point(rect, x, y))
        {
            return index;
        }
    }
    return -1;
}

int desktop_start_menu_height(void)
{
    return (desktop_menu_item_count() * DESKTOP_MENU_ITEM_HEIGHT) +
           (DESKTOP_MENU_PADDING * 2) +
           DESKTOP_MENU_HEADER_HEIGHT +
           DESKTOP_MENU_FOOTER_HEIGHT +
           16;
}

int desktop_start_menu_content_x(int menu_x)
{
    return menu_x + 6 + DESKTOP_MENU_STRIP_WIDTH + 18;
}

int desktop_start_menu_content_width(void)
{
    return DESKTOP_MENU_WIDTH - (desktop_start_menu_content_x(0) + 14);
}

int desktop_start_menu_items_y(int menu_y)
{
    return menu_y + DESKTOP_MENU_HEADER_HEIGHT + DESKTOP_MENU_PADDING + 10;
}

int desktop_start_menu_footer_y(int menu_y, int menu_height)
{
    return menu_y + menu_height - DESKTOP_MENU_FOOTER_HEIGHT - 10;
}

void desktop_start_menu_bounds(const struct savanxp_fb_info *info, int *x, int *y, int *width, int *height)
{
    if (x == 0 || y == 0 || width == 0 || height == 0 || info == 0)
    {
        return;
    }
    *width = DESKTOP_MENU_WIDTH;
    *height = desktop_start_menu_height();
    *x = 0;
    *y = (int)info->height - DESKTOP_TASKBAR_HEIGHT - *height;
}

struct sx_rect desktop_shortcut_rect(const struct savanxp_fb_info *info, int index)
{
    int area_x = 0;
    int area_y = 0;
    int area_width = 0;
    int area_height = 0;
    int rows_per_column = 1;
    int column = 0;
    int row = 0;

    if (info == 0 || index < 0 || index >= desktop_shortcut_count())
    {
        return sx_rect_make(0, 0, 0, 0);
    }

    desktop_work_area_bounds(info, &area_x, &area_y, &area_width, &area_height);
    rows_per_column = (area_height - 24) / (DESKTOP_SHORTCUT_CELL_HEIGHT + DESKTOP_SHORTCUT_GRID_GAP_Y);
    if (rows_per_column < 1)
    {
        rows_per_column = 1;
    }

    column = index / rows_per_column;
    row = index % rows_per_column;
    return sx_rect_make(
        area_x + 18 + (column * (DESKTOP_SHORTCUT_CELL_WIDTH + DESKTOP_SHORTCUT_GRID_GAP_X)),
        area_y + 18 + (row * (DESKTOP_SHORTCUT_CELL_HEIGHT + DESKTOP_SHORTCUT_GRID_GAP_Y)),
        DESKTOP_SHORTCUT_CELL_WIDTH,
        DESKTOP_SHORTCUT_CELL_HEIGHT);
}

int desktop_shortcut_from_point(const struct savanxp_fb_info *info, int x, int y)
{
    int index;

    for (index = 0; index < desktop_shortcut_count(); ++index)
    {
        struct sx_rect rect = desktop_shortcut_rect(info, index);
        if (sx_rect_contains_point(rect, x, y))
        {
            return index;
        }
    }
    return -1;
}

void desktop_cursor_bounds(int cursor_x, int cursor_y, int *x, int *y, int *width, int *height)
{
    if (x == 0 || y == 0 || width == 0 || height == 0)
    {
        return;
    }
    *x = cursor_x - DESKTOP_CURSOR_HOTSPOT_X;
    *y = cursor_y - DESKTOP_CURSOR_HOTSPOT_Y;
    *width = DESKTOP_CURSOR_WIDTH;
    *height = DESKTOP_CURSOR_HEIGHT;
}

int desktop_selected_item_from_cursor(const struct savanxp_gfx_context *gfx, int cursor_x, int cursor_y)
{
    const int menu_height = desktop_start_menu_height();
    const int menu_y = (int)gfx->info.height - DESKTOP_TASKBAR_HEIGHT - menu_height;
    const int content_x = desktop_start_menu_content_x(0);
    const int content_width = desktop_start_menu_content_width();
    const int items_y = desktop_start_menu_items_y(menu_y);
    int index;

    for (index = 0; index < desktop_menu_item_count(); ++index)
    {
        const int item_y = items_y + (index * DESKTOP_MENU_ITEM_HEIGHT);
        if (desktop_point_in_rect(cursor_x, cursor_y, content_x - 2, item_y, content_width - 4, DESKTOP_MENU_ITEM_HEIGHT - 2))
        {
            return index;
        }
    }
    return -1;
}
