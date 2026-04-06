#include "libc.h"
#include "shared/version.h"
#include "cursor_asset.h"
#include "desktop_menu.h"
#include "desktop_layout.h"
#include "desktop_render.h"

#define DESKTOP_RGB_LITERAL(red, green, blue) (((uint32_t)(red) << 16) | ((uint32_t)(green) << 8) | (uint32_t)(blue))

static uint32_t *g_backbuffer = 0;

void desktop_set_backbuffer(uint32_t *pixels)
{
    g_backbuffer = pixels;
}

static int clip_rect_to_framebuffer(const struct savanxp_fb_info *info, int *x, int *y, int *width, int *height)
{
    if (info == 0 || x == 0 || y == 0 || width == 0 || height == 0)
    {
        return 0;
    }
    if (*width <= 0 || *height <= 0)
    {
        return 0;
    }
    if (*x < 0)
    {
        *width += *x;
        *x = 0;
    }
    if (*y < 0)
    {
        *height += *y;
        *y = 0;
    }
    if (*width <= 0 || *height <= 0 || *x >= (int)info->width || *y >= (int)info->height)
    {
        return 0;
    }
    if (*x + *width > (int)info->width)
    {
        *width = (int)info->width - *x;
    }
    if (*y + *height > (int)info->height)
    {
        *height = (int)info->height - *y;
    }
    return *width > 0 && *height > 0;
}

void desktop_dirty_rect_reset(struct desktop_dirty_rect *dirty)
{
    if (dirty != 0)
    {
        memset(dirty, 0, sizeof(*dirty));
    }
}

void desktop_dirty_rect_add(struct desktop_dirty_rect *dirty, const struct savanxp_fb_info *info, int x, int y, int width, int height)
{
    int right;
    int bottom;

    if (dirty == 0 || !clip_rect_to_framebuffer(info, &x, &y, &width, &height))
    {
        return;
    }

    if (!dirty->valid)
    {
        dirty->valid = 1;
        dirty->x = x;
        dirty->y = y;
        dirty->width = width;
        dirty->height = height;
        return;
    }

    right = dirty->x + dirty->width;
    bottom = dirty->y + dirty->height;
    if (x < dirty->x)
    {
        dirty->x = x;
    }
    if (y < dirty->y)
    {
        dirty->y = y;
    }
    if (x + width > right)
    {
        right = x + width;
    }
    if (y + height > bottom)
    {
        bottom = y + height;
    }
    dirty->width = right - dirty->x;
    dirty->height = bottom - dirty->y;
}

void desktop_dirty_rect_add_fullscreen(struct desktop_dirty_rect *dirty, const struct savanxp_fb_info *info)
{
    if (info != 0)
    {
        desktop_dirty_rect_add(dirty, info, 0, 0, (int)info->width, (int)info->height);
    }
}

void desktop_dirty_rect_add_taskbar(struct desktop_dirty_rect *dirty, const struct savanxp_fb_info *info)
{
    if (info != 0)
    {
        desktop_dirty_rect_add(dirty, info, 0, (int)info->height - DESKTOP_TASKBAR_HEIGHT, (int)info->width, DESKTOP_TASKBAR_HEIGHT);
    }
}

void desktop_dirty_rect_add_menu(struct desktop_dirty_rect *dirty, const struct savanxp_fb_info *info)
{
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;

    desktop_start_menu_bounds(info, &x, &y, &width, &height);
    desktop_dirty_rect_add(dirty, info, x, y, width, height);
}

void desktop_dirty_rect_add_cursor(struct desktop_dirty_rect *dirty, const struct savanxp_fb_info *info, int cursor_x, int cursor_y)
{
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;

    desktop_cursor_bounds(cursor_x, cursor_y, &x, &y, &width, &height);
    desktop_dirty_rect_add(dirty, info, x, y, width, height);
}

static void format_clock_text(char *buffer, unsigned int hours, unsigned int minutes)
{
    buffer[0] = (char)('0' + (hours / 10u));
    buffer[1] = (char)('0' + (hours % 10u));
    buffer[2] = ':';
    buffer[3] = (char)('0' + (minutes / 10u));
    buffer[4] = (char)('0' + (minutes % 10u));
    buffer[5] = '\0';
}

