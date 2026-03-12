#include "libc.h"

#define GFX_MAX_WIDTH 1920
#define GFX_MAX_HEIGHT 1080
#define KEYTEST_EVENT_LINES 20
#define KEYTEST_LINE_LENGTH 96

static uint32_t g_backbuffer[GFX_MAX_WIDTH * GFX_MAX_HEIGHT];

struct keytest_event_line {
    char text[KEYTEST_LINE_LENGTH];
};

static struct keytest_event_line g_lines[KEYTEST_EVENT_LINES];
static unsigned int g_line_count = 0;
static unsigned int g_event_counter = 0;

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

static int append_ascii_value(char* buffer, int offset, int capacity, int value) {
    if (value < 0) {
        offset = append_char(buffer, offset, capacity, '-');
        value = -value;
    }
    return append_uint(buffer, offset, capacity, (unsigned int)value);
}

static int append_ascii_repr(char* buffer, int offset, int capacity, int ascii) {
    if (ascii == 0) {
        return append_text(buffer, offset, capacity, "none");
    }
    if (ascii == '\n') {
        return append_text(buffer, offset, capacity, "\\n");
    }
    if (ascii == '\t') {
        return append_text(buffer, offset, capacity, "\\t");
    }
    if (ascii == '\b') {
        return append_text(buffer, offset, capacity, "\\b");
    }
    if (ascii < 32 || ascii > 126) {
        offset = append_text(buffer, offset, capacity, "0x");
        if (ascii < 16) {
            offset = append_char(buffer, offset, capacity, '0');
        }
        if (ascii < 0) {
            ascii = 0;
        }
        if ((ascii / 16) < 10) {
            offset = append_char(buffer, offset, capacity, (char)('0' + ((ascii / 16) % 16)));
        } else {
            offset = append_char(buffer, offset, capacity, (char)('a' + ((ascii / 16) % 16) - 10));
        }
        if ((ascii % 16) < 10) {
            offset = append_char(buffer, offset, capacity, (char)('0' + (ascii % 16)));
        } else {
            offset = append_char(buffer, offset, capacity, (char)('a' + (ascii % 16) - 10));
        }
        return offset;
    }

    offset = append_char(buffer, offset, capacity, '\'');
    offset = append_char(buffer, offset, capacity, (char)ascii);
    offset = append_char(buffer, offset, capacity, '\'');
    return offset;
}

static const char* key_name(unsigned int key) {
    switch (key) {
        case SAVANXP_KEY_NONE: return "NONE";
        case SAVANXP_KEY_BACKSPACE: return "BACKSPACE";
        case SAVANXP_KEY_TAB: return "TAB";
        case SAVANXP_KEY_ENTER: return "ENTER";
        case SAVANXP_KEY_ESC: return "ESC";
        case SAVANXP_KEY_UP: return "UP";
        case SAVANXP_KEY_DOWN: return "DOWN";
        case SAVANXP_KEY_LEFT: return "LEFT";
        case SAVANXP_KEY_RIGHT: return "RIGHT";
        case SAVANXP_KEY_SHIFT: return "SHIFT";
        case SAVANXP_KEY_CTRL: return "CTRL";
        case SAVANXP_KEY_ALT: return "ALT";
        case SAVANXP_KEY_CAPSLOCK: return "CAPSLOCK";
        case SAVANXP_KEY_HOME: return "HOME";
        case SAVANXP_KEY_END: return "END";
        case SAVANXP_KEY_PAGE_UP: return "PAGEUP";
        case SAVANXP_KEY_PAGE_DOWN: return "PAGEDOWN";
        case SAVANXP_KEY_INSERT: return "INSERT";
        case SAVANXP_KEY_DELETE: return "DELETE";
        case SAVANXP_KEY_F1: return "F1";
        case SAVANXP_KEY_F2: return "F2";
        case SAVANXP_KEY_F3: return "F3";
        case SAVANXP_KEY_F4: return "F4";
        case SAVANXP_KEY_F5: return "F5";
        case SAVANXP_KEY_F6: return "F6";
        case SAVANXP_KEY_F7: return "F7";
        case SAVANXP_KEY_F8: return "F8";
        case SAVANXP_KEY_F9: return "F9";
        case SAVANXP_KEY_F10: return "F10";
        case SAVANXP_KEY_F11: return "F11";
        case SAVANXP_KEY_F12: return "F12";
        case SAVANXP_KEY_PRINT_SCREEN: return "PRINT_SCREEN";
        case SAVANXP_KEY_PAUSE: return "PAUSE";
        case SAVANXP_KEY_NUMLOCK: return "NUMLOCK";
        case SAVANXP_KEY_SCROLLLOCK: return "SCROLLLOCK";
        case SAVANXP_KEY_SUPER: return "SUPER";
        case SAVANXP_KEY_MENU: return "MENU";
        case SAVANXP_KEY_ALT_GR: return "ALTGR";
        default:
            return "CHAR";
    }
}

static void clear_lines(void) {
    unsigned int index;
    for (index = 0; index < KEYTEST_EVENT_LINES; ++index) {
        memset(g_lines[index].text, 0, sizeof(g_lines[index].text));
    }
    g_line_count = 0;
}

