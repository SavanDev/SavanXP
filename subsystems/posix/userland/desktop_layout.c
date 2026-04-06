#include "libc.h"
#include "desktop_menu.h"
#include "desktop_layout.h"
#include "cursor_asset.h"

int desktop_client_surface_height_for_display(const struct savanxp_fb_info *info)
{
    if (info == 0)
    {
        return 0;
    }
    return (int)info->height > DESKTOP_TASKBAR_HEIGHT ? (int)info->height - DESKTOP_TASKBAR_HEIGHT : (int)info->height;
}

void desktop_fill_client_surface_info(const struct savanxp_fb_info *display_info, struct savanxp_fb_info *client_info)
{
    if (display_info == 0 || client_info == 0)
    {
        return;
    }

    *client_info = *display_info;
    client_info->height = (uint32_t)desktop_client_surface_height_for_display(display_info);
    client_info->buffer_size = client_info->pitch * client_info->height;
}

const struct savanxp_fb_info *desktop_client_surface_info(const struct desktop_session *session)
{
    return session != 0 && session->client.header != 0 ? &session->client.header->info : 0;
}

int desktop_point_in_client_area(const struct desktop_session *session, int x, int y)
{
    const struct savanxp_fb_info *info = desktop_client_surface_info(session);
    if (info == 0)
    {
        return y >= 0 && y < desktop_client_surface_height_for_display(&session->gfx.info) &&
               x >= 0 && x < (int)session->gfx.info.width;
    }
    return x >= 0 && y >= 0 && x < (int)info->width && y < (int)info->height;
}

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
    return menu_x + 6 + DESKTOP_MENU_STRIP_WIDTH + 16;
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
