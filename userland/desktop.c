#include "libc.h"
#include "shared/version.h"
#include "cursor_asset.h"

#define GFX_MAX_WIDTH 1920
#define GFX_MAX_HEIGHT 1080
#define TASKBAR_HEIGHT 36
#define START_BUTTON_WIDTH 88
#define CLOCK_BOX_WIDTH 84
#define MENU_WIDTH 272
#define MENU_ITEM_HEIGHT 32
#define MENU_PADDING 12
#define MENU_HEADER_HEIGHT 50
#define MENU_STRIP_WIDTH 36

struct desktop_menu_item
{
    const char *label;
    const char *path;
    int exits_desktop;
};

struct desktop_session
{
    struct savanxp_gfx_context gfx;
    int mouse_fd;
};

static uint32_t g_backbuffer[GFX_MAX_WIDTH * GFX_MAX_HEIGHT];

static const struct desktop_menu_item k_menu_items[] = {
    {"Doom", "/disk/bin/doomgeneric", 0},
    {"Gfx Demo", "/bin/gfxdemo", 0},
    {"Key Test", "/bin/keytest", 0},
    {"Mouse Test", "/bin/mousetest", 0},
    {"Exit Desktop", 0, 1},
};

static int menu_item_count(void)
{
    return (int)(sizeof(k_menu_items) / sizeof(k_menu_items[0]));
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
    const int panel_y = taskbar_y + 4;
    const int panel_height = TASKBAR_HEIGHT - 8;
    const int clock_x = (int)gfx->info.width - CLOCK_BOX_WIDTH - 6;
    char clock_text[6];
    int clock_text_x;
    int clock_text_y;

    gfx_rect(g_backbuffer, &gfx->info, 0, taskbar_y, (int)gfx->info.width, TASKBAR_HEIGHT, gfx_rgb(196, 199, 203));
    gfx_hline(g_backbuffer, &gfx->info, 0, taskbar_y, (int)gfx->info.width, gfx_rgb(255, 255, 255));
    gfx_hline(g_backbuffer, &gfx->info, 0, taskbar_y + 1, (int)gfx->info.width, gfx_rgb(223, 223, 223));
    gfx_hline(g_backbuffer, &gfx->info, 0, taskbar_y + 2, (int)gfx->info.width, gfx_rgb(164, 168, 173));

    draw_button(gfx, 4, panel_y, START_BUTTON_WIDTH, panel_height, gfx_rgb(196, 199, 203), menu_open);
    gfx_rect(g_backbuffer, &gfx->info, 14 + (menu_open ? 1 : 0), panel_y + 7 + (menu_open ? 1 : 0), 10, 10, gfx_rgb(0, 96, 64));
    draw_embossed_text(
        gfx,
        31 + (menu_open ? 1 : 0),
        taskbar_y + 10 + (menu_open ? 1 : 0),
        "Start",
        gfx_rgb(255, 255, 255),
        gfx_rgb(0, 0, 0));

    draw_inset_box(gfx, clock_x, panel_y, CLOCK_BOX_WIDTH, panel_height, gfx_rgb(196, 199, 203));
    (void)current_clock_stamp(clock_text);
    clock_text_x = clock_x + ((CLOCK_BOX_WIDTH - gfx_text_width(clock_text)) / 2);
    clock_text_y = panel_y + ((panel_height - gfx_text_height()) / 2);
    gfx_blit_text(g_backbuffer, &gfx->info, clock_text_x, clock_text_y, clock_text, gfx_rgb(0, 0, 0));
}

