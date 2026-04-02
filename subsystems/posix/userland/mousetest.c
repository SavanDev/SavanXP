#include "libc.h"

#define GFX_MAX_WIDTH 1920
#define GFX_MAX_HEIGHT 1080

static uint32_t g_backbuffer[GFX_MAX_WIDTH * GFX_MAX_HEIGHT];

static int append_char(char* buffer, int offset, int capacity, char value) {
    if (offset + 1 >= capacity) {
        return offset;
    }
    buffer[offset] = value;
    buffer[offset + 1] = '\0';
    return offset + 1;
}

static int append_text(char* buffer, int offset, int capacity, const char* text) {
    while (*text != '\0' && offset + 1 < capacity) {
        buffer[offset++] = *text++;
    }
    buffer[offset] = '\0';
    return offset;
}

static int append_uint(char* buffer, int offset, int capacity, unsigned int value) {
    char digits[16];
    int digit_count = 0;

    if (value == 0) {
        return append_char(buffer, offset, capacity, '0');
    }

    while (value != 0 && digit_count < (int)sizeof(digits)) {
        digits[digit_count++] = (char)('0' + (value % 10u));
        value /= 10u;
    }

    while (digit_count > 0) {
        offset = append_char(buffer, offset, capacity, digits[--digit_count]);
    }
    return offset;
}

static int append_int(char* buffer, int offset, int capacity, int value) {
    if (value < 0) {
        offset = append_char(buffer, offset, capacity, '-');
        value = -value;
    }
    return append_uint(buffer, offset, capacity, (unsigned int)value);
}

static void format_position(char* buffer, int x, int y) {
    int offset = 0;
    memset(buffer, 0, 64);
    offset = append_text(buffer, offset, 64, "Cursor: ");
    offset = append_int(buffer, offset, 64, x);
    offset = append_text(buffer, offset, 64, ", ");
    (void)append_int(buffer, offset, 64, y);
}

static void format_delta(char* buffer, int dx, int dy) {
    int offset = 0;
    memset(buffer, 0, 64);
    offset = append_text(buffer, offset, 64, "Last delta: ");
    offset = append_int(buffer, offset, 64, dx);
    offset = append_text(buffer, offset, 64, ", ");
    (void)append_int(buffer, offset, 64, dy);
}

static void format_buttons(char* buffer, uint32_t buttons) {
    int offset = 0;
    memset(buffer, 0, 96);
    offset = append_text(buffer, offset, 96, "Buttons: ");
    if (buttons == 0) {
        append_text(buffer, offset, 96, "none");
        return;
    }
    if ((buttons & SAVANXP_MOUSE_BUTTON_LEFT) != 0) {
        offset = append_text(buffer, offset, 96, "left ");
    }
    if ((buttons & SAVANXP_MOUSE_BUTTON_RIGHT) != 0) {
        offset = append_text(buffer, offset, 96, "right ");
    }
    if ((buttons & SAVANXP_MOUSE_BUTTON_MIDDLE) != 0) {
        (void)append_text(buffer, offset, 96, "middle");
    }
}

static void draw_crosshair(struct savanxp_gfx_context* gfx, int x, int y) {
    gfx_vline(g_backbuffer, &gfx->info, x, y - 10, 21, gfx_rgb(255, 255, 255));
    gfx_hline(g_backbuffer, &gfx->info, x - 10, y, 21, gfx_rgb(255, 255, 255));
    gfx_rect(g_backbuffer, &gfx->info, x - 2, y - 2, 5, 5, gfx_rgb(14, 88, 161));
}