unsigned long desktop_current_clock_stamp(char *buffer)
{
    struct savanxp_realtime now = {0};

    if (realtime(&now) == 0 && now.valid != 0)
    {
        format_clock_text(buffer, (unsigned int)now.hour, (unsigned int)now.minute);
        return ((unsigned long)now.hour * 60UL) + (unsigned long)now.minute;
    }

    {
        unsigned long total_minutes = uptime_ms() / 60000UL;
        unsigned int hours = (unsigned int)((total_minutes / 60UL) % 24UL);
        unsigned int minutes = (unsigned int)(total_minutes % 60UL);
        format_clock_text(buffer, hours, minutes);
        return total_minutes % (24UL * 60UL);
    }
}

static const struct desktop_menu_item *active_client_menu_item(const struct desktop_session *session)
{
    return session != 0 ? desktop_find_menu_item_by_path(session->client.path) : 0;
}

static const char *active_client_label(const struct desktop_session *session)
{
    const struct desktop_menu_item *item = active_client_menu_item(session);
    return item != 0 ? item->label : (session != 0 && session->client.path != 0 ? "App" : "Desktop");
}

static uint32_t active_client_accent(const struct desktop_session *session)
{
    const struct desktop_menu_item *item = active_client_menu_item(session);
    return item != 0 ? item->accent : DESKTOP_RGB_LITERAL(40, 108, 182);
}

static void draw_button(struct savanxp_gfx_context *gfx, int x, int y, int width, int height, uint32_t face, int pressed)
{
    const uint32_t shadow = gfx_rgb(88, 88, 88);
    const uint32_t dark = gfx_rgb(48, 48, 48);
    const uint32_t light = gfx_rgb(255, 255, 255);
    const uint32_t highlight = gfx_rgb(223, 223, 223);

    gfx_rect(g_backbuffer, &gfx->info, x, y, width, height, face);
    if (!pressed)
    {
        gfx_hline(g_backbuffer, &gfx->info, x, y, width, light);
        gfx_vline(g_backbuffer, &gfx->info, x, y, height, light);
        gfx_hline(g_backbuffer, &gfx->info, x + 1, y + 1, width - 2, highlight);
        gfx_vline(g_backbuffer, &gfx->info, x + 1, y + 1, height - 2, highlight);
        gfx_hline(g_backbuffer, &gfx->info, x, y + height - 1, width, dark);
        gfx_vline(g_backbuffer, &gfx->info, x + width - 1, y, height, dark);
        gfx_hline(g_backbuffer, &gfx->info, x + 1, y + height - 2, width - 2, shadow);
        gfx_vline(g_backbuffer, &gfx->info, x + width - 2, y + 1, height - 2, shadow);
    }
    else
    {
        gfx_hline(g_backbuffer, &gfx->info, x, y, width, dark);
        gfx_vline(g_backbuffer, &gfx->info, x, y, height, dark);
        gfx_hline(g_backbuffer, &gfx->info, x + 1, y + 1, width - 2, shadow);
        gfx_vline(g_backbuffer, &gfx->info, x + 1, y + 1, height - 2, shadow);
        gfx_hline(g_backbuffer, &gfx->info, x, y + height - 1, width, light);
        gfx_vline(g_backbuffer, &gfx->info, x + width - 1, y, height, light);
        gfx_hline(g_backbuffer, &gfx->info, x + 1, y + height - 2, width - 2, highlight);
        gfx_vline(g_backbuffer, &gfx->info, x + width - 2, y + 1, height - 2, highlight);
    }
}

static void draw_inset_box(struct savanxp_gfx_context *gfx, int x, int y, int width, int height, uint32_t face)
{
    const uint32_t shadow = gfx_rgb(88, 88, 88);
    const uint32_t dark = gfx_rgb(48, 48, 48);
    const uint32_t light = gfx_rgb(255, 255, 255);
    const uint32_t highlight = gfx_rgb(223, 223, 223);

    gfx_rect(g_backbuffer, &gfx->info, x, y, width, height, face);
    gfx_hline(g_backbuffer, &gfx->info, x, y, width, shadow);
    gfx_vline(g_backbuffer, &gfx->info, x, y, height, shadow);
    gfx_hline(g_backbuffer, &gfx->info, x + 1, y + 1, width - 2, dark);
    gfx_vline(g_backbuffer, &gfx->info, x + 1, y + 1, height - 2, dark);
    gfx_hline(g_backbuffer, &gfx->info, x, y + height - 1, width, light);
    gfx_vline(g_backbuffer, &gfx->info, x + width - 1, y, height, light);
    gfx_hline(g_backbuffer, &gfx->info, x + 1, y + height - 2, width - 2, highlight);
    gfx_vline(g_backbuffer, &gfx->info, x + width - 2, y + 1, height - 2, highlight);
}