static void draw_start_menu(struct savanxp_gfx_context *gfx, int selected_index)
{
    const int menu_height = (menu_item_count() * MENU_ITEM_HEIGHT) + (MENU_PADDING * 2) + MENU_HEADER_HEIGHT + 14;
    const int menu_y = (int)gfx->info.height - TASKBAR_HEIGHT - menu_height;
    const int strip_x = 6;
    const int strip_inner_x = strip_x + 4;
    const int strip_inner_y = menu_y + 14;
    const int content_x = strip_x + MENU_STRIP_WIDTH + 12;
    const int content_width = MENU_WIDTH - content_x - 12;
    const int header_y = menu_y + 8;
    const int items_y = menu_y + MENU_HEADER_HEIGHT + MENU_PADDING + 4;
    int index;

    draw_button(gfx, 0, menu_y, MENU_WIDTH, menu_height, gfx_rgb(196, 199, 203), 0);
    gfx_rect(g_backbuffer, &gfx->info, strip_x, menu_y + 6, MENU_STRIP_WIDTH, menu_height - 12, gfx_rgb(0, 78, 152));
    gfx_rect(g_backbuffer, &gfx->info, strip_inner_x, strip_inner_y, MENU_STRIP_WIDTH - 8, 18, gfx_rgb(0, 124, 96));
    gfx_blit_text(g_backbuffer, &gfx->info, strip_inner_x + 3, strip_inner_y + 4, "S", gfx_rgb(255, 255, 255));
    gfx_blit_text(g_backbuffer, &gfx->info, strip_inner_x + 3, menu_y + menu_height - 86, "S", gfx_rgb(255, 255, 255));
    gfx_blit_text(g_backbuffer, &gfx->info, strip_inner_x + 3, menu_y + menu_height - 68, "X", gfx_rgb(255, 255, 255));

    gfx_rect(g_backbuffer, &gfx->info, content_x - 4, header_y, content_width + 4, MENU_HEADER_HEIGHT - 10, gfx_rgb(234, 233, 227));
    gfx_blit_text(g_backbuffer, &gfx->info, content_x + 4, header_y + 16, "Programs", gfx_rgb(0, 0, 0));
    gfx_hline(g_backbuffer, &gfx->info, content_x - 4, header_y + MENU_HEADER_HEIGHT - 10, content_width + 4, gfx_rgb(172, 172, 172));

    for (index = 0; index < menu_item_count(); ++index)
    {
        const int item_y = items_y + (index * MENU_ITEM_HEIGHT);
        const int icon_x = content_x + 4;
        const int text_x = content_x + 32;
        const uint32_t icon_color = k_menu_items[index].exits_desktop ? gfx_rgb(144, 40, 40) : gfx_rgb(0, 124, 96);
        const int arrow_x = content_x + content_width - 18;

        if (k_menu_items[index].exits_desktop)
        {
            gfx_hline(g_backbuffer, &gfx->info, content_x, item_y - 3, content_width - 8, gfx_rgb(172, 172, 172));
            gfx_hline(g_backbuffer, &gfx->info, content_x, item_y - 2, content_width - 8, gfx_rgb(255, 255, 255));
        }

        if (index == selected_index)
        {
            gfx_rect(g_backbuffer, &gfx->info, content_x - 2, item_y, content_width - 4, MENU_ITEM_HEIGHT - 2, gfx_rgb(10, 36, 106));
            gfx_hline(g_backbuffer, &gfx->info, content_x - 2, item_y, content_width - 4, gfx_rgb(49, 107, 206));
            gfx_rect(g_backbuffer, &gfx->info, icon_x, item_y + 7, 16, 16, gfx_rgb(255, 255, 255));
            gfx_rect(g_backbuffer, &gfx->info, icon_x + 2, item_y + 9, 12, 12, icon_color);
            gfx_blit_text(g_backbuffer, &gfx->info, text_x, item_y + 9, k_menu_items[index].label, gfx_rgb(255, 255, 255));
            if (!k_menu_items[index].exits_desktop)
            {
                gfx_blit_text(g_backbuffer, &gfx->info, arrow_x, item_y + 9, ">", gfx_rgb(255, 255, 255));
            }
        }
        else
        {
            gfx_rect(g_backbuffer, &gfx->info, icon_x, item_y + 7, 16, 16, gfx_rgb(255, 255, 255));
            gfx_rect(g_backbuffer, &gfx->info, icon_x + 2, item_y + 9, 12, 12, icon_color);
            gfx_blit_text(g_backbuffer, &gfx->info, text_x, item_y + 9, k_menu_items[index].label, gfx_rgb(0, 0, 0));
            if (!k_menu_items[index].exits_desktop)
            {
                gfx_blit_text(g_backbuffer, &gfx->info, arrow_x, item_y + 9, ">", gfx_rgb(88, 88, 88));
            }
        }
    }
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

static int selected_item_from_cursor(struct savanxp_gfx_context *gfx, int cursor_x, int cursor_y)
{
    const int menu_height = (menu_item_count() * MENU_ITEM_HEIGHT) + (MENU_PADDING * 2) + MENU_HEADER_HEIGHT + 14;
    const int menu_y = (int)gfx->info.height - TASKBAR_HEIGHT - menu_height;
    const int content_x = 6 + MENU_STRIP_WIDTH + 12;
    const int content_width = MENU_WIDTH - content_x - 12;
    const int items_y = menu_y + MENU_HEADER_HEIGHT + MENU_PADDING + 4;
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

static void draw_desktop(struct desktop_session *session, int cursor_x, int cursor_y, int menu_open, int selected_index)
{
    draw_background(&session->gfx);
    draw_taskbar(session, menu_open);
    if (menu_open)
    {
        draw_start_menu(&session->gfx, selected_index);
    }
    draw_cursor(&session->gfx, cursor_x, cursor_y);
}

static void close_graphics_session(struct desktop_session *session)
{
    gfx_release(&session->gfx);
    if (session->mouse_fd >= 0)
    {
        close(session->mouse_fd);
        session->mouse_fd = -1;
    }
    gfx_close(&session->gfx);
}

static int open_graphics_session(struct desktop_session *session)
{
    long mouse_fd;

    if (gfx_open(&session->gfx) < 0)
    {
        return -1;
    }
    if (session->gfx.info.width > GFX_MAX_WIDTH || session->gfx.info.height > GFX_MAX_HEIGHT || (session->gfx.info.pitch / 4u) > GFX_MAX_WIDTH)
    {
        gfx_close(&session->gfx);
        return -1;
    }
    if (gfx_acquire(&session->gfx) < 0)
    {
        gfx_close(&session->gfx);
        return -1;
    }

    mouse_fd = mouse_open();
    session->mouse_fd = mouse_fd >= 0 ? (int)mouse_fd : -1;
    return 0;
}

static int launch_menu_item(const struct desktop_menu_item *item)
{
    const char *argv[2];
    long pid;
    int status = 0;

    if (item->exits_desktop)
    {
        return 1;
    }

    argv[0] = item->path;
    argv[1] = 0;
    pid = spawn(item->path, argv, 1);
    if (pid < 0)
    {
        eprintf("desktop: failed to launch %s (%s)\n", item->label, result_error_string(pid));
        return 0;
    }

    waitpid((int)pid, &status);
    return 0;
}

static int handoff_and_launch(struct desktop_session *session, const struct desktop_menu_item *item)
{
    close_graphics_session(session);

    if (launch_menu_item(item) != 0)
    {
        return 1;
    }

    if (open_graphics_session(session) < 0)
    {
        puts_fd(2, "desktop: failed to reopen graphics session\n");
        return -1;
    }

    return 0;
}

int main(void)
{
    struct desktop_session session = {{-1, -1, {0, 0, 0, 0, 0}}, -1};
    struct savanxp_input_event key_event;
    struct savanxp_mouse_event mouse_event;
    int cursor_x;
    int cursor_y;
    int menu_open = 0;
    int selected_index = 0;
    uint32_t last_buttons = 0;
    unsigned long last_clock_stamp = 0;
    int needs_redraw = 1;

    if (open_graphics_session(&session) < 0)
    {
        puts_fd(2, "desktop: failed to open graphics session\n");
        return 1;
    }

    cursor_x = 0;
    cursor_y = 0;
    {
        char clock_text[6];
        last_clock_stamp = current_clock_stamp(clock_text);
    }

    for (;;)
    {
        while (gfx_poll_event(&session.gfx, &key_event) > 0)
        {
            if (key_event.type != SAVANXP_INPUT_EVENT_KEY_DOWN)
            {
                continue;
            }

            if (key_event.key == SAVANXP_KEY_SUPER)
            {
                menu_open = !menu_open;
                if (menu_open)
                {
                    selected_index = 0;
                }
                needs_redraw = 1;
                continue;
            }

            if (key_event.key == SAVANXP_KEY_ESC)
            {
                if (menu_open)
                {
                    menu_open = 0;
                    needs_redraw = 1;
                    continue;
                }
                close_graphics_session(&session);
                return 0;
            }

            if (key_event.key == SAVANXP_KEY_UP || key_event.key == SAVANXP_KEY_DOWN)
            {
                if (!menu_open)
                {
                    menu_open = 1;
                    selected_index = 0;
                }
                else if (key_event.key == SAVANXP_KEY_UP)
                {
                    selected_index = (selected_index + menu_item_count() - 1) % menu_item_count();
                }
                else
                {
                    selected_index = (selected_index + 1) % menu_item_count();
                }
                needs_redraw = 1;
                continue;
            }

            if (key_event.key == SAVANXP_KEY_ENTER && menu_open)
            {
                const int launch_result = handoff_and_launch(&session, &k_menu_items[selected_index]);
                if (launch_result > 0)
                {
                    return 0;
                }
                if (launch_result < 0)
                {
                    return 1;
                }
                cursor_x = clamp_int(cursor_x, 0, (int)session.gfx.info.width - 1);
                cursor_y = clamp_int(cursor_y, 0, (int)session.gfx.info.height - 1);
                last_buttons = 0;
                menu_open = 0;
                needs_redraw = 1;
            }
        }

        while (session.mouse_fd >= 0 && mouse_poll_event(session.mouse_fd, &mouse_event) > 0)
        {
            uint32_t pressed_buttons = mouse_event.buttons;
            const uint32_t left_pressed = pressed_buttons & SAVANXP_MOUSE_BUTTON_LEFT;
            const uint32_t right_pressed = pressed_buttons & SAVANXP_MOUSE_BUTTON_RIGHT;
            const uint32_t left_was_pressed = last_buttons & SAVANXP_MOUSE_BUTTON_LEFT;
            const uint32_t right_was_pressed = last_buttons & SAVANXP_MOUSE_BUTTON_RIGHT;

            cursor_x = clamp_int(cursor_x + mouse_event.delta_x, 0, (int)session.gfx.info.width - 1);
            cursor_y = clamp_int(cursor_y + mouse_event.delta_y, 0, (int)session.gfx.info.height - 1);

            if (menu_open)
            {
                const int hovered = selected_item_from_cursor(&session.gfx, cursor_x, cursor_y);
                if (hovered >= 0)
                {
                    selected_index = hovered;
                }
            }

            if (left_pressed != 0 && left_was_pressed == 0)
            {
                const int taskbar_y = (int)session.gfx.info.height - TASKBAR_HEIGHT;
                const int hovered = menu_open ? selected_item_from_cursor(&session.gfx, cursor_x, cursor_y) : -1;

                if (point_in_rect(cursor_x, cursor_y, 4, taskbar_y + 4, START_BUTTON_WIDTH, TASKBAR_HEIGHT - 8))
                {
                    menu_open = !menu_open;
                    if (menu_open)
                    {
                        selected_index = 0;
                    }
                }
                else if (menu_open && hovered >= 0)
                {
                    const int launch_result = handoff_and_launch(&session, &k_menu_items[hovered]);
                    if (launch_result > 0)
                    {
                        return 0;
                    }
                    if (launch_result < 0)
                    {
                        return 1;
                    }
                    cursor_x = clamp_int(cursor_x, 0, (int)session.gfx.info.width - 1);
                    cursor_y = clamp_int(cursor_y, 0, (int)session.gfx.info.height - 1);
                    pressed_buttons = 0;
                    menu_open = 0;
                }
                else if (menu_open)
                {
                    menu_open = 0;
                }
                needs_redraw = 1;
            }

            if (right_pressed != 0 && right_was_pressed == 0 && menu_open)
            {
                menu_open = 0;
                needs_redraw = 1;
            }

            last_buttons = pressed_buttons;
            needs_redraw = 1;
        }

        {
            char clock_text[6];
            const unsigned long clock_stamp = current_clock_stamp(clock_text);
            if (clock_stamp != last_clock_stamp)
            {
                last_clock_stamp = clock_stamp;
                needs_redraw = 1;
            }
        }

        if (!needs_redraw)
        {
            sleep_ms(16);
            continue;
        }

        draw_desktop(&session, cursor_x, cursor_y, menu_open, selected_index);
        if (gfx_present(&session.gfx, g_backbuffer) < 0)
        {
            break;
        }
        needs_redraw = 0;
    }

    close_graphics_session(&session);
    puts_fd(2, "desktop: present failed\n");
    return 1;
}
