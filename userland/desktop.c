#include "libc.h"
#include "shared/version.h"
#include "cursor_asset.h"

#define TASKBAR_HEIGHT 40
#define START_BUTTON_WIDTH 96
#define CLOCK_BOX_WIDTH 88
#define MENU_WIDTH 392
#define MENU_ITEM_HEIGHT 46
#define MENU_PADDING 16
#define MENU_HEADER_HEIGHT 68
#define MENU_STRIP_WIDTH 52
#define MENU_FOOTER_HEIGHT 44
#define TASKBAR_GAP 8

#define RGB_LITERAL(red, green, blue) (((uint32_t)(red) << 16) | ((uint32_t)(green) << 8) | (uint32_t)(blue))

struct desktop_menu_item
{
    const char *label;
    const char *path;
    const char *subtitle;
    uint32_t accent;
};

struct desktop_session
{
    struct savanxp_gfx_context gfx;
    struct savanxp_gpu_info gpu_info;
    int gpu_fd;
    int input_fd;
    int mouse_fd;
    int display_section_fd;
    uint32_t display_surface_id;
    void *display_view;
    uint32_t *display_pixels;
    int hw_cursor_enabled;
    struct
    {
        const char *path;
        long pid;
        int section_fd;
        int input_write_fd;
        int mouse_write_fd;
        int present_read_fd;
        int present_nonblocking;
        void *mapped_view;
        struct savanxp_gpu_client_surface_header *header;
        uint32_t *pixels;
    } client;
};

struct desktop_dirty_rect
{
    int valid;
    int x;
    int y;
    int width;
    int height;
};

static uint32_t *g_backbuffer = 0;
static const char *k_shellapp_path = "/bin/shellapp";

static const struct desktop_menu_item k_menu_items[] = {
    {"Shell", "/bin/shellapp", "Terminal and builtins", RGB_LITERAL(0, 124, 96)},
    {"Doom", "/disk/bin/doomgeneric", "Classic FPS test port", RGB_LITERAL(181, 81, 55)},
    {"Gfx Demo", "/bin/gfxdemo", "2D rendering and dirty regions", RGB_LITERAL(34, 142, 96)},
    {"Key Test", "/bin/keytest", "Keyboard diagnostics", RGB_LITERAL(41, 111, 188)},
    {"Mouse Test", "/bin/mousetest", "Mouse diagnostics", RGB_LITERAL(156, 104, 38)},
};

static int menu_item_count(void)
{
    return (int)(sizeof(k_menu_items) / sizeof(k_menu_items[0]));
}

static const struct desktop_menu_item *menu_item_at(int index)
{
    if (index < 0 || index >= menu_item_count())
    {
        return 0;
    }
    return &k_menu_items[index];
}

static const struct desktop_menu_item *find_menu_item_by_path(const char *path)
{
    int index;

    if (path == 0)
    {
        return 0;
    }

    for (index = 0; index < menu_item_count(); ++index)
    {
        if (strcmp(k_menu_items[index].path, path) == 0)
        {
            return &k_menu_items[index];
        }
    }
    return 0;
}

static int client_surface_height_for_display(const struct savanxp_fb_info *info)
{
    if (info == 0)
    {
        return 0;
    }
    return (int)info->height > TASKBAR_HEIGHT ? (int)info->height - TASKBAR_HEIGHT : (int)info->height;
}

static void fill_client_surface_info(const struct savanxp_fb_info *display_info, struct savanxp_fb_info *client_info)
{
    if (display_info == 0 || client_info == 0)
    {
        return;
    }

    *client_info = *display_info;
    client_info->height = (uint32_t)client_surface_height_for_display(display_info);
    client_info->buffer_size = client_info->pitch * client_info->height;
}

static const struct savanxp_fb_info *client_surface_info(const struct desktop_session *session)
{
    return session != 0 && session->client.header != 0 ? &session->client.header->info : 0;
}

static int point_in_client_area(const struct desktop_session *session, int x, int y)
{
    const struct savanxp_fb_info *info = client_surface_info(session);
    if (info == 0)
    {
        return y >= 0 && y < client_surface_height_for_display(&session->gfx.info) &&
               x >= 0 && x < (int)session->gfx.info.width;
    }
    return x >= 0 && y >= 0 && x < (int)info->width && y < (int)info->height;
}

static int clamp_int(int value, int minimum, int maximum)
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

static int point_in_rect(int x, int y, int rect_x, int rect_y, int rect_w, int rect_h)
{
    return x >= rect_x && y >= rect_y && x < rect_x + rect_w && y < rect_y + rect_h;
}