static void draw_scene(struct savanxp_gfx_context* gfx, int cursor_x, int cursor_y, int delta_x, int delta_y, uint32_t buttons) {
    char line0[64];
    char line1[64];
    char line2[96];

    format_position(line0, cursor_x, cursor_y);
    format_delta(line1, delta_x, delta_y);
    format_buttons(line2, buttons);

    gfx_clear(g_backbuffer, &gfx->info, gfx_rgb(18, 59, 102));
    gfx_rect(g_backbuffer, &gfx->info, 0, 0, (int)gfx->info.width, 34, gfx_rgb(6, 40, 78));
    gfx_hline(g_backbuffer, &gfx->info, 0, 34, (int)gfx->info.width, gfx_rgb(145, 201, 236));
    gfx_blit_text(g_backbuffer, &gfx->info, 36, 10, "SavanXP mouse test", gfx_rgb(255, 255, 255));

    gfx_rect(g_backbuffer, &gfx->info, 24, 54, (int)gfx->info.width - 48, 98, gfx_rgb(225, 232, 238));
    gfx_frame(g_backbuffer, &gfx->info, 24, 54, (int)gfx->info.width - 48, 98, gfx_rgb(60, 90, 120));
    gfx_blit_text(g_backbuffer, &gfx->info, 40, 70, "Move the mouse, click buttons, ESC exits", gfx_rgb(0, 0, 0));
    gfx_blit_text(g_backbuffer, &gfx->info, 40, 92, line0, gfx_rgb(0, 0, 0));
    gfx_blit_text(g_backbuffer, &gfx->info, 40, 114, line1, gfx_rgb(0, 0, 0));
    gfx_blit_text(g_backbuffer, &gfx->info, 40, 136, line2, gfx_rgb(0, 0, 0));

    draw_crosshair(gfx, cursor_x, cursor_y);
}

int main(void) {
    struct savanxp_gfx_context gfx;
    struct savanxp_input_event key_event;
    struct savanxp_mouse_event mouse_event;
    long mouse_fd;
    int cursor_x;
    int cursor_y;
    int delta_x = 0;
    int delta_y = 0;
    uint32_t buttons = 0;
    int needs_redraw = 1;

    if (gfx_open(&gfx) < 0) {
        puts_fd(2, "mousetest: open failed\n");
        return 1;
    }
    if (gfx.info.width > GFX_MAX_WIDTH || gfx.info.height > GFX_MAX_HEIGHT || (gfx.info.pitch / 4u) > GFX_MAX_WIDTH) {
        puts_fd(2, "mousetest: framebuffer too large\n");
        gfx_close(&gfx);
        return 1;
    }

    mouse_fd = mouse_open();
    if (mouse_fd < 0) {
        puts_fd(2, "mousetest: /dev/mouse0 not available\n");
        gfx_close(&gfx);
        return 1;
    }
    if (gfx_acquire(&gfx) < 0) {
        puts_fd(2, "mousetest: acquire failed\n");
        close((int)mouse_fd);
        gfx_close(&gfx);
        return 1;
    }

    cursor_x = 0;
    cursor_y = 0;

    for (;;) {
        while (gfx_poll_event(&gfx, &key_event) > 0) {
            if (key_event.type == SAVANXP_INPUT_EVENT_KEY_DOWN && key_event.key == SAVANXP_KEY_ESC) {
                gfx_release(&gfx);
                close((int)mouse_fd);
                gfx_close(&gfx);
                return 0;
            }
        }

        while (mouse_poll_event((int)mouse_fd, &mouse_event) > 0) {
            cursor_x += mouse_event.delta_x;
            cursor_y += mouse_event.delta_y;
            delta_x = mouse_event.delta_x;
            delta_y = mouse_event.delta_y;
            buttons = mouse_event.buttons;

            if (cursor_x < 0) {
                cursor_x = 0;
            }
            if (cursor_y < 0) {
                cursor_y = 0;
            }
            if (cursor_x >= (int)gfx.info.width) {
                cursor_x = (int)gfx.info.width - 1;
            }
            if (cursor_y >= (int)gfx.info.height) {
                cursor_y = (int)gfx.info.height - 1;
            }

            needs_redraw = 1;
        }

        if (!needs_redraw) {
            sleep_ms(16);
            continue;
        }

        draw_scene(&gfx, cursor_x, cursor_y, delta_x, delta_y, buttons);
        if (gfx_present(&gfx, g_backbuffer) < 0) {
            break;
        }
        needs_redraw = 0;
    }

    gfx_release(&gfx);
    close((int)mouse_fd);
    gfx_close(&gfx);
    puts_fd(2, "mousetest: present failed\n");
    return 1;
}