static void draw_embossed_text(struct savanxp_gfx_context *gfx, int x, int y, const char *text, uint32_t light, uint32_t dark)
{
    gfx_blit_text(g_backbuffer, &gfx->info, x + 1, y + 1, text, light);
    gfx_blit_text(g_backbuffer, &gfx->info, x, y, text, dark);
}

static void draw_vertical_text(struct savanxp_gfx_context *gfx, int x, int y, const char *text, uint32_t colour, int step)
{
    char glyph[2] = {0, 0};
    int cursor_y = y;

    if (gfx == 0 || text == 0)
    {
        return;
    }

    if (step <= 0)
    {
        step = gfx_text_height();
    }

    while (*text != '\0')
    {
        if (*text == ' ')
        {
            cursor_y += step / 2;
            ++text;
            continue;
        }

        glyph[0] = *text++;
        gfx_blit_text(g_backbuffer, &gfx->info, x, cursor_y, glyph, colour);
        cursor_y += step;
    }
}

static void draw_background(struct savanxp_gfx_context *gfx)
{
    const char *watermark = SAVANXP_DISPLAY_NAME;
    const int text_width = gfx_text_width(watermark);
    const int text_height = gfx_text_height();
    const int text_x = (int)gfx->info.width - text_width - 14;
    const int text_y = (int)gfx->info.height - DESKTOP_TASKBAR_HEIGHT - text_height - 14;

    gfx_clear(g_backbuffer, &gfx->info, gfx_rgb(0, 128, 128));
    gfx_blit_text(g_backbuffer, &gfx->info, text_x + 1, text_y + 1, watermark, gfx_rgb(0, 64, 64));
    gfx_blit_text(g_backbuffer, &gfx->info, text_x, text_y, watermark, gfx_rgb(210, 244, 244));
}

static void draw_taskbar(struct desktop_session *session, int menu_open)
{
    struct savanxp_gfx_context *gfx = &session->gfx;
    const int taskbar_y = (int)gfx->info.height - DESKTOP_TASKBAR_HEIGHT;
    const int panel_y = taskbar_y + 6;
    const int panel_height = DESKTOP_TASKBAR_HEIGHT - 12;
    const char *client_label = active_client_label(session);
    const char *version_text = SAVANXP_VERSION_STRING;
    const uint32_t accent = active_client_accent(session);
    const int clock_x = (int)gfx->info.width - DESKTOP_CLOCK_BOX_WIDTH - DESKTOP_TASKBAR_GAP;
    const int version_width = gfx_text_width(version_text) + 22;
    const int version_x = clock_x - version_width - DESKTOP_TASKBAR_GAP;
    const int status_x = DESKTOP_START_BUTTON_WIDTH + 12;
    const int status_width = version_x - status_x - DESKTOP_TASKBAR_GAP;
    char clock_text[6];
    int clock_text_x;
    int clock_text_y;
    int status_text_x;

    gfx_rect(g_backbuffer, &gfx->info, 0, taskbar_y, (int)gfx->info.width, DESKTOP_TASKBAR_HEIGHT, gfx_rgb(190, 194, 200));
    gfx_hline(g_backbuffer, &gfx->info, 0, taskbar_y, (int)gfx->info.width, gfx_rgb(255, 255, 255));
    gfx_hline(g_backbuffer, &gfx->info, 0, taskbar_y + 1, (int)gfx->info.width, gfx_rgb(232, 235, 239));
    gfx_hline(g_backbuffer, &gfx->info, 0, taskbar_y + 2, (int)gfx->info.width, gfx_rgb(166, 170, 176));
    gfx_hline(g_backbuffer, &gfx->info, 0, taskbar_y + DESKTOP_TASKBAR_HEIGHT - 1, (int)gfx->info.width, gfx_rgb(79, 83, 89));

    draw_button(gfx, 6, panel_y, DESKTOP_START_BUTTON_WIDTH, panel_height, gfx_rgb(196, 199, 203), menu_open);
    gfx_rect(g_backbuffer, &gfx->info, 18 + (menu_open ? 1 : 0), panel_y + 5 + (menu_open ? 1 : 0), 12, 12, gfx_rgb(255, 255, 255));
    gfx_rect(g_backbuffer, &gfx->info, 20 + (menu_open ? 1 : 0), panel_y + 7 + (menu_open ? 1 : 0), 8, 8, gfx_rgb(0, 106, 72));
    draw_embossed_text(
        gfx,
        38 + (menu_open ? 1 : 0),
        taskbar_y + 12 + (menu_open ? 1 : 0),
        "Start",
        gfx_rgb(255, 255, 255),
        gfx_rgb(0, 0, 0));

    if (status_width > 96)
    {
        draw_button(gfx, status_x, panel_y, status_width, panel_height, gfx_rgb(196, 199, 203), 0);
        gfx_rect(g_backbuffer, &gfx->info, status_x + 8, panel_y + 5, 12, 12, accent);
        gfx_frame(g_backbuffer, &gfx->info, status_x + 7, panel_y + 4, 14, 14, gfx_rgb(255, 255, 255));
        status_text_x = status_x + 30;
        draw_embossed_text(gfx, status_text_x, taskbar_y + 12, client_label, gfx_rgb(255, 255, 255), gfx_rgb(0, 0, 0));
    }

    draw_inset_box(gfx, version_x, panel_y, version_width, panel_height, gfx_rgb(196, 199, 203));
    gfx_blit_text(
        g_backbuffer,
        &gfx->info,
        version_x + ((version_width - gfx_text_width(version_text)) / 2),
        panel_y + ((panel_height - gfx_text_height()) / 2),
        version_text,
        gfx_rgb(52, 67, 82));

    draw_inset_box(gfx, clock_x, panel_y, DESKTOP_CLOCK_BOX_WIDTH, panel_height, gfx_rgb(196, 199, 203));
    (void)desktop_current_clock_stamp(clock_text);
    clock_text_x = clock_x + ((DESKTOP_CLOCK_BOX_WIDTH - gfx_text_width(clock_text)) / 2);
    clock_text_y = panel_y + ((panel_height - gfx_text_height()) / 2);
    gfx_blit_text(g_backbuffer, &gfx->info, clock_text_x, clock_text_y, clock_text, gfx_rgb(0, 0, 0));
}