static void push_event_line(const struct savanxp_input_event* event) {
    unsigned int index;
    char line[KEYTEST_LINE_LENGTH];
    int offset = 0;

    memset(line, 0, sizeof(line));
    g_event_counter += 1;

    offset = append_char(line, offset, (int)sizeof(line), '#');
    offset = append_uint(line, offset, (int)sizeof(line), g_event_counter);
    offset = append_text(line, offset, (int)sizeof(line), " ");
    offset = append_text(
        line,
        offset,
        (int)sizeof(line),
        event->type == SAVANXP_INPUT_EVENT_KEY_DOWN ? "DOWN " : "UP   ");
    offset = append_text(line, offset, (int)sizeof(line), key_name(event->key));
    offset = append_text(line, offset, (int)sizeof(line), " key=");
    offset = append_uint(line, offset, (int)sizeof(line), event->key);
    offset = append_text(line, offset, (int)sizeof(line), " ascii=");
    offset = append_ascii_repr(line, offset, (int)sizeof(line), event->ascii);
    offset = append_text(line, offset, (int)sizeof(line), " (");
    offset = append_ascii_value(line, offset, (int)sizeof(line), event->ascii);
    offset = append_char(line, offset, (int)sizeof(line), ')');

    if (g_line_count < KEYTEST_EVENT_LINES) {
        strcpy(g_lines[g_line_count].text, line);
        g_line_count += 1;
        return;
    }

    for (index = 1; index < KEYTEST_EVENT_LINES; ++index) {
        strcpy(g_lines[index - 1].text, g_lines[index].text);
    }
    strcpy(g_lines[KEYTEST_EVENT_LINES - 1].text, line);
}

static void draw_scene(struct savanxp_gfx_context* gfx) {
    int title_x = ((int)gfx->info.width - gfx_text_width("SavanXP keyboard test")) / 2;
    unsigned int index;
    int y = 62;

    gfx_clear(g_backbuffer, &gfx->info, gfx_rgb(12, 18, 28));
    gfx_rect(g_backbuffer, &gfx->info, 0, 0, (int)gfx->info.width, 34, gfx_rgb(26, 49, 71));
    gfx_hline(g_backbuffer, &gfx->info, 0, 34, (int)gfx->info.width, gfx_rgb(99, 166, 214));
    gfx_blit_text(g_backbuffer, &gfx->info, title_x, 8, "SavanXP keyboard test", gfx_rgb(234, 244, 255));

    gfx_rect(g_backbuffer, &gfx->info, 24, 48, (int)gfx->info.width - 48, (int)gfx->info.height - 72, gfx_rgb(19, 29, 42));
    gfx_frame(g_backbuffer, &gfx->info, 24, 48, (int)gfx->info.width - 48, (int)gfx->info.height - 72, gfx_rgb(74, 122, 163));
    gfx_blit_text(g_backbuffer, &gfx->info, 40, 62 - 22, "ESC exits  C clears  Try AltGr, locks, keypad, Pause", gfx_rgb(186, 211, 235));
    gfx_blit_text(g_backbuffer, &gfx->info, 40, 62 - 4, "On some hosts/QEMU setups, ImpPnt reaches the guest as Alt+ImpPnt", gfx_rgb(186, 211, 235));

    if (g_line_count == 0) {
        gfx_blit_text(g_backbuffer, &gfx->info, 48, y, "Waiting for keyboard events...", gfx_rgb(208, 226, 241));
        return;
    }

    for (index = 0; index < g_line_count; ++index) {
        gfx_blit_text(g_backbuffer, &gfx->info, 48, y, g_lines[index].text, gfx_rgb(208, 226, 241));
        y += 18;
        if (y + gfx_text_height() > (int)gfx->info.height - 24) {
            break;
        }
    }
}

int main(void) {
    struct savanxp_gfx_context gfx;
    struct savanxp_input_event event;

    clear_lines();

    if (gfx_open(&gfx) < 0) {
        puts_fd(2, "keytest: open failed\n");
        return 1;
    }
    if (gfx.info.width > GFX_MAX_WIDTH || gfx.info.height > GFX_MAX_HEIGHT || (gfx.info.pitch / 4u) > GFX_MAX_WIDTH) {
        puts_fd(2, "keytest: framebuffer too large\n");
        gfx_close(&gfx);
        return 1;
    }
    if (gfx_acquire(&gfx) < 0) {
        puts_fd(2, "keytest: acquire failed\n");
        gfx_close(&gfx);
        return 1;
    }

    for (;;) {
        while (gfx_poll_event(&gfx, &event) > 0) {
            if (event.type == SAVANXP_INPUT_EVENT_KEY_DOWN && event.key == SAVANXP_KEY_ESC) {
                gfx_release(&gfx);
                gfx_close(&gfx);
                return 0;
            }
            if (event.type == SAVANXP_INPUT_EVENT_KEY_DOWN &&
                (event.key == 'c' || event.key == 'C') &&
                (event.ascii == 'c' || event.ascii == 'C')) {
                clear_lines();
                continue;
            }
            push_event_line(&event);
        }

        draw_scene(&gfx);
        if (gfx_present(&gfx, g_backbuffer) < 0) {
            break;
        }
        sleep_ms(16);
    }

    gfx_release(&gfx);
    gfx_close(&gfx);
    puts_fd(2, "keytest: present failed\n");
    return 1;
}
