#include "libc.h"
#include "shared/version.h"
#include "cursor_asset.h"
#include "desktop_icons.h"
#include "desktop_menu.h"
#include "desktop_layout.h"
#include "desktop_render.h"

#define DESKTOP_RGB_LITERAL(red, green, blue) (((uint32_t)(red) << 16) | ((uint32_t)(green) << 8) | (uint32_t)(blue))

static uint32_t *g_backbuffer = 0;

static const char *window_title_for_client(const struct desktop_client *client);

static void fill_embedded_bitmap_info(const struct desktop_embedded_bitmap *source, struct savanxp_fb_info *info)
{
    if (source == 0 || info == 0)
    {
        return;
    }

    memset(info, 0, sizeof(*info));
    info->width = source->width;
    info->height = source->height;
    info->pitch = source->width * (uint32_t)sizeof(uint32_t);
    info->bpp = 32u;
    info->buffer_size = info->pitch * info->height;
}

static void draw_embedded_bitmap(struct sx_painter *painter, const struct desktop_embedded_bitmap *source, int x, int y)
{
    struct sx_bitmap bitmap;
    struct savanxp_fb_info info;

    if (painter == 0 || source == 0 || source->pixels == 0 || source->width == 0 || source->height == 0)
    {
        return;
    }

    fill_embedded_bitmap_info(source, &info);
    sx_bitmap_wrap(&bitmap, (uint32_t *)source->pixels, &info, SX_PIXEL_FORMAT_BGRA8888);
    sx_painter_blit_bitmap(painter, &bitmap, x, y);
}

static void draw_embedded_bitmap_scaled(struct sx_painter *painter, const struct desktop_embedded_bitmap *source, int x, int y, int width, int height)
{
    struct sx_bitmap bitmap;
    struct savanxp_fb_info info;

    if (painter == 0 || source == 0 || source->pixels == 0 || source->width == 0 || source->height == 0 || width <= 0 || height <= 0)
    {
        return;
    }

    fill_embedded_bitmap_info(source, &info);
    sx_bitmap_wrap(&bitmap, (uint32_t *)source->pixels, &info, SX_PIXEL_FORMAT_BGRA8888);
    sx_painter_draw_scaled_bitmap_nearest(
        painter,
        &bitmap,
        sx_rect_make(x, y, width, height),
        sx_rect_make(0, 0, (int)source->width, (int)source->height));
}

static int clip_rect_to_framebuffer(const struct savanxp_fb_info *info, struct sx_rect *rect)
{
    if (info == 0 || rect == 0 || rect->width <= 0 || rect->height <= 0)
    {
        return 0;
    }
    if (rect->x < 0)
    {
        rect->width += rect->x;
        rect->x = 0;
    }
    if (rect->y < 0)
    {
        rect->height += rect->y;
        rect->y = 0;
    }
    if (rect->width <= 0 || rect->height <= 0 ||
        rect->x >= (int)info->width || rect->y >= (int)info->height)
    {
        return 0;
    }
    if (sx_rect_right(*rect) > (int)info->width)
    {
        rect->width = (int)info->width - rect->x;
    }
    if (sx_rect_bottom(*rect) > (int)info->height)
    {
        rect->height = (int)info->height - rect->y;
    }
    return rect->width > 0 && rect->height > 0;
}

void desktop_set_backbuffer(uint32_t *pixels)
{
    g_backbuffer = pixels;
}

void desktop_dirty_rect_reset(struct desktop_dirty_rect *dirty)
{
    if (dirty != 0)
    {
        sx_rect_set_clear(&dirty->rects);
    }
}