static void draw_start_menu(struct savanxp_gfx_context *gfx, int selected_index)
{
    const int menu_x = 0;
    const int menu_height = desktop_start_menu_height();
    const int menu_y = (int)gfx->info.height - DESKTOP_TASKBAR_HEIGHT - menu_height;
    const int strip_x = menu_x + 6;
    const int strip_inner_x = strip_x + 4;
    const int strip_inner_y = menu_y + 16;
    const int strip_center_x = strip_x + (DESKTOP_MENU_STRIP_WIDTH / 2);
    const int content_x = desktop_start_menu_content_x(menu_x);
    const int content_width = desktop_start_menu_content_width();
    const int header_y = menu_y + 8;
    const int items_y = desktop_start_menu_items_y(menu_y);
    const int footer_y = desktop_start_menu_footer_y(menu_y, menu_height);
    const struct desktop_menu_item *selected_item = desktop_menu_item_at(selected_index >= 0 ? selected_index : 0);
    int index;

    draw_button(gfx, menu_x, menu_y, DESKTOP_MENU_WIDTH, menu_height, gfx_rgb(196, 199, 203), 0);
    gfx_rect(g_backbuffer, &gfx->info, strip_x, menu_y + 6, DESKTOP_MENU_STRIP_WIDTH, menu_height - 12, gfx_rgb(8, 88, 150));
    gfx_hline(g_backbuffer, &gfx->info, strip_x, menu_y + 6, DESKTOP_MENU_STRIP_WIDTH, gfx_rgb(80, 152, 216));
    gfx_rect(g_backbuffer, &gfx->info, strip_inner_x, strip_inner_y, DESKTOP_MENU_STRIP_WIDTH - 8, 20, gfx_rgb(0, 124, 96));
    gfx_blit_text(g_backbuffer, &gfx->info, strip_inner_x + 4, strip_inner_y + 5, "S", gfx_rgb(255, 255, 255));
    draw_vertical_text(
        gfx,
        strip_center_x - (gfx_text_width("S") / 2),
        strip_inner_y + 34,
        SAVANXP_SYSTEM_NAME,
        gfx_rgb(255, 255, 255),
        gfx_text_height() + 2);
    draw_vertical_text(
        gfx,
        strip_center_x - (gfx_text_width("0") / 2),
        menu_y + menu_height - 84,
        SAVANXP_VERSION_STRING,
        gfx_rgb(214, 233, 248),
        gfx_text_height());

    gfx_rect(g_backbuffer, &gfx->info, content_x - 4, header_y, content_width + 4, DESKTOP_MENU_HEADER_HEIGHT - 10, gfx_rgb(234, 233, 227));
    gfx_blit_text(g_backbuffer, &gfx->info, content_x + 4, header_y + 12, SAVANXP_SYSTEM_NAME, gfx_rgb(0, 0, 0));
    gfx_blit_text(g_backbuffer, &gfx->info, content_x + 4, header_y + 30, "Apps and diagnostics", gfx_rgb(70, 84, 99));
    gfx_hline(g_backbuffer, &gfx->info, content_x - 4, header_y + DESKTOP_MENU_HEADER_HEIGHT - 10, content_width + 4, gfx_rgb(172, 172, 172));

    for (index = 0; index < desktop_menu_item_count(); ++index)
    {
        const struct desktop_menu_item *item = desktop_menu_item_at(index);
        const int item_y = items_y + (index * DESKTOP_MENU_ITEM_HEIGHT);
        const int icon_x = content_x + 4;
        const int text_x = content_x + 38;
        const int subtitle_y = item_y + 24;
        const int arrow_x = content_x + content_width - 18;
        const uint32_t icon_color = item != 0 ? item->accent : gfx_rgb(0, 124, 96);

        if (index == selected_index)
        {
            gfx_rect(g_backbuffer, &gfx->info, content_x - 2, item_y, content_width - 4, DESKTOP_MENU_ITEM_HEIGHT - 2, gfx_rgb(13, 46, 120));
            gfx_hline(g_backbuffer, &gfx->info, content_x - 2, item_y, content_width - 4, gfx_rgb(60, 127, 224));
            gfx_rect(g_backbuffer, &gfx->info, icon_x, item_y + 10, 18, 18, gfx_rgb(255, 255, 255));
            gfx_rect(g_backbuffer, &gfx->info, icon_x + 2, item_y + 12, 14, 14, icon_color);
            gfx_blit_text(g_backbuffer, &gfx->info, text_x, item_y + 10, item->label, gfx_rgb(255, 255, 255));
            gfx_blit_text(g_backbuffer, &gfx->info, text_x, subtitle_y, item->subtitle, gfx_rgb(206, 226, 250));
            gfx_blit_text(g_backbuffer, &gfx->info, arrow_x, item_y + 12, ">", gfx_rgb(255, 255, 255));
        }
        else
        {
            gfx_hline(g_backbuffer, &gfx->info, content_x - 2, item_y + DESKTOP_MENU_ITEM_HEIGHT - 2, content_width - 4, gfx_rgb(210, 210, 210));
            gfx_rect(g_backbuffer, &gfx->info, icon_x, item_y + 10, 18, 18, gfx_rgb(255, 255, 255));
            gfx_rect(g_backbuffer, &gfx->info, icon_x + 2, item_y + 12, 14, 14, icon_color);
            gfx_blit_text(g_backbuffer, &gfx->info, text_x, item_y + 10, item->label, gfx_rgb(0, 0, 0));
            gfx_blit_text(g_backbuffer, &gfx->info, text_x, subtitle_y, item->subtitle, gfx_rgb(86, 97, 109));
            gfx_blit_text(g_backbuffer, &gfx->info, arrow_x, item_y + 12, ">", gfx_rgb(88, 88, 88));
        }
    }

    draw_inset_box(gfx, content_x - 4, footer_y, content_width + 4, DESKTOP_MENU_FOOTER_HEIGHT, gfx_rgb(196, 199, 203));
    if (selected_item != 0)
    {
        gfx_blit_text(g_backbuffer, &gfx->info, content_x + 2, footer_y + 10, selected_item->path, gfx_rgb(0, 0, 0));
    }
    gfx_blit_text(g_backbuffer, &gfx->info, content_x + 2, footer_y + 26, "Enter launches  Esc closes", gfx_rgb(70, 84, 99));
}