static int rects_intersect(int left_x, int left_y, int left_w, int left_h, int right_x, int right_y, int right_w, int right_h)
{
    return left_x < right_x + right_w && right_x < left_x + left_w &&
           left_y < right_y + right_h && right_y < left_y + left_h;
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

static void dirty_rect_reset(struct desktop_dirty_rect *dirty)
{
    if (dirty != 0)
    {
        memset(dirty, 0, sizeof(*dirty));
    }
}

static void dirty_rect_add(struct desktop_dirty_rect *dirty, const struct savanxp_fb_info *info, int x, int y, int width, int height)
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

static void dirty_rect_add_fullscreen(struct desktop_dirty_rect *dirty, const struct savanxp_fb_info *info)
{
    if (info != 0)
    {
        dirty_rect_add(dirty, info, 0, 0, (int)info->width, (int)info->height);
    }
}

static void dirty_rect_add_taskbar(struct desktop_dirty_rect *dirty, const struct savanxp_fb_info *info)
{
    if (info != 0)
    {
        dirty_rect_add(dirty, info, 0, (int)info->height - TASKBAR_HEIGHT, (int)info->width, TASKBAR_HEIGHT);
    }
}

static int start_menu_height(void)
{
    return (menu_item_count() * MENU_ITEM_HEIGHT) + (MENU_PADDING * 2) + MENU_HEADER_HEIGHT + MENU_FOOTER_HEIGHT + 16;
}

static int start_menu_content_x(int menu_x)
{
    return menu_x + 6 + MENU_STRIP_WIDTH + 16;
}

static int start_menu_content_width(void)
{
    return MENU_WIDTH - (start_menu_content_x(0) + 14);
}

static int start_menu_items_y(int menu_y)
{
    return menu_y + MENU_HEADER_HEIGHT + MENU_PADDING + 10;
}

static int start_menu_footer_y(int menu_y, int menu_height)
{
    return menu_y + menu_height - MENU_FOOTER_HEIGHT - 10;
}

static void start_menu_bounds(const struct savanxp_fb_info *info, int *x, int *y, int *width, int *height)
{
    if (x == 0 || y == 0 || width == 0 || height == 0 || info == 0)
    {
        return;
    }
    *width = MENU_WIDTH;
    *height = start_menu_height();
    *x = 0;
    *y = (int)info->height - TASKBAR_HEIGHT - *height;
}

static const struct desktop_menu_item *active_client_menu_item(const struct desktop_session *session)
{
    return session != 0 ? find_menu_item_by_path(session->client.path) : 0;
}

static const char *active_client_label(const struct desktop_session *session)
{
    const struct desktop_menu_item *item = active_client_menu_item(session);
    return item != 0 ? item->label : (session != 0 && session->client.path != 0 ? "App" : "Desktop");
}

static uint32_t active_client_accent(const struct desktop_session *session)
{
    const struct desktop_menu_item *item = active_client_menu_item(session);
    return item != 0 ? item->accent : RGB_LITERAL(40, 108, 182);
}

static void dirty_rect_add_menu(struct desktop_dirty_rect *dirty, const struct savanxp_fb_info *info)
{
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;

    start_menu_bounds(info, &x, &y, &width, &height);
    dirty_rect_add(dirty, info, x, y, width, height);
}

static void cursor_bounds(int cursor_x, int cursor_y, int *x, int *y, int *width, int *height)
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

static void dirty_rect_add_cursor(struct desktop_dirty_rect *dirty, const struct savanxp_fb_info *info, int cursor_x, int cursor_y)
{
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;

    cursor_bounds(cursor_x, cursor_y, &x, &y, &width, &height);
    dirty_rect_add(dirty, info, x, y, width, height);
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

static unsigned long current_clock_stamp(char *buffer)
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

static void draw_background(struct savanxp_gfx_context *gfx)
{
    const char *watermark = SAVANXP_DISPLAY_NAME;
    const int text_width = gfx_text_width(watermark);
    const int text_height = gfx_text_height();
    const int text_x = (int)gfx->info.width - text_width - 14;
    const int text_y = (int)gfx->info.height - TASKBAR_HEIGHT - text_height - 14;

    gfx_clear(g_backbuffer, &gfx->info, gfx_rgb(0, 128, 128));
    gfx_blit_text(g_backbuffer, &gfx->info, text_x + 1, text_y + 1, watermark, gfx_rgb(0, 64, 64));
    gfx_blit_text(g_backbuffer, &gfx->info, text_x, text_y, watermark, gfx_rgb(210, 244, 244));
}

static void draw_taskbar(struct desktop_session *session, int menu_open)
{
    struct savanxp_gfx_context *gfx = &session->gfx;
    const int taskbar_y = (int)gfx->info.height - TASKBAR_HEIGHT;
    const int panel_y = taskbar_y + 6;
    const int panel_height = TASKBAR_HEIGHT - 12;
    const char *client_label = active_client_label(session);
    const char *version_text = SAVANXP_VERSION_STRING;
    const uint32_t accent = active_client_accent(session);
    const int clock_x = (int)gfx->info.width - CLOCK_BOX_WIDTH - TASKBAR_GAP;
    const int version_width = gfx_text_width(version_text) + 22;
    const int version_x = clock_x - version_width - TASKBAR_GAP;
    const int status_x = START_BUTTON_WIDTH + 12;
    const int status_width = version_x - status_x - TASKBAR_GAP;
    char clock_text[6];
    int clock_text_x;
    int clock_text_y;
    int status_text_x;

    gfx_rect(g_backbuffer, &gfx->info, 0, taskbar_y, (int)gfx->info.width, TASKBAR_HEIGHT, gfx_rgb(190, 194, 200));
    gfx_hline(g_backbuffer, &gfx->info, 0, taskbar_y, (int)gfx->info.width, gfx_rgb(255, 255, 255));
    gfx_hline(g_backbuffer, &gfx->info, 0, taskbar_y + 1, (int)gfx->info.width, gfx_rgb(232, 235, 239));
    gfx_hline(g_backbuffer, &gfx->info, 0, taskbar_y + 2, (int)gfx->info.width, gfx_rgb(166, 170, 176));
    gfx_hline(g_backbuffer, &gfx->info, 0, taskbar_y + TASKBAR_HEIGHT - 1, (int)gfx->info.width, gfx_rgb(79, 83, 89));

    draw_button(gfx, 6, panel_y, START_BUTTON_WIDTH, panel_height, gfx_rgb(196, 199, 203), menu_open);
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

    draw_inset_box(gfx, clock_x, panel_y, CLOCK_BOX_WIDTH, panel_height, gfx_rgb(196, 199, 203));
    (void)current_clock_stamp(clock_text);
    clock_text_x = clock_x + ((CLOCK_BOX_WIDTH - gfx_text_width(clock_text)) / 2);
    clock_text_y = panel_y + ((panel_height - gfx_text_height()) / 2);
    gfx_blit_text(g_backbuffer, &gfx->info, clock_text_x, clock_text_y, clock_text, gfx_rgb(0, 0, 0));
}

static void draw_start_menu(struct savanxp_gfx_context *gfx, int selected_index)
{
    const int menu_x = 0;
    const int menu_height = start_menu_height();
    const int menu_y = (int)gfx->info.height - TASKBAR_HEIGHT - menu_height;
    const int strip_x = menu_x + 6;
    const int strip_inner_x = strip_x + 4;
    const int strip_inner_y = menu_y + 16;
    const int content_x = start_menu_content_x(menu_x);
    const int content_width = start_menu_content_width();
    const int header_y = menu_y + 8;
    const int items_y = start_menu_items_y(menu_y);
    const int footer_y = start_menu_footer_y(menu_y, menu_height);
    const struct desktop_menu_item *selected_item = menu_item_at(selected_index >= 0 ? selected_index : 0);
    int index;

    draw_button(gfx, menu_x, menu_y, MENU_WIDTH, menu_height, gfx_rgb(196, 199, 203), 0);
    gfx_rect(g_backbuffer, &gfx->info, strip_x, menu_y + 6, MENU_STRIP_WIDTH, menu_height - 12, gfx_rgb(8, 88, 150));
    gfx_hline(g_backbuffer, &gfx->info, strip_x, menu_y + 6, MENU_STRIP_WIDTH, gfx_rgb(80, 152, 216));
    gfx_rect(g_backbuffer, &gfx->info, strip_inner_x, strip_inner_y, MENU_STRIP_WIDTH - 8, 20, gfx_rgb(0, 124, 96));
    gfx_blit_text(g_backbuffer, &gfx->info, strip_inner_x + 4, strip_inner_y + 5, "S", gfx_rgb(255, 255, 255));
    gfx_blit_text(g_backbuffer, &gfx->info, strip_inner_x + 2, menu_y + menu_height - 102, "Savan", gfx_rgb(255, 255, 255));
    gfx_blit_text(g_backbuffer, &gfx->info, strip_inner_x + 2, menu_y + menu_height - 84, "XP", gfx_rgb(255, 255, 255));
    gfx_blit_text(g_backbuffer, &gfx->info, strip_inner_x + 2, menu_y + menu_height - 38, SAVANXP_VERSION_STRING, gfx_rgb(214, 233, 248));

    gfx_rect(g_backbuffer, &gfx->info, content_x - 4, header_y, content_width + 4, MENU_HEADER_HEIGHT - 10, gfx_rgb(234, 233, 227));
    gfx_blit_text(g_backbuffer, &gfx->info, content_x + 4, header_y + 12, SAVANXP_SYSTEM_NAME, gfx_rgb(0, 0, 0));
    gfx_blit_text(g_backbuffer, &gfx->info, content_x + 4, header_y + 30, "Apps and diagnostics", gfx_rgb(70, 84, 99));
    gfx_hline(g_backbuffer, &gfx->info, content_x - 4, header_y + MENU_HEADER_HEIGHT - 10, content_width + 4, gfx_rgb(172, 172, 172));

    for (index = 0; index < menu_item_count(); ++index)
    {
        const struct desktop_menu_item *item = menu_item_at(index);
        const int item_y = items_y + (index * MENU_ITEM_HEIGHT);
        const int icon_x = content_x + 4;
        const int text_x = content_x + 38;
        const int subtitle_y = item_y + 24;
        const int arrow_x = content_x + content_width - 18;
        const uint32_t icon_color = item != 0 ? item->accent : gfx_rgb(0, 124, 96);

        if (index == selected_index)
        {
            gfx_rect(g_backbuffer, &gfx->info, content_x - 2, item_y, content_width - 4, MENU_ITEM_HEIGHT - 2, gfx_rgb(13, 46, 120));
            gfx_hline(g_backbuffer, &gfx->info, content_x - 2, item_y, content_width - 4, gfx_rgb(60, 127, 224));
            gfx_rect(g_backbuffer, &gfx->info, icon_x, item_y + 10, 18, 18, gfx_rgb(255, 255, 255));
            gfx_rect(g_backbuffer, &gfx->info, icon_x + 2, item_y + 12, 14, 14, icon_color);
            gfx_blit_text(g_backbuffer, &gfx->info, text_x, item_y + 10, item->label, gfx_rgb(255, 255, 255));
            gfx_blit_text(g_backbuffer, &gfx->info, text_x, subtitle_y, item->subtitle, gfx_rgb(206, 226, 250));
            gfx_blit_text(g_backbuffer, &gfx->info, arrow_x, item_y + 12, ">", gfx_rgb(255, 255, 255));
        }
        else
        {
            gfx_hline(g_backbuffer, &gfx->info, content_x - 2, item_y + MENU_ITEM_HEIGHT - 2, content_width - 4, gfx_rgb(210, 210, 210));
            gfx_rect(g_backbuffer, &gfx->info, icon_x, item_y + 10, 18, 18, gfx_rgb(255, 255, 255));
            gfx_rect(g_backbuffer, &gfx->info, icon_x + 2, item_y + 12, 14, 14, icon_color);
            gfx_blit_text(g_backbuffer, &gfx->info, text_x, item_y + 10, item->label, gfx_rgb(0, 0, 0));
            gfx_blit_text(g_backbuffer, &gfx->info, text_x, subtitle_y, item->subtitle, gfx_rgb(86, 97, 109));
            gfx_blit_text(g_backbuffer, &gfx->info, arrow_x, item_y + 12, ">", gfx_rgb(88, 88, 88));
        }
    }

    draw_inset_box(gfx, content_x - 4, footer_y, content_width + 4, MENU_FOOTER_HEIGHT, gfx_rgb(196, 199, 203));
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

static int set_hw_cursor_position(struct desktop_session *session, int cursor_x, int cursor_y, int visible)
{
    struct savanxp_gpu_cursor_position position;

    if (session == 0 || !session->hw_cursor_enabled || session->gpu_fd < 0)
    {
        return -1;
    }

    position.x = (uint32_t)cursor_x;
    position.y = (uint32_t)cursor_y;
    position.visible = visible ? 1u : 0u;
    position.reserved0 = 0;
    return gpu_move_cursor(session->gpu_fd, &position) < 0 ? -1 : 0;
}

static int try_enable_hw_cursor(struct desktop_session *session, int cursor_x, int cursor_y)
{
    static uint32_t cursor_pixels[64 * 64];
    struct savanxp_gpu_cursor_image image;
    int row;
    int column;

    if (session == 0 || session->gpu_fd < 0)
    {
        return 0;
    }
    if ((session->gpu_info.flags & SAVANXP_GPU_INFO_FLAG_CURSOR_PLANE) == 0)
    {
        return 0;
    }

    memset(cursor_pixels, 0, sizeof(cursor_pixels));
    for (row = 0; row < DESKTOP_CURSOR_HEIGHT; ++row)
    {
        for (column = 0; column < DESKTOP_CURSOR_WIDTH; ++column)
        {
            cursor_pixels[(row * 64) + column] = k_desktop_cursor_pixels[(row * DESKTOP_CURSOR_WIDTH) + column];
        }
    }

    image.pixels = (uint64_t)(unsigned long)cursor_pixels;
    image.width = 64;
    image.height = 64;
    image.pitch = 64u * sizeof(uint32_t);
    image.hotspot_x = DESKTOP_CURSOR_HOTSPOT_X;
    image.hotspot_y = DESKTOP_CURSOR_HOTSPOT_Y;
    if (gpu_set_cursor(session->gpu_fd, &image) < 0)
    {
        return 0;
    }

    session->hw_cursor_enabled = 1;
    if (set_hw_cursor_position(session, cursor_x, cursor_y, 1) < 0)
    {
        session->hw_cursor_enabled = 0;
        return 0;
    }
    return 1;
}

static int selected_item_from_cursor(struct savanxp_gfx_context *gfx, int cursor_x, int cursor_y)
{
    const int menu_height = start_menu_height();
    const int menu_y = (int)gfx->info.height - TASKBAR_HEIGHT - menu_height;
    const int content_x = start_menu_content_x(0);
    const int content_width = start_menu_content_width();
    const int items_y = start_menu_items_y(menu_y);
    int index;

    for (index = 0; index < menu_item_count(); ++index)
    {
        const int item_y = items_y + (index * MENU_ITEM_HEIGHT);
        if (point_in_rect(cursor_x, cursor_y, content_x - 2, item_y, content_width - 4, MENU_ITEM_HEIGHT - 2))
        {
            return index;
        }
    }
    return -1;
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

static void draw_desktop_region(
    struct desktop_session *session,
    int cursor_x,
    int cursor_y,
    int menu_open,
    int selected_index,
    const struct desktop_dirty_rect *dirty)
{
    const struct savanxp_fb_info *client_info = client_surface_info(session);
    int menu_x = 0;
    int menu_y = 0;
    int menu_width = 0;
    int menu_height = 0;
    int cursor_rect_x = 0;
    int cursor_rect_y = 0;
    int cursor_rect_width = 0;
    int cursor_rect_height = 0;
    const int taskbar_y = (int)session->gfx.info.height - TASKBAR_HEIGHT;

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

    if (rects_intersect(dirty->x, dirty->y, dirty->width, dirty->height, 0, taskbar_y, (int)session->gfx.info.width, TASKBAR_HEIGHT))
    {
        draw_taskbar(session, menu_open);
    }

    start_menu_bounds(&session->gfx.info, &menu_x, &menu_y, &menu_width, &menu_height);
    if (menu_open && rects_intersect(dirty->x, dirty->y, dirty->width, dirty->height, menu_x, menu_y, menu_width, menu_height))
    {
        draw_start_menu(&session->gfx, selected_index);
    }

    cursor_bounds(cursor_x, cursor_y, &cursor_rect_x, &cursor_rect_y, &cursor_rect_width, &cursor_rect_height);
    if (!session->hw_cursor_enabled &&
        rects_intersect(
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

static void close_fd_if_needed(int *fd)
{
    if (fd != 0 && *fd >= 0)
    {
        close(*fd);
        *fd = -1;
    }
}

static void close_fd_unless_target(int *fd, int target_fd)
{
    if (fd != 0 && *fd >= 0 && *fd != target_fd)
    {
        close(*fd);
        *fd = -1;
    }
}

static void reset_client(struct desktop_session *session)
{
    memset(&session->client, 0, sizeof(session->client));
    session->client.section_fd = -1;
    session->client.input_write_fd = -1;
    session->client.mouse_write_fd = -1;
    session->client.present_read_fd = -1;
    session->client.present_nonblocking = 0;
}

static int desktop_stage_failed(const char *stage, long result)
{
    if (result < 0)
    {
        eprintf("desktop: %s failed (%s)\n", stage, result_error_string(result));
    }
    else
    {
        eprintf("desktop: %s failed\n", stage);
    }
    return -1;
}

static int route_packet(int fd, const void *packet, size_t size)
{
    return fd >= 0 && write(fd, packet, size) == (long)size;
}

static int present_frame(struct desktop_session *session, const struct desktop_dirty_rect *dirty)
{
    struct savanxp_gpu_surface_present present = {
        .surface_id = session->display_surface_id,
        .x = dirty != 0 ? (uint32_t)dirty->x : 0,
        .y = dirty != 0 ? (uint32_t)dirty->y : 0,
        .width = dirty != 0 ? (uint32_t)dirty->width : session->gfx.info.width,
        .height = dirty != 0 ? (uint32_t)dirty->height : session->gfx.info.height,
    };

    if (gpu_present_surface_region(session->gpu_fd, &present) < 0)
    {
        return -1;
    }
    return 0;
}

static void destroy_client(struct desktop_session *session, int terminate_client)
{
    int status = 0;

    if (session->client.pid > 0)
    {
        if (terminate_client)
        {
            (void)kill((int)session->client.pid, SAVANXP_SIGKILL);
        }
        waitpid((int)session->client.pid, &status);
    }
    close_fd_if_needed(&session->client.input_write_fd);
    close_fd_if_needed(&session->client.mouse_write_fd);
    close_fd_if_needed(&session->client.present_read_fd);
    if (session->client.mapped_view != 0 && !result_is_error((long)session->client.mapped_view))
    {
        (void)unmap_view(session->client.mapped_view);
    }
    close_fd_if_needed(&session->client.section_fd);
    reset_client(session);
}

static int launch_client(struct desktop_session *session, const char *path)
{
    struct savanxp_gpu_client_surface_header *header;
    struct savanxp_fb_info client_info = {0};
    unsigned long section_size = 0;
    int input_pipe[2] = {-1, -1};
    int mouse_pipe[2] = {-1, -1};
    int present_pipe[2] = {-1, -1};
    const char *argv[2] = {path, 0};
    long pid;
    long result;

    reset_client(session);
    fill_client_surface_info(&session->gfx.info, &client_info);
    if (client_info.width == 0 || client_info.height == 0 || client_info.buffer_size == 0)
    {
        return -1;
    }
    section_size = (unsigned long)sizeof(*header) + client_info.buffer_size;
    session->client.section_fd = (int)section_create(section_size, SAVANXP_SECTION_READ | SAVANXP_SECTION_WRITE);
    if (session->client.section_fd < 0)
    {
        return -1;
    }
    session->client.mapped_view = map_view(session->client.section_fd, SAVANXP_SECTION_READ | SAVANXP_SECTION_WRITE);
    if (result_is_error((long)session->client.mapped_view))
    {
        close_fd_if_needed(&session->client.section_fd);
        return -1;
    }

    header = (struct savanxp_gpu_client_surface_header *)session->client.mapped_view;
    header->magic = SAVANXP_GPU_CLIENT_SURFACE_MAGIC;
    header->pixels_offset = sizeof(*header);
    header->info = client_info;
    session->client.header = header;
    session->client.pixels = (uint32_t *)((unsigned char *)session->client.mapped_view + header->pixels_offset);
    memset(session->client.pixels, 0, client_info.buffer_size);

    if (pipe(input_pipe) < 0 || pipe(mouse_pipe) < 0 || pipe(present_pipe) < 0)
    {
        goto fail;
    }

    pid = fork();
    if (pid < 0)
    {
        goto fail;
    }
    if (pid == 0)
    {
        if (dup2(session->client.section_fd, 3) < 0 ||
            dup2(input_pipe[0], 4) < 0 ||
            dup2(mouse_pipe[0], 5) < 0 ||
            dup2(present_pipe[1], 6) < 0)
        {
            exit(1);
        }

        close_fd_unless_target(&input_pipe[0], 4);
        close_fd_if_needed(&input_pipe[1]);
        close_fd_unless_target(&mouse_pipe[0], 5);
        close_fd_if_needed(&mouse_pipe[1]);
        close_fd_if_needed(&present_pipe[0]);
        close_fd_unless_target(&present_pipe[1], 6);
        close_fd_unless_target(&session->client.section_fd, 3);
        if (exec(path, argv, 1) < 0)
        {
            eprintf("desktop: exec failed for %s\n", path);
        }
        exit(1);
    }

    session->client.path = path;
    session->client.pid = pid;
    session->client.input_write_fd = input_pipe[1];
    session->client.mouse_write_fd = mouse_pipe[1];
    session->client.present_read_fd = present_pipe[0];
    result = fcntl(session->client.present_read_fd, SAVANXP_F_SETFL, SAVANXP_OPEN_NONBLOCK);
    if (result >= 0)
    {
        session->client.present_nonblocking = 1;
    }
    else
    {
        eprintf("desktop: present pipe remained blocking (%s)\n", result_error_string(result));
    }
    close_fd_if_needed(&input_pipe[0]);
    close_fd_if_needed(&mouse_pipe[0]);
    close_fd_if_needed(&present_pipe[1]);
    return 0;

fail:
    close_fd_if_needed(&input_pipe[0]);
    close_fd_if_needed(&input_pipe[1]);
    close_fd_if_needed(&mouse_pipe[0]);
    close_fd_if_needed(&mouse_pipe[1]);
    close_fd_if_needed(&present_pipe[0]);
    close_fd_if_needed(&present_pipe[1]);
    if (session->client.mapped_view != 0 && !result_is_error((long)session->client.mapped_view))
    {
        (void)unmap_view(session->client.mapped_view);
    }
    close_fd_if_needed(&session->client.section_fd);
    reset_client(session);
    return -1;
}

static int switch_client(struct desktop_session *session, const char *path)
{
    destroy_client(session, 1);
    return launch_client(session, path);
}

static int open_compositor_session(struct desktop_session *session)
{
    struct savanxp_gpu_mode mode = {0};
    struct savanxp_gpu_surface_import import_request = {0};
    long result = 0;

    memset(session, 0, sizeof(*session));
    session->gpu_fd = -1;
    session->input_fd = -1;
    session->mouse_fd = -1;
    session->display_section_fd = -1;
    session->hw_cursor_enabled = 0;
    reset_client(session);

    session->gpu_fd = (int)gpu_open();
    if (session->gpu_fd < 0)
    {
        return desktop_stage_failed("open /dev/gpu0", session->gpu_fd);
    }
    result = gpu_get_info(session->gpu_fd, &session->gpu_info);
    if (result < 0)
    {
        return desktop_stage_failed("GPU_IOC_GET_INFO", result);
    }
    result = gpu_acquire(session->gpu_fd);
    if (result < 0)
    {
        return desktop_stage_failed("GPU_IOC_ACQUIRE", result);
    }
    result = gpu_set_mode(session->gpu_fd, &mode);
    if (result < 0)
    {
        return desktop_stage_failed("GPU_IOC_SET_MODE", result);
    }

    session->gfx.info.width = mode.width;
    session->gfx.info.height = mode.height;
    session->gfx.info.pitch = mode.pitch;
    session->gfx.info.bpp = mode.bpp;
    session->gfx.info.buffer_size = mode.buffer_size;
    session->input_fd = (int)open_mode("/dev/input0", SAVANXP_OPEN_READ);
    if (session->input_fd < 0)
    {
        return desktop_stage_failed("open /dev/input0", session->input_fd);
    }

    session->mouse_fd = (int)open_mode("/dev/mouse0", SAVANXP_OPEN_READ);
    if (session->mouse_fd < 0)
    {
        eprintf("desktop: /dev/mouse0 unavailable (%s), continuing keyboard-only\n", result_error_string(session->mouse_fd));
        session->mouse_fd = -1;
    }

    session->display_section_fd = (int)section_create(session->gfx.info.buffer_size, SAVANXP_SECTION_READ | SAVANXP_SECTION_WRITE);
    if (session->display_section_fd < 0)
    {
        return desktop_stage_failed("section_create compositor surface", session->display_section_fd);
    }
    session->display_view = map_view(session->display_section_fd, SAVANXP_SECTION_READ | SAVANXP_SECTION_WRITE);
    if (result_is_error((long)session->display_view))
    {
        return desktop_stage_failed("map_view compositor surface", (long)session->display_view);
    }
    session->display_pixels = (uint32_t *)session->display_view;
    memset(session->display_pixels, 0, session->gfx.info.buffer_size);
    g_backbuffer = session->display_pixels;

    import_request.section_handle = session->display_section_fd;
    import_request.width = session->gfx.info.width;
    import_request.height = session->gfx.info.height;
    import_request.pitch = session->gfx.info.pitch;
    import_request.bpp = session->gfx.info.bpp;
    import_request.buffer_size = session->gfx.info.buffer_size;
    import_request.flags = SAVANXP_GPU_SURFACE_FLAG_SCANOUT;
    result = gpu_import_section(session->gpu_fd, &import_request);
    if (result < 0)
    {
        return desktop_stage_failed("GPU_IOC_IMPORT_SECTION", result);
    }
    session->display_surface_id = (uint32_t)import_request.surface_id;
    return 0;
}

static void close_compositor_session(struct desktop_session *session)
{
    destroy_client(session, 1);
    if (session->display_surface_id != 0 && session->gpu_fd >= 0)
    {
        (void)gpu_release_surface(session->gpu_fd, session->display_surface_id);
    }
    if (session->display_view != 0 && !result_is_error((long)session->display_view))
    {
        (void)unmap_view(session->display_view);
    }
    close_fd_if_needed(&session->display_section_fd);
    close_fd_if_needed(&session->input_fd);
    close_fd_if_needed(&session->mouse_fd);
    if (session->gpu_fd >= 0)
    {
        (void)gpu_release(session->gpu_fd);
        close_fd_if_needed(&session->gpu_fd);
    }
    g_backbuffer = 0;
}

static int launch_selected_item(struct desktop_session *session, int index)
{
    if (index < 0 || index >= menu_item_count())
    {
        return 0;
    }
    return switch_client(session, k_menu_items[index].path) < 0 ? -1 : 0;
}

int main(void)
{
    struct desktop_session session;
    struct savanxp_input_event key_event = {0};
    struct savanxp_mouse_event mouse_event = {0};
    struct desktop_dirty_rect dirty = {0};
    int cursor_x = 24;
    int cursor_y = 24;
    int menu_open = 0;
    int selected_index = 0;
    uint32_t last_buttons = 0;
    unsigned long last_clock_stamp = 0;

    if (open_compositor_session(&session) < 0 || launch_client(&session, k_shellapp_path) < 0)
    {
        puts_fd(2, "desktop: compositor startup failed\n");
        close_compositor_session(&session);
        return 1;
    }
    (void)try_enable_hw_cursor(&session, cursor_x, cursor_y);

    {
        char clock_text[6];
        last_clock_stamp = current_clock_stamp(clock_text);
    }
    dirty_rect_add_fullscreen(&dirty, &session.gfx.info);

    for (;;)
    {
        struct savanxp_pollfd poll_fds[3];
        int input_poll_index = -1;
        int mouse_poll_index = -1;
        int client_poll_index = -1;
        int poll_count = 0;
        long count = 0;

        input_poll_index = poll_count;
        poll_fds[poll_count].fd = session.input_fd;
        poll_fds[poll_count].events = SAVANXP_POLLIN;
        poll_fds[poll_count].revents = 0;
        ++poll_count;
        if (session.mouse_fd >= 0)
        {
            mouse_poll_index = poll_count;
            poll_fds[poll_count].fd = session.mouse_fd;
            poll_fds[poll_count].events = SAVANXP_POLLIN;
            poll_fds[poll_count].revents = 0;
            ++poll_count;
        }
        if (session.client.present_read_fd >= 0)
        {
            client_poll_index = poll_count;
            poll_fds[poll_count].fd = session.client.present_read_fd;
            poll_fds[poll_count].events = SAVANXP_POLLIN | SAVANXP_POLLHUP;
            poll_fds[poll_count].revents = 0;
            ++poll_count;
        }

        if (poll(poll_fds, (unsigned long)poll_count, 16) < 0)
        {
            break;
        }

        if (input_poll_index >= 0 && (poll_fds[input_poll_index].revents & SAVANXP_POLLIN) != 0)
        {
            while ((count = read(session.input_fd, &key_event, sizeof(key_event))) == (long)sizeof(key_event))
            {
                if (key_event.type == SAVANXP_INPUT_EVENT_KEY_DOWN && key_event.key == SAVANXP_KEY_SUPER)
                {
                    dirty_rect_add_menu(&dirty, &session.gfx.info);
                    dirty_rect_add_taskbar(&dirty, &session.gfx.info);
                    menu_open = !menu_open;
                    if (menu_open)
                    {
                        selected_index = 0;
                    }
                }
                else if (menu_open && key_event.type == SAVANXP_INPUT_EVENT_KEY_DOWN)
                {
                    int launch_requested = 0;

                    dirty_rect_add_menu(&dirty, &session.gfx.info);
                    if (key_event.key == SAVANXP_KEY_ESC)
                    {
                        menu_open = 0;
                    }
                    else if (key_event.key == SAVANXP_KEY_UP)
                    {
                        selected_index = (selected_index + menu_item_count() - 1) % menu_item_count();
                    }
                    else if (key_event.key == SAVANXP_KEY_DOWN)
                    {
                        selected_index = (selected_index + 1) % menu_item_count();
                    }
                    else if (key_event.key == SAVANXP_KEY_ENTER)
                    {
                        int launch_result = launch_selected_item(&session, selected_index);
                        launch_requested = 1;
                        if (launch_result < 0)
                        {
                            puts_fd(2, "desktop: failed to switch client\n");
                        }
                        menu_open = 0;
                        last_buttons = 0;
                    }
                    dirty_rect_add_taskbar(&dirty, &session.gfx.info);
                    if (menu_open)
                    {
                        dirty_rect_add_menu(&dirty, &session.gfx.info);
                    }
                    if (launch_requested)
                    {
                        dirty_rect_add_fullscreen(&dirty, &session.gfx.info);
                    }
                }
                else if (session.client.input_write_fd >= 0)
                {
                    (void)route_packet(session.client.input_write_fd, &key_event, sizeof(key_event));
                }
            }
        }

        if (mouse_poll_index >= 0 && (poll_fds[mouse_poll_index].revents & SAVANXP_POLLIN) != 0)
        {
            while ((count = read(session.mouse_fd, &mouse_event, sizeof(mouse_event))) == (long)sizeof(mouse_event))
            {
                uint32_t pressed_buttons = mouse_event.buttons;
                uint32_t left_pressed = pressed_buttons & SAVANXP_MOUSE_BUTTON_LEFT;
                uint32_t right_pressed = pressed_buttons & SAVANXP_MOUSE_BUTTON_RIGHT;
                uint32_t left_was_pressed = last_buttons & SAVANXP_MOUSE_BUTTON_LEFT;
                uint32_t right_was_pressed = last_buttons & SAVANXP_MOUSE_BUTTON_RIGHT;
                int taskbar_y = (int)session.gfx.info.height - TASKBAR_HEIGHT;
                int previous_cursor_x = cursor_x;
                int previous_cursor_y = cursor_y;
                int previous_menu_open = menu_open;
                int previous_selected_index = selected_index;
                int previous_in_client = 0;
                int current_in_client = 0;
                int launch_requested = 0;

                cursor_x = clamp_int(cursor_x + mouse_event.delta_x, 0, (int)session.gfx.info.width - 1);
                cursor_y = clamp_int(cursor_y + mouse_event.delta_y, 0, (int)session.gfx.info.height - 1);
                previous_in_client = point_in_client_area(&session, previous_cursor_x, previous_cursor_y);
                current_in_client = point_in_client_area(&session, cursor_x, cursor_y);
                if (menu_open)
                {
                    int hovered = selected_item_from_cursor(&session.gfx, cursor_x, cursor_y);
                    if (hovered >= 0)
                    {
                        selected_index = hovered;
                    }
                }

                if (left_pressed != 0 && left_was_pressed == 0)
                {
                    int hovered = menu_open ? selected_item_from_cursor(&session.gfx, cursor_x, cursor_y) : -1;
                    if (point_in_rect(cursor_x, cursor_y, 6, taskbar_y + 6, START_BUTTON_WIDTH, TASKBAR_HEIGHT - 12))
                    {
                        menu_open = !menu_open;
                        if (menu_open)
                        {
                            selected_index = 0;
                        }
                    }
                    else if (menu_open && hovered >= 0)
                    {
                        int launch_result = launch_selected_item(&session, hovered);
                        launch_requested = 1;
                        if (launch_result < 0)
                        {
                            puts_fd(2, "desktop: failed to switch client\n");
                        }
                        menu_open = 0;
                        pressed_buttons = 0;
                    }
                    else if (menu_open)
                    {
                        menu_open = 0;
                    }
                    else if (session.client.mouse_write_fd >= 0 && current_in_client)
                    {
                        (void)route_packet(session.client.mouse_write_fd, &mouse_event, sizeof(mouse_event));
                    }
                }
                else if (!menu_open && session.client.mouse_write_fd >= 0 && (current_in_client || previous_in_client))
                {
                    (void)route_packet(session.client.mouse_write_fd, &mouse_event, sizeof(mouse_event));
                }

                if (right_pressed != 0 && right_was_pressed == 0 && menu_open)
                {
                    menu_open = 0;
                }

                if (previous_cursor_x != cursor_x || previous_cursor_y != cursor_y)
                {
                    if (session.hw_cursor_enabled)
                    {
                        (void)set_hw_cursor_position(&session, cursor_x, cursor_y, 1);
                    }
                    else
                    {
                        dirty_rect_add_cursor(&dirty, &session.gfx.info, previous_cursor_x, previous_cursor_y);
                        dirty_rect_add_cursor(&dirty, &session.gfx.info, cursor_x, cursor_y);
                    }
                }
                if (previous_menu_open != menu_open || previous_selected_index != selected_index)
                {
                    dirty_rect_add_menu(&dirty, &session.gfx.info);
                    dirty_rect_add_taskbar(&dirty, &session.gfx.info);
                }
                if (launch_requested)
                {
                    dirty_rect_add_fullscreen(&dirty, &session.gfx.info);
                }
                last_buttons = pressed_buttons;
            }
        }

        if (client_poll_index >= 0 && session.client.present_read_fd >= 0)
        {
            if ((poll_fds[client_poll_index].revents & SAVANXP_POLLHUP) != 0)
            {
                destroy_client(&session, 0);
                dirty_rect_add_fullscreen(&dirty, &session.gfx.info);
            }
            else if ((poll_fds[client_poll_index].revents & SAVANXP_POLLIN) != 0)
            {
                struct savanxp_gpu_client_present_packet packet;
                int client_failed = 0;

                do
                {
                    count = read(session.client.present_read_fd, &packet, sizeof(packet));
                    if (count == (long)sizeof(packet))
                    {
                        dirty_rect_add(
                            &dirty,
                            &session.gfx.info,
                            (int)packet.x,
                            (int)packet.y,
                            (int)packet.width,
                            (int)packet.height);
                    }
                    else if (count < 0)
                    {
                        if (result_error_code(count) == SAVANXP_EAGAIN)
                        {
                            break;
                        }
                        client_failed = 1;
                        break;
                    }
                    else if (count == 0)
                    {
                        client_failed = 1;
                        break;
                    }
                    else
                    {
                        client_failed = 1;
                        break;
                    }
                }
                while (session.client.present_nonblocking);

                if (client_failed)
                {
                    destroy_client(&session, 0);
                    dirty_rect_add_fullscreen(&dirty, &session.gfx.info);
                }
            }
        }

        {
            char clock_text[6];
            unsigned long clock_stamp = current_clock_stamp(clock_text);
            if (clock_stamp != last_clock_stamp)
            {
                last_clock_stamp = clock_stamp;
                dirty_rect_add_taskbar(&dirty, &session.gfx.info);
            }
        }

        if (!dirty.valid)
        {
            continue;
        }
        draw_desktop_region(&session, cursor_x, cursor_y, menu_open, selected_index, &dirty);
        if (present_frame(&session, &dirty) < 0)
        {
            puts_fd(2, "desktop: present failed\n");
            break;
        }
        dirty_rect_reset(&dirty);
    }
    close_compositor_session(&session);
    return 1;
}