void desktop_dirty_rect_add(struct desktop_dirty_rect *dirty, const struct savanxp_fb_info *info, int x, int y, int width, int height)
{
    struct sx_rect rect = sx_rect_make(x, y, width, height);

    if (dirty == 0 || !clip_rect_to_framebuffer(info, &rect))
    {
        return;
    }
    (void)sx_rect_set_add(&dirty->rects, rect);
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

void desktop_dirty_rect_add_shortcut(struct desktop_dirty_rect *dirty, const struct savanxp_fb_info *info, int shortcut_index)
{
    struct sx_rect rect = desktop_shortcut_rect(info, shortcut_index);

    if (dirty == 0 || info == 0 || sx_rect_is_empty(rect))
    {
        return;
    }
    desktop_dirty_rect_add(dirty, info, rect.x, rect.y, rect.width, rect.height);
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

void desktop_dirty_rect_add_client(struct desktop_dirty_rect *dirty, const struct desktop_client *client)
{
    struct sx_rect rect;

    if (dirty == 0 || client == 0 || client->pid <= 0)
    {
        return;
    }
    rect = client->frame_visible ? desktop_client_frame_rect(client) : desktop_client_surface_rect(client);
    (void)sx_rect_set_add(&dirty->rects, rect);
}

int desktop_dirty_rect_valid(const struct desktop_dirty_rect *dirty)
{
    return dirty != 0 && sx_rect_set_valid(&dirty->rects);
}

size_t desktop_dirty_rect_count(const struct desktop_dirty_rect *dirty)
{
    return dirty != 0 ? dirty->rects.count : 0;
}

const struct sx_rect *desktop_dirty_rect_at(const struct desktop_dirty_rect *dirty, size_t index)
{
    if (dirty == 0 || index >= dirty->rects.count)
    {
        return 0;
    }
    return &dirty->rects.rects[index];
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

static const char *window_title_for_client(const struct desktop_client *client)
{
    const struct desktop_menu_item *item = client != 0 ? desktop_find_menu_item_by_path(client->path) : 0;
    if (item != 0)
    {
        return item->label;
    }
    return client != 0 && client->path != 0 ? client->path : "App";
}

static void draw_button(struct sx_painter *painter, struct sx_rect rect, uint32_t face, int pressed)
{
    const uint32_t shadow = gfx_rgb(88, 88, 88);
    const uint32_t dark = gfx_rgb(48, 48, 48);
    const uint32_t light = gfx_rgb(255, 255, 255);
    const uint32_t highlight = gfx_rgb(223, 223, 223);

    sx_painter_fill_rect(painter, rect, face);
    if (!pressed)
    {
        sx_painter_fill_rect(painter, sx_rect_make(rect.x, rect.y, rect.width, 1), light);
        sx_painter_fill_rect(painter, sx_rect_make(rect.x, rect.y, 1, rect.height), light);
        sx_painter_fill_rect(painter, sx_rect_make(rect.x + 1, rect.y + 1, rect.width - 2, 1), highlight);
        sx_painter_fill_rect(painter, sx_rect_make(rect.x + 1, rect.y + 1, 1, rect.height - 2), highlight);
        sx_painter_fill_rect(painter, sx_rect_make(rect.x, rect.y + rect.height - 1, rect.width, 1), dark);
        sx_painter_fill_rect(painter, sx_rect_make(rect.x + rect.width - 1, rect.y, 1, rect.height), dark);
        sx_painter_fill_rect(painter, sx_rect_make(rect.x + 1, rect.y + rect.height - 2, rect.width - 2, 1), shadow);
        sx_painter_fill_rect(painter, sx_rect_make(rect.x + rect.width - 2, rect.y + 1, 1, rect.height - 2), shadow);
    }
    else
    {
        sx_painter_fill_rect(painter, sx_rect_make(rect.x, rect.y, rect.width, 1), dark);
        sx_painter_fill_rect(painter, sx_rect_make(rect.x, rect.y, 1, rect.height), dark);
        sx_painter_fill_rect(painter, sx_rect_make(rect.x + 1, rect.y + 1, rect.width - 2, 1), shadow);
        sx_painter_fill_rect(painter, sx_rect_make(rect.x + 1, rect.y + 1, 1, rect.height - 2), shadow);
        sx_painter_fill_rect(painter, sx_rect_make(rect.x, rect.y + rect.height - 1, rect.width, 1), light);
        sx_painter_fill_rect(painter, sx_rect_make(rect.x + rect.width - 1, rect.y, 1, rect.height), light);
        sx_painter_fill_rect(painter, sx_rect_make(rect.x + 1, rect.y + rect.height - 2, rect.width - 2, 1), highlight);
        sx_painter_fill_rect(painter, sx_rect_make(rect.x + rect.width - 2, rect.y + 1, 1, rect.height - 2), highlight);
    }
}

static void draw_inset_box(struct sx_painter *painter, struct sx_rect rect, uint32_t face)
{
    const uint32_t shadow = gfx_rgb(88, 88, 88);
    const uint32_t dark = gfx_rgb(48, 48, 48);
    const uint32_t light = gfx_rgb(255, 255, 255);
    const uint32_t highlight = gfx_rgb(223, 223, 223);

    sx_painter_fill_rect(painter, rect, face);
    sx_painter_fill_rect(painter, sx_rect_make(rect.x, rect.y, rect.width, 1), shadow);
    sx_painter_fill_rect(painter, sx_rect_make(rect.x, rect.y, 1, rect.height), shadow);
    sx_painter_fill_rect(painter, sx_rect_make(rect.x + 1, rect.y + 1, rect.width - 2, 1), dark);
    sx_painter_fill_rect(painter, sx_rect_make(rect.x + 1, rect.y + 1, 1, rect.height - 2), dark);
    sx_painter_fill_rect(painter, sx_rect_make(rect.x, rect.y + rect.height - 1, rect.width, 1), light);
    sx_painter_fill_rect(painter, sx_rect_make(rect.x + rect.width - 1, rect.y, 1, rect.height), light);
    sx_painter_fill_rect(painter, sx_rect_make(rect.x + 1, rect.y + rect.height - 2, rect.width - 2, 1), highlight);
    sx_painter_fill_rect(painter, sx_rect_make(rect.x + rect.width - 2, rect.y + 1, 1, rect.height - 2), highlight);
}

static void draw_embossed_text(struct sx_painter *painter, int x, int y, const char *text, uint32_t light, uint32_t dark)
{
    sx_painter_draw_text(painter, x + 1, y + 1, text, light);
    sx_painter_draw_text(painter, x, y, text, dark);
}

static void draw_close_button(struct sx_painter *painter, const struct desktop_client *client)
{
    struct sx_rect rect;
    int inset = 0;
    int glyph_size = 0;
    int index = 0;

    if (painter == 0 || client == 0 || client->pid <= 0 || !client->frame_visible)
    {
        return;
    }

    rect = desktop_client_close_button_rect(client);
    if (rect.width <= 0 || rect.height <= 0)
    {
        return;
    }

    draw_button(painter, rect, gfx_rgb(196, 199, 203), 0);

    inset = rect.width >= 16 ? 4 : 3;
    glyph_size = rect.width - (inset * 2);
    if (glyph_size < 4)
    {
        glyph_size = 4;
    }

    for (index = 0; index < glyph_size; ++index)
    {
        sx_painter_fill_rect(painter, sx_rect_make(rect.x + inset + index, rect.y + inset + index, 1, 1), gfx_rgb(32, 32, 32));
        sx_painter_fill_rect(painter, sx_rect_make(rect.x + rect.width - inset - 1 - index, rect.y + inset + index, 1, 1), gfx_rgb(32, 32, 32));
    }
}

static void draw_minimize_button(struct sx_painter *painter, const struct desktop_client *client)
{
    struct sx_rect rect;

    if (painter == 0 || client == 0 || client->pid <= 0 || !client->frame_visible)
    {
        return;
    }

    rect = desktop_client_minimize_button_rect(client);
    if (rect.width <= 0 || rect.height <= 0)
    {
        return;
    }

    draw_button(painter, rect, gfx_rgb(196, 199, 203), 0);
    sx_painter_fill_rect(
        painter,
        sx_rect_make(rect.x + 4, rect.y + rect.height - 6, rect.width - 8, 2),
        gfx_rgb(32, 32, 32));
}

static void draw_maximize_button(struct sx_painter *painter, const struct desktop_client *client)
{
    struct sx_rect rect;

    if (painter == 0 || client == 0 || client->pid <= 0 || !client->frame_visible)
    {
        return;
    }

    rect = desktop_client_maximize_button_rect(client);
    if (rect.width <= 0 || rect.height <= 0)
    {
        return;
    }

    draw_button(painter, rect, gfx_rgb(196, 199, 203), 0);
    sx_painter_draw_frame(
        painter,
        sx_rect_make(rect.x + 4, rect.y + 4, rect.width - 8, rect.height - 8),
        gfx_rgb(32, 32, 32));
    if (client->maximized)
    {
        sx_painter_draw_frame(
            painter,
            sx_rect_make(rect.x + 6, rect.y + 6, rect.width - 8, rect.height - 8),
            gfx_rgb(32, 32, 32));
    }
}

static void draw_desktop_shortcuts(
    struct sx_painter *painter,
    const struct savanxp_fb_info *display_info,
    int selected_shortcut)
{
    int index;

    for (index = 0; index < desktop_shortcut_count(); ++index)
    {
        const struct desktop_menu_item *item = desktop_shortcut_at(index);
        const struct desktop_embedded_bitmap *icon = item != 0 ? desktop_icon_large(item->icon_id) : 0;
        struct sx_rect rect = desktop_shortcut_rect(display_info, index);
        int label_x = rect.x + ((rect.width - gfx_text_width(item != 0 ? item->label : "")) / 2);

        if (item == 0 || sx_rect_is_empty(rect))
        {
            continue;
        }
        if (index == selected_shortcut)
        {
            sx_painter_fill_rect(painter, rect, gfx_rgb(0, 96, 144));
            sx_painter_draw_frame(painter, rect, gfx_rgb(210, 244, 255));
        }
        draw_embedded_bitmap_scaled(painter, icon, rect.x + ((rect.width - 32) / 2), rect.y + 6, 32, 32);
        sx_painter_draw_text(
            painter,
            label_x,
            rect.y + 42,
            item->label,
            index == selected_shortcut ? gfx_rgb(255, 255, 255) : gfx_rgb(245, 255, 255));
    }
}

static void draw_background(
    struct sx_painter *painter,
    const struct savanxp_fb_info *display_info,
    int selected_shortcut)
{
    const char *watermark = SAVANXP_DISPLAY_NAME;
    const int text_width = gfx_text_width(watermark);
    const int text_height = gfx_text_height();
    const int text_x = (int)display_info->width - text_width - 14;
    const int text_y = (int)display_info->height - DESKTOP_TASKBAR_HEIGHT - text_height - 14;

    sx_painter_fill(painter, gfx_rgb(0, 128, 128));
    draw_desktop_shortcuts(painter, display_info, selected_shortcut);
    sx_painter_draw_text(painter, text_x + 1, text_y + 1, watermark, gfx_rgb(0, 64, 64));
    sx_painter_draw_text(painter, text_x, text_y, watermark, gfx_rgb(210, 244, 244));
}

static void draw_taskbar(struct sx_painter *painter, struct desktop_session *session, int menu_open)
{
    const int taskbar_y = (int)session->gfx.info.height - DESKTOP_TASKBAR_HEIGHT;
    const int panel_y = taskbar_y + 5;
    const int panel_height = DESKTOP_TASKBAR_HEIGHT - 9;
    const int text_y = taskbar_y + (DESKTOP_TASKBAR_HEIGHT - gfx_text_height()) / 2;
    const int icon_y = taskbar_y + (DESKTOP_TASKBAR_HEIGHT - 16) / 2;
    const char *version_text = SAVANXP_VERSION_STRING;
    const int clock_x = (int)session->gfx.info.width - DESKTOP_CLOCK_BOX_WIDTH - DESKTOP_TASKBAR_GAP;
    const int version_width = gfx_text_width(version_text) + 16;
    const int version_x = clock_x - version_width - DESKTOP_TASKBAR_GAP;
    const struct desktop_embedded_bitmap *start_icon = desktop_icon_small(DESKTOP_ICON_DESKTOP);
    char clock_text[6];
    int index;

    sx_painter_fill_rect(painter, sx_rect_make(0, taskbar_y, (int)session->gfx.info.width, DESKTOP_TASKBAR_HEIGHT), gfx_rgb(190, 194, 200));
    sx_painter_fill_rect(painter, sx_rect_make(0, taskbar_y, (int)session->gfx.info.width, 1), gfx_rgb(255, 255, 255));
    sx_painter_fill_rect(painter, sx_rect_make(0, taskbar_y + 1, (int)session->gfx.info.width, 1), gfx_rgb(232, 235, 239));
    sx_painter_fill_rect(painter, sx_rect_make(0, taskbar_y + 2, (int)session->gfx.info.width, 1), gfx_rgb(166, 170, 176));
    sx_painter_fill_rect(painter, sx_rect_make(0, taskbar_y + DESKTOP_TASKBAR_HEIGHT - 1, (int)session->gfx.info.width, 1), gfx_rgb(79, 83, 89));

    draw_button(painter, sx_rect_make(6, panel_y, DESKTOP_START_BUTTON_WIDTH, panel_height), gfx_rgb(196, 199, 203), menu_open);
    draw_embedded_bitmap(painter, start_icon, 12 + (menu_open ? 1 : 0), icon_y + (menu_open ? 1 : 0));
    draw_embossed_text(painter, 32 + (menu_open ? 1 : 0), text_y + (menu_open ? 1 : 0), "Start", gfx_rgb(255, 255, 255), gfx_rgb(0, 0, 0));

    for (index = 0; index < desktop_taskbar_button_count(session); ++index)
    {
        const struct desktop_client *client = 0;
        const struct desktop_menu_item *item = 0;
        const struct desktop_embedded_bitmap *icon = 0;
        struct sx_rect rect = desktop_taskbar_button_rect(session, index);
        int is_shell = 0;
        int slot = -1;
        uint32_t face = gfx_rgb(210, 214, 220);
        uint32_t accent = gfx_rgb(40, 108, 182);
        const char *label = "Desktop";

        client = desktop_taskbar_button_client(session, index, &is_shell, &slot);
        if (client == 0 || sx_rect_is_empty(rect))
        {
            continue;
        }
        item = desktop_find_menu_item_by_path(client->path);
        icon = item != 0 ? desktop_icon_small(item->icon_id) : desktop_icon_small(DESKTOP_ICON_DESKTOP);
        label = item != 0 ? item->label : window_title_for_client(client);
        if (item != 0)
        {
            accent = item->accent;
        }
        if (client->active && !client->minimized)
        {
            face = gfx_rgb(229, 232, 236);
        }
        if (client->minimized)
        {
            face = gfx_rgb(188, 193, 198);
            accent = gfx_rgb(102, 108, 118);
        }

        draw_button(painter, rect, face, client->active && !client->minimized);
        sx_painter_fill_rect(painter, sx_rect_make(rect.x + 5, rect.y + 3, 3, rect.height - 6), accent);
        draw_embedded_bitmap(painter, icon, rect.x + 12, icon_y);
        sx_painter_draw_text(painter, rect.x + 32, text_y, label, gfx_rgb(12, 16, 20));
    }

    draw_inset_box(painter, sx_rect_make(version_x, panel_y, version_width, panel_height), gfx_rgb(210, 214, 220));
    sx_painter_draw_text(painter, version_x + 8, text_y, version_text, gfx_rgb(46, 50, 56));

    desktop_current_clock_stamp(clock_text);
    draw_inset_box(painter, sx_rect_make(clock_x, panel_y, DESKTOP_CLOCK_BOX_WIDTH, panel_height), gfx_rgb(210, 214, 220));
    sx_painter_draw_text(
        painter,
        clock_x + ((DESKTOP_CLOCK_BOX_WIDTH - gfx_text_width(clock_text)) / 2),
        text_y,
        clock_text,
        gfx_rgb(46, 50, 56));
}

static void draw_start_menu(struct sx_painter *painter, struct savanxp_gfx_context *gfx, int selected_index)
{
    int menu_x = 0;
    int menu_y = 0;
    int menu_width = 0;
    int menu_height = 0;
    int content_x = 0;
    int items_y = 0;
    int footer_y = 0;
    const struct desktop_embedded_bitmap *sidebar_art = desktop_menu_strip_art();
    int index;

    desktop_start_menu_bounds(&gfx->info, &menu_x, &menu_y, &menu_width, &menu_height);
    content_x = desktop_start_menu_content_x(menu_x);
    items_y = desktop_start_menu_items_y(menu_y);
    footer_y = desktop_start_menu_footer_y(menu_y, menu_height);

    sx_painter_fill_rect(painter, sx_rect_make(menu_x, menu_y, menu_width, menu_height), gfx_rgb(198, 201, 206));
    sx_painter_draw_frame(painter, sx_rect_make(menu_x, menu_y, menu_width, menu_height), gfx_rgb(46, 50, 56));
    sx_painter_fill_rect(painter, sx_rect_make(menu_x + 6, menu_y + 6, DESKTOP_MENU_STRIP_WIDTH, menu_height - 12), gfx_rgb(0, 106, 72));
    draw_embedded_bitmap_scaled(painter, sidebar_art, menu_x + 6, menu_y + 6, DESKTOP_MENU_STRIP_WIDTH, menu_height - 12);
    sx_painter_draw_text(painter, content_x, menu_y + 8, "Applications", gfx_rgb(24, 28, 34));
    sx_painter_draw_text(painter, content_x, menu_y + 28, "Open an app in its own", gfx_rgb(82, 88, 96));
    sx_painter_draw_text(painter, content_x, menu_y + 46, "movable window", gfx_rgb(82, 88, 96));

    for (index = 0; index < desktop_menu_item_count(); ++index)
    {
        const struct desktop_menu_item *item = desktop_menu_item_at(index);
        const struct desktop_embedded_bitmap *icon = item != 0 ? desktop_icon_large(item->icon_id) : 0;
        const int item_y = items_y + (index * DESKTOP_MENU_ITEM_HEIGHT);
        const uint32_t face = index == selected_index ? gfx_rgb(222, 234, 255) : gfx_rgb(236, 238, 241);

        sx_painter_fill_rect(painter, sx_rect_make(content_x - 2, item_y, desktop_start_menu_content_width() - 4, DESKTOP_MENU_ITEM_HEIGHT - 2), face);
        sx_painter_fill_rect(painter, sx_rect_make(content_x + 4, item_y + 8, 22, 22), gfx_rgb(245, 247, 250));
        sx_painter_fill_rect(painter, sx_rect_make(content_x + 4, item_y + 33, 22, 2), item->accent);
        draw_embedded_bitmap_scaled(painter, icon, content_x + 5, item_y + 9, 20, 20);
        sx_painter_draw_text(painter, content_x + 32, item_y + 4, item->label, gfx_rgb(24, 28, 34));
        sx_painter_draw_text(painter, content_x + 32, item_y + 22, item->subtitle, gfx_rgb(92, 98, 108));
    }

    sx_painter_fill_rect(painter, sx_rect_make(content_x - 2, footer_y, desktop_start_menu_content_width() - 4, 1), gfx_rgb(146, 150, 156));
    sx_painter_draw_text(painter, content_x, footer_y + 8, "Apps open in movable windows", gfx_rgb(82, 88, 96));
}

static void draw_cursor(struct sx_painter *painter, int x, int y)
{
    struct sx_bitmap cursor_bitmap;
    struct savanxp_fb_info info = {
        .width = DESKTOP_CURSOR_WIDTH,
        .height = DESKTOP_CURSOR_HEIGHT,
        .pitch = DESKTOP_CURSOR_WIDTH * sizeof(uint32_t),
        .bpp = 32u,
        .buffer_size = DESKTOP_CURSOR_WIDTH * DESKTOP_CURSOR_HEIGHT * sizeof(uint32_t),
    };
    sx_bitmap_wrap(&cursor_bitmap, (uint32_t *)k_desktop_cursor_pixels, &info, SX_PIXEL_FORMAT_BGRA8888);
    sx_painter_blit_bitmap(painter, &cursor_bitmap, x - DESKTOP_CURSOR_HOTSPOT_X, y - DESKTOP_CURSOR_HOTSPOT_Y);
}

static void draw_client(struct sx_painter *painter, const struct desktop_client *client)
{
    struct sx_bitmap bitmap;
    struct sx_rect surface_rect;
    struct sx_rect frame_rect;
    const struct desktop_menu_item *item = desktop_find_menu_item_by_path(client->path);
    const struct desktop_embedded_bitmap *icon = item != 0 ? desktop_icon_small(item->icon_id) : desktop_icon_small(DESKTOP_ICON_DESKTOP);
    uint32_t title_colour = client != 0 && client->active
        ? (item != 0 ? item->accent : gfx_rgb(59, 95, 156))
        : gfx_rgb(126, 132, 142);
    uint32_t frame_face = client != 0 && client->active
        ? gfx_rgb(208, 212, 219)
        : gfx_rgb(188, 192, 198);

    if (client == 0 || client->pid <= 0 || client->pixels == 0 || client->minimized)
    {
        return;
    }

    surface_rect = desktop_client_surface_rect(client);
    frame_rect = desktop_client_frame_rect(client);

    if (client->frame_visible)
    {
        draw_button(painter, frame_rect, frame_face, 0);
        sx_painter_fill_rect(painter, sx_rect_make(frame_rect.x + 2, frame_rect.y + 2, frame_rect.width - 4, DESKTOP_WINDOW_TITLEBAR_HEIGHT - 4), title_colour);
        draw_embedded_bitmap(painter, icon, frame_rect.x + 6, frame_rect.y + (DESKTOP_WINDOW_TITLEBAR_HEIGHT - 16) / 2);
        sx_painter_draw_text(painter, frame_rect.x + 26, frame_rect.y + (DESKTOP_WINDOW_TITLEBAR_HEIGHT - gfx_text_height()) / 2, window_title_for_client(client), gfx_rgb(255, 255, 255));
        draw_minimize_button(painter, client);
        draw_maximize_button(painter, client);
        draw_close_button(painter, client);
    }

    sx_bitmap_wrap(&bitmap, client->pixels, &client->surface_info, SX_PIXEL_FORMAT_BGRX8888);
    sx_painter_blit_bitmap(painter, &bitmap, surface_rect.x, surface_rect.y);
}

static long rect_set_total_area(const struct sx_rect_set *set)
{
    long area = 0;
    size_t index;

    for (index = 0; index < set->count; ++index)
    {
        if (!sx_rect_is_empty(set->rects[index]))
        {
            area += (long)set->rects[index].width * (long)set->rects[index].height;
        }
    }
    return area;
}

static int rect_set_intersects(const struct sx_rect_set *set, struct sx_rect hole)
{
    size_t index;

    for (index = 0; index < set->count; ++index)
    {
        if (!sx_rect_is_empty(sx_rect_intersect(set->rects[index], hole)))
        {
            return 1;
        }
    }
    return 0;
}

int desktop_region_selftest(void)
{
    struct sx_rect_set set;
    struct sx_rect base = sx_rect_make(0, 0, 100, 100);
    struct sx_rect hole;
    const long base_area = 100 * 100;

    /* Disjoint hole leaves the region untouched. */
    sx_rect_set_clear(&set);
    (void)sx_rect_set_add(&set, base);
    (void)sx_rect_set_subtract_rect(&set, sx_rect_make(200, 200, 50, 50));
    if (rect_set_total_area(&set) != base_area)
    {
        return 1;
    }

    /* A hole that fully contains the rect leaves the region empty. */
    sx_rect_set_clear(&set);
    (void)sx_rect_set_add(&set, sx_rect_make(10, 10, 20, 20));
    (void)sx_rect_set_subtract_rect(&set, sx_rect_make(0, 0, 100, 100));
    if (sx_rect_set_valid(&set) || rect_set_total_area(&set) != 0)
    {
        return 1;
    }

    /* Corner overlap: area shrinks by exactly the overlap, nothing left over
     * intersects the hole. */
    sx_rect_set_clear(&set);
    (void)sx_rect_set_add(&set, base);
    hole = sx_rect_make(50, 50, 100, 100); /* 50x50 = 2500 inside base */
    (void)sx_rect_set_subtract_rect(&set, hole);
    if (rect_set_total_area(&set) != base_area - 2500 || rect_set_intersects(&set, hole))
    {
        return 1;
    }

    /* Centre hole splits the rect into a four-strip ring. */
    sx_rect_set_clear(&set);
    (void)sx_rect_set_add(&set, base);
    hole = sx_rect_make(40, 40, 20, 20); /* 400 in the middle */
    (void)sx_rect_set_subtract_rect(&set, hole);
    if (rect_set_total_area(&set) != base_area - 400 || rect_set_intersects(&set, hole))
    {
        return 1;
    }

    return 0;
}

/* Compositor layers, back-to-front. Each layer carries its screen-space bounds
 * and whether it is fully opaque (a valid occluder for layers behind it). The
 * compose pass paints every layer exactly once over its visible region only:
 * visible = damage ∩ bounds − (union of opaque bounds in front). */
enum desktop_layer_kind
{
    DESKTOP_LAYER_BACKGROUND = 0,
    DESKTOP_LAYER_CLIENT,
    DESKTOP_LAYER_TASKBAR,
    DESKTOP_LAYER_MENU,
    DESKTOP_LAYER_CURSOR,
};

struct desktop_layer
{
    int kind;
    int opaque;
    struct sx_rect bounds;
    const struct desktop_client *client;
};

#define DESKTOP_MAX_COMPOSE_LAYERS (DESKTOP_MAX_OVERLAY_CLIENTS + 5)

static int client_is_drawable(const struct desktop_client *client)
{
    return client != 0 && client->pid > 0 && client->pixels != 0 && !client->minimized;
}

static struct sx_rect client_occluder_rect(const struct desktop_client *client)
{
    return client->frame_visible ? desktop_client_frame_rect(client) : desktop_client_surface_rect(client);
}

static void paint_layer(
    struct sx_painter *painter,
    struct desktop_session *session,
    const struct desktop_layer *layer,
    int menu_open,
    int selected_index,
    int selected_shortcut,
    int cursor_x,
    int cursor_y)
{
    switch (layer->kind)
    {
    case DESKTOP_LAYER_BACKGROUND:
        draw_background(painter, &session->gfx.info, selected_shortcut);
        break;
    case DESKTOP_LAYER_CLIENT:
        draw_client(painter, layer->client);
        break;
    case DESKTOP_LAYER_TASKBAR:
        draw_taskbar(painter, session, menu_open);
        break;
    case DESKTOP_LAYER_MENU:
        draw_start_menu(painter, &session->gfx, selected_index);
        break;
    case DESKTOP_LAYER_CURSOR:
        draw_cursor(painter, cursor_x, cursor_y);
        break;
    default:
        break;
    }
}

/* Back-to-front layer list for the current frame. */
static int build_layers(struct desktop_session *session, int menu_open, int cursor_x, int cursor_y, struct desktop_layer *layers)
{
    const struct savanxp_fb_info *info = &session->gfx.info;
    int count = 0;
    int order_index;
    int menu_x = 0;
    int menu_y = 0;
    int menu_w = 0;
    int menu_h = 0;
    int cur_x = 0;
    int cur_y = 0;
    int cur_w = 0;
    int cur_h = 0;

    layers[count].kind = DESKTOP_LAYER_BACKGROUND;
    layers[count].opaque = 1;
    layers[count].bounds = sx_rect_make(0, 0, (int)info->width, (int)info->height);
    layers[count].client = 0;
    ++count;

    if (client_is_drawable(&session->shell_client))
    {
        layers[count].kind = DESKTOP_LAYER_CLIENT;
        layers[count].opaque = 1;
        layers[count].bounds = client_occluder_rect(&session->shell_client);
        layers[count].client = &session->shell_client;
        ++count;
    }

    for (order_index = 0; order_index < session->overlay_count; ++order_index)
    {
        int slot = session->overlay_order[order_index];
        if (slot < 0 || slot >= DESKTOP_MAX_OVERLAY_CLIENTS)
        {
            continue;
        }
        if (!client_is_drawable(&session->overlay_clients[slot]))
        {
            continue;
        }
        layers[count].kind = DESKTOP_LAYER_CLIENT;
        layers[count].opaque = 1;
        layers[count].bounds = client_occluder_rect(&session->overlay_clients[slot]);
        layers[count].client = &session->overlay_clients[slot];
        ++count;
    }

    layers[count].kind = DESKTOP_LAYER_TASKBAR;
    layers[count].opaque = 1;
    layers[count].bounds = sx_rect_make(0, (int)info->height - DESKTOP_TASKBAR_HEIGHT, (int)info->width, DESKTOP_TASKBAR_HEIGHT);
    layers[count].client = 0;
    ++count;

    if (menu_open)
    {
        desktop_start_menu_bounds(info, &menu_x, &menu_y, &menu_w, &menu_h);
        layers[count].kind = DESKTOP_LAYER_MENU;
        layers[count].opaque = 1;
        layers[count].bounds = sx_rect_make(menu_x, menu_y, menu_w, menu_h);
        layers[count].client = 0;
        ++count;
    }

    if (!session->hw_cursor_enabled)
    {
        desktop_cursor_bounds(cursor_x, cursor_y, &cur_x, &cur_y, &cur_w, &cur_h);
        layers[count].kind = DESKTOP_LAYER_CURSOR;
        layers[count].opaque = 0; /* BGRA cursor blends; never an occluder. */
        layers[count].bounds = sx_rect_make(cur_x, cur_y, cur_w, cur_h);
        layers[count].client = 0;
        ++count;
    }

    return count;
}

void desktop_draw_desktop(
    struct desktop_session *session,
    int cursor_x,
    int cursor_y,
    int menu_open,
    int selected_index,
    int selected_shortcut,
    const struct desktop_dirty_rect *dirty)
{
    /* Single-threaded compositor: keep the working sets off the stack. */
    static struct desktop_layer layers[DESKTOP_MAX_COMPOSE_LAYERS];
    static struct sx_rect_set damage;
    static struct sx_rect_set visible;
    struct sx_bitmap backbuffer_bitmap;
    struct sx_painter painter;
    int layer_count = 0;
    int layer_index;

    if (session == 0 || g_backbuffer == 0)
    {
        return;
    }

    sx_bitmap_wrap(&backbuffer_bitmap, g_backbuffer, &session->gfx.info, SX_PIXEL_FORMAT_BGRX8888);
    sx_painter_init(&painter, &backbuffer_bitmap);

    /* Damage region for this frame; an empty/invalid dirty set forces a full
     * repaint (still occlusion-aware: each layer painted once). */
    sx_rect_set_clear(&damage);
    if (dirty != 0 && desktop_dirty_rect_valid(dirty))
    {
        size_t i;
        for (i = 0; i < dirty->rects.count; ++i)
        {
            (void)sx_rect_set_add(&damage, dirty->rects.rects[i]);
        }
    }
    if (!sx_rect_set_valid(&damage))
    {
        (void)sx_rect_set_add(&damage, sx_rect_make(0, 0, (int)session->gfx.info.width, (int)session->gfx.info.height));
    }

    layer_count = build_layers(session, menu_open, cursor_x, cursor_y, layers);

    /* Paint back-to-front; each layer only over the area not covered by an
     * opaque layer in front of it. */
    for (layer_index = 0; layer_index < layer_count; ++layer_index)
    {
        const struct desktop_layer *layer = &layers[layer_index];
        size_t damage_index;
        size_t visible_index;
        int front;

        sx_rect_set_clear(&visible);
        for (damage_index = 0; damage_index < damage.count; ++damage_index)
        {
            struct sx_rect clipped = sx_rect_intersect(damage.rects[damage_index], layer->bounds);
            if (!sx_rect_is_empty(clipped))
            {
                (void)sx_rect_set_add(&visible, clipped);
            }
        }

        for (front = layer_index + 1; front < layer_count && sx_rect_set_valid(&visible); ++front)
        {
            if (layers[front].opaque)
            {
                (void)sx_rect_set_subtract_rect(&visible, layers[front].bounds);
            }
        }

        for (visible_index = 0; visible_index < visible.count; ++visible_index)
        {
            struct sx_rect sub = visible.rects[visible_index];
            if (sx_rect_is_empty(sub))
            {
                continue;
            }
            sx_painter_clear_clip(&painter);
            sx_painter_add_clip_rect(&painter, sub);
            paint_layer(&painter, session, layer, menu_open, selected_index, selected_shortcut, cursor_x, cursor_y);
        }
    }
}