static void draw_cursor(struct savanxp_gfx_context *gfx, int x, int y)
{
    const uint32_t stride = gfx->info.pitch / 4u;
    int row;
    int column;

    for (row = 0; row < DESKTOP_CURSOR_HEIGHT; ++row)
    {
        for (column = 0; column < DESKTOP_CURSOR_WIDTH; ++column)
        {
            const unsigned int pixel = k_desktop_cursor_pixels[(row * DESKTOP_CURSOR_WIDTH) + column];
            const unsigned int alpha = (pixel >> 24) & 0xffu;
            const int draw_x = x + column - DESKTOP_CURSOR_HOTSPOT_X;
            const int draw_y = y + row - DESKTOP_CURSOR_HOTSPOT_Y;
            uint32_t *destination;
            unsigned int destination_pixel;
            unsigned int source_red;
            unsigned int source_green;
            unsigned int source_blue;
            unsigned int destination_red;
            unsigned int destination_green;
            unsigned int destination_blue;

            if ((pixel & 0xff000000u) == 0)
            {
                continue;
            }

            if (draw_x < 0 || draw_y < 0 || draw_x >= (int)gfx->info.width || draw_y >= (int)gfx->info.height)
            {
                continue;
            }

            if (alpha >= 250u)
            {
                gfx_pixel(g_backbuffer, &gfx->info, draw_x, draw_y, pixel & 0x00ffffffu);
                continue;
            }

            destination = &g_backbuffer[(size_t)draw_y * stride + (size_t)draw_x];
            destination_pixel = *destination;
            source_red = (pixel >> 16) & 0xffu;
            source_green = (pixel >> 8) & 0xffu;
            source_blue = pixel & 0xffu;
            destination_red = (destination_pixel >> 16) & 0xffu;
            destination_green = (destination_pixel >> 8) & 0xffu;
            destination_blue = destination_pixel & 0xffu;

            destination_red = ((source_red * alpha) + (destination_red * (255u - alpha))) / 255u;
            destination_green = ((source_green * alpha) + (destination_green * (255u - alpha))) / 255u;
            destination_blue = ((source_blue * alpha) + (destination_blue * (255u - alpha))) / 255u;
            *destination = (destination_red << 16) | (destination_green << 8) | destination_blue;
        }
    }
}

