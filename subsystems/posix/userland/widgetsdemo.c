#include "libc.h"
#include "savanxp/sxgui.h"

#include <stdio.h>
#include <string.h>

static char g_text_buffer[48] = "type here";
static char g_status[64] = "Ready";
static int g_ok_clicks = 0;

static const char *const g_list_items[] = {
    "Documents",
    "Pictures",
    "Music",
    "Programs",
    "Network",
    "Recycle Bin"
};

static void on_ok(struct sxgui_widget *widget, void *user)
{
    (void)widget;
    (void)user;
    g_ok_clicks += 1;
    snprintf(g_status, sizeof(g_status), "OK pressed %d time(s)", g_ok_clicks);
}

static void on_cancel(struct sxgui_widget *widget, void *user)
{
    (void)widget;
    (void)user;
    snprintf(g_status, sizeof(g_status), "Cancel pressed");
}

int main(void)
{
    struct savanxp_gfx_context gfx;
    struct savanxp_input_event key_event;
    struct savanxp_gui_pointer_event pointer_event;
    struct sxgui_widget widgets[7];
    struct sxgui_context ui;
    long pointer_fd = -1;
    int needs_redraw = 1;
    int item_count = (int)(sizeof(g_list_items) / sizeof(g_list_items[0]));

    if (gfx_open(&gfx) < 0)
    {
        puts_fd(2, "widgetsdemo: gfx_open failed\n");
        return 1;
    }
    if (gfx_acquire(&gfx) < 0)
    {
        puts_fd(2, "widgetsdemo: gfx_acquire failed\n");
        gfx_close(&gfx);
        return 1;
    }
    pointer_fd = gfx_pointer_open();

    widgets[0] = sxgui_label(sx_rect_make(16, 14, 320, 16), "sxgui widget gallery");
    widgets[1] = sxgui_button(sx_rect_make(16, 42, 100, 26), "OK", on_ok, 0);
    widgets[2] = sxgui_button(sx_rect_make(128, 42, 100, 26), "Cancel", on_cancel, 0);
    widgets[3] = sxgui_checkbox(sx_rect_make(16, 84, 220, 18), "Enable option", 1);
    widgets[4] = sxgui_textfield(sx_rect_make(16, 114, 220, 22), g_text_buffer, (int)sizeof(g_text_buffer));
    widgets[5] = sxgui_listbox(sx_rect_make(16, 148, 220, 112), g_list_items, item_count);
    widgets[6] = sxgui_label(sx_rect_make(16, 270, 320, 16), g_status);

    sxgui_context_init(&ui, gfx.pixels, &gfx.info, widgets, 7);

    for (;;)
    {
        while (gfx_poll_event(&gfx, &key_event) > 0)
        {
            if (key_event.type == SAVANXP_INPUT_EVENT_RESIZED)
            {
                (void)gfx_apply_resize_event(&gfx, &key_event);
                sx_bitmap_wrap(&ui.target, gfx.pixels, &gfx.info, SX_PIXEL_FORMAT_BGRX8888);
                sx_painter_init(&ui.painter, &ui.target);
                needs_redraw = 1;
                continue;
            }
            if (key_event.type == SAVANXP_INPUT_EVENT_KEY_DOWN && key_event.key == SAVANXP_KEY_ESC)
            {
                gfx_release(&gfx);
                if (pointer_fd >= 0)
                {
                    close((int)pointer_fd);
                }
                gfx_close(&gfx);
                return 0;
            }
            if (sxgui_handle_key(&ui, &key_event))
            {
                needs_redraw = 1;
            }
        }

        while (pointer_fd >= 0 && gfx_poll_pointer((int)pointer_fd, &pointer_event) > 0)
        {
            if (sxgui_handle_pointer(&ui, &pointer_event))
            {
                needs_redraw = 1;
            }
        }

        if (!needs_redraw)
        {
            sleep_ms(16);
            continue;
        }

        sxgui_paint(&ui);
        if (gfx_present(&gfx, gfx.pixels) < 0)
        {
            break;
        }
        needs_redraw = 0;
    }

    gfx_release(&gfx);
    if (pointer_fd >= 0)
    {
        close((int)pointer_fd);
    }
    gfx_close(&gfx);
    puts_fd(2, "widgetsdemo: present failed\n");
    return 1;
}