static void copy_pixels_region(
    uint32_t *destination,
    const uint32_t *source,
    const struct savanxp_fb_info *info,
    int x,
    int y,
    int width,
    int height)
{
    const uint32_t stride = gfx_stride_pixels(info);
    int row;

    if (destination == 0 || source == 0 || !clip_rect_to_framebuffer(info, &x, &y, &width, &height))
    {
        return;
    }

    for (row = 0; row < height; ++row)
    {
        memcpy(
            destination + ((size_t)(y + row) * stride) + (size_t)x,
            source + ((size_t)(y + row) * stride) + (size_t)x,
            (size_t)width * sizeof(uint32_t));
    }
}

void desktop_draw_desktop_region(
    struct desktop_session *session,
    int cursor_x,
    int cursor_y,
    int menu_open,
    int selected_index,
    const struct desktop_dirty_rect *dirty)
{
    const struct savanxp_fb_info *client_info = desktop_client_surface_info(session);
    int menu_x = 0;
    int menu_y = 0;
    int menu_width = 0;
    int menu_height = 0;
    int cursor_rect_x = 0;
    int cursor_rect_y = 0;
    int cursor_rect_width = 0;
    int cursor_rect_height = 0;
    const int taskbar_y = (int)session->gfx.info.height - DESKTOP_TASKBAR_HEIGHT;

    if (dirty == 0 || !dirty->valid)
    {
        return;
    }

    if (session->client.pixels != 0 && client_info != 0)
    {
        copy_pixels_region(
            g_backbuffer,
            session->client.pixels,
            client_info,
            dirty->x,
            dirty->y,
            dirty->width,
            dirty->height);
    }
    else
    {
        draw_background(&session->gfx);
    }

    if (desktop_rects_intersect(dirty->x, dirty->y, dirty->width, dirty->height, 0, taskbar_y, (int)session->gfx.info.width, DESKTOP_TASKBAR_HEIGHT))
    {
        draw_taskbar(session, menu_open);
    }

    desktop_start_menu_bounds(&session->gfx.info, &menu_x, &menu_y, &menu_width, &menu_height);
    if (menu_open && desktop_rects_intersect(dirty->x, dirty->y, dirty->width, dirty->height, menu_x, menu_y, menu_width, menu_height))
    {
        draw_start_menu(&session->gfx, selected_index);
    }

    desktop_cursor_bounds(cursor_x, cursor_y, &cursor_rect_x, &cursor_rect_y, &cursor_rect_width, &cursor_rect_height);
    if (!session->hw_cursor_enabled &&
        desktop_rects_intersect(
            dirty->x,
            dirty->y,
            dirty->width,
            dirty->height,
            cursor_rect_x,
            cursor_rect_y,
            cursor_rect_width,
            cursor_rect_height))
    {
        draw_cursor(&session->gfx, cursor_x, cursor_y);
    }
}
