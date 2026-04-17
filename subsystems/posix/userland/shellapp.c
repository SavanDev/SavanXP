#include "shell_core.h"

#include "shared/version.h"

#define SHELLAPP_MAX_WIDTH 1920
#define SHELLAPP_MAX_HEIGHT 1080
#define SHELLAPP_HISTORY_LINES 256
#define SHELLAPP_LINE_LENGTH 160
#define SHELLAPP_MARGIN_X 16
#define SHELLAPP_MARGIN_Y 14
#define SHELLAPP_HEADER_HEIGHT 30
#define SHELLAPP_LINE_HEIGHT 18
#define SHELLAPP_CURSOR_PERIOD_MS 500UL
#define SHELLAPP_PRESENT_INTERVAL_MS 16UL

struct shellapp_line {
    char text[SHELLAPP_LINE_LENGTH];
    int stream;
};

struct shellapp_state {
    struct savanxp_gfx_context gfx;
    uint32_t* frame;
    char input[256];
    int line_count;
    int current_line_open;
    int cursor_visible;
    int needs_redraw;
    int force_full_present;
    struct sx_rect_set dirty_rects;
    unsigned long last_present_ms;
    unsigned long next_cursor_toggle_ms;
};

static uint32_t g_backbuffer[SHELLAPP_MAX_WIDTH * SHELLAPP_MAX_HEIGHT];
static struct shellapp_line g_lines[SHELLAPP_HISTORY_LINES];
static struct shellapp_state g_shellapp = {0};

static void shellapp_shift_history(void) {
    int index = 0;
    while (index + 1 < SHELLAPP_HISTORY_LINES) {
        g_lines[index] = g_lines[index + 1];
        ++index;
    }
    memset(&g_lines[SHELLAPP_HISTORY_LINES - 1], 0, sizeof(g_lines[SHELLAPP_HISTORY_LINES - 1]));
}

static struct shellapp_line* shellapp_push_line(int stream) {
    struct shellapp_line* line = 0;

    if (g_shellapp.line_count == SHELLAPP_HISTORY_LINES) {
        shellapp_shift_history();
        g_shellapp.line_count = SHELLAPP_HISTORY_LINES - 1;
    }

    line = &g_lines[g_shellapp.line_count++];
    memset(line, 0, sizeof(*line));
    line->stream = stream;
    g_shellapp.current_line_open = 1;
    return line;
}

static struct shellapp_line* shellapp_ensure_line(int stream) {
    struct shellapp_line* line = 0;

    if (g_shellapp.line_count == 0 || !g_shellapp.current_line_open) {
        return shellapp_push_line(stream);
    }

    line = &g_lines[g_shellapp.line_count - 1];
    if (line->stream != stream && line->text[0] != '\0') {
        return shellapp_push_line(stream);
    }

    line->stream = stream;
    return line;
}

static void shellapp_finish_line(void) {
    g_shellapp.current_line_open = 0;
}

static void shellapp_append_char(int stream, char value) {
    struct shellapp_line* line = 0;
    size_t length = 0;

    if (value == '\r') {
        return;
    }
    if (value == '\n') {
        (void)shellapp_ensure_line(stream);
        shellapp_finish_line();
        return;
    }
    if (value == '\t') {
        shellapp_append_char(stream, ' ');
        shellapp_append_char(stream, ' ');
        shellapp_append_char(stream, ' ');
        shellapp_append_char(stream, ' ');
        return;
    }
    if ((unsigned char)value < 32u || (unsigned char)value > 126u) {
        value = '?';
    }

    line = shellapp_ensure_line(stream);
    length = strlen(line->text);
    if (length + 1 >= sizeof(line->text)) {
        shellapp_finish_line();
        line = shellapp_push_line(stream);
        length = 0;
    }

    line->text[length] = value;
    line->text[length + 1] = '\0';
}

static void shellapp_append_text_line(int stream, const char* text) {
    while (text != 0 && *text != '\0') {
        shellapp_append_char(stream, *text++);
    }
    shellapp_finish_line();
}

static void shellapp_clear_history(void) {
    memset(g_lines, 0, sizeof(g_lines));
    g_shellapp.line_count = 0;
    g_shellapp.current_line_open = 0;
}

static int shellapp_prompt_y(const struct savanxp_fb_info* info) {
    return (int)info->height - SHELLAPP_MARGIN_Y - gfx_text_height();
}

static void shellapp_invalidate_rect(int x, int y, int width, int height) {
    struct sx_rect rect = sx_rect_make(x, y, width, height);
    struct sx_rect bounds = sx_rect_make(0, 0, (int)g_shellapp.gfx.info.width, (int)g_shellapp.gfx.info.height);

    if (width <= 0 || height <= 0) {
        return;
    }

    rect = sx_rect_intersect(rect, bounds);
    if (sx_rect_is_empty(rect)) {
        return;
    }

    (void)sx_rect_set_add(&g_shellapp.dirty_rects, rect);
    g_shellapp.needs_redraw = 1;
}

static void shellapp_invalidate_full(void) {
    g_shellapp.force_full_present = 1;
    shellapp_invalidate_rect(0, 0, (int)g_shellapp.gfx.info.width, (int)g_shellapp.gfx.info.height);
}

static void shellapp_invalidate_header(void) {
    shellapp_invalidate_rect(0, 0, (int)g_shellapp.gfx.info.width, SHELLAPP_HEADER_HEIGHT + 2);
}

static void shellapp_invalidate_content(void) {
    g_shellapp.force_full_present = 1;
    shellapp_invalidate_rect(
        0,
        SHELLAPP_HEADER_HEIGHT,
        (int)g_shellapp.gfx.info.width,
        (int)g_shellapp.gfx.info.height - SHELLAPP_HEADER_HEIGHT);
}

static void shellapp_invalidate_prompt(void) {
    const int y = shellapp_prompt_y(&g_shellapp.gfx.info) - 8;
    shellapp_invalidate_rect(0, y, (int)g_shellapp.gfx.info.width, (int)g_shellapp.gfx.info.height - y);
}

static void shellapp_redraw(void) {
    const struct savanxp_fb_info* info = &g_shellapp.gfx.info;
    const int prompt_y = shellapp_prompt_y(info);
    const int history_top = SHELLAPP_HEADER_HEIGHT + SHELLAPP_MARGIN_Y;
    const int history_bottom = prompt_y - SHELLAPP_LINE_HEIGHT;
    const int visible_lines = history_bottom > history_top ? (history_bottom - history_top) / SHELLAPP_LINE_HEIGHT : 0;
    const int first_line = g_shellapp.line_count > visible_lines ? g_shellapp.line_count - visible_lines : 0;
    char cwd[256] = {};
    int line_index = 0;
    int y = history_top;
    int cursor_x = 0;

    shell_current_directory(cwd, sizeof(cwd));
    gfx_clear(g_shellapp.frame, info, gfx_rgb(15, 19, 26));
    gfx_rect(g_shellapp.frame, info, 0, 0, (int)info->width, SHELLAPP_HEADER_HEIGHT, gfx_rgb(25, 36, 52));
    gfx_hline(g_shellapp.frame, info, 0, SHELLAPP_HEADER_HEIGHT, (int)info->width, gfx_rgb(76, 112, 156));
    gfx_blit_text(g_shellapp.frame, info, SHELLAPP_MARGIN_X, 8, SAVANXP_DISPLAY_NAME " shell", gfx_rgb(236, 243, 255));
    gfx_blit_text(g_shellapp.frame, info, (int)info->width - gfx_text_width(cwd) - SHELLAPP_MARGIN_X, 8, cwd, gfx_rgb(173, 201, 232));

    for (line_index = first_line; line_index < g_shellapp.line_count; ++line_index) {
        uint32_t colour = g_lines[line_index].stream == 2 ? gfx_rgb(255, 170, 170) : gfx_rgb(220, 233, 245);
        gfx_blit_text(g_shellapp.frame, info, SHELLAPP_MARGIN_X, y, g_lines[line_index].text, colour);
        y += SHELLAPP_LINE_HEIGHT;
        if (y + gfx_text_height() > prompt_y) {
            break;
        }
    }

    gfx_hline(g_shellapp.frame, info, 0, prompt_y - 8, (int)info->width, gfx_rgb(41, 58, 78));
    gfx_blit_text(g_shellapp.frame, info, SHELLAPP_MARGIN_X, prompt_y, "> ", gfx_rgb(128, 226, 164));
    gfx_blit_text(g_shellapp.frame, info, SHELLAPP_MARGIN_X + gfx_text_width("> "), prompt_y, g_shellapp.input, gfx_rgb(246, 248, 252));

    if (g_shellapp.cursor_visible) {
        cursor_x = SHELLAPP_MARGIN_X + gfx_text_width("> ") + gfx_text_width(g_shellapp.input);
        gfx_rect(g_shellapp.frame, info, cursor_x, prompt_y + gfx_text_height() + 1, 10, 2, gfx_rgb(128, 226, 164));
    }

    if (sx_rect_set_valid(&g_shellapp.dirty_rects)) {
        long present_result = 0;
        struct sx_rect bounds = sx_rect_set_bounds(&g_shellapp.dirty_rects);
        if (g_shellapp.force_full_present ||
            (g_shellapp.dirty_rects.count == 1 &&
            bounds.x == 0 &&
            bounds.y == 0 &&
            bounds.width == (int)info->width &&
            bounds.height == (int)info->height)) {
            present_result = gfx_present(&g_shellapp.gfx, g_shellapp.frame);
        } else {
            present_result = gfx_present_rects(
                &g_shellapp.gfx,
                g_shellapp.frame,
                g_shellapp.dirty_rects.rects,
                g_shellapp.dirty_rects.count);
        }
        if (present_result < 0) {
            exit(1);
        }
    }
    g_shellapp.needs_redraw = 0;
    g_shellapp.force_full_present = 0;
    sx_rect_set_clear(&g_shellapp.dirty_rects);
    g_shellapp.last_present_ms = uptime_ms();
}

static void shellapp_request_redraw(int immediate) {
    g_shellapp.needs_redraw = 1;
    if (immediate || g_shellapp.last_present_ms == 0 ||
        uptime_ms() - g_shellapp.last_present_ms >= SHELLAPP_PRESENT_INTERVAL_MS) {
        shellapp_redraw();
    }
}

static void shellapp_sink_emit(void* context, int fd, const char* bytes, size_t length) {
    size_t index = 0;
    (void)context;

    while (index < length) {
        shellapp_append_char(fd == 2 ? 2 : 1, bytes[index]);
        ++index;
    }

    shellapp_invalidate_content();
    shellapp_request_redraw(0);
}

static void shellapp_sink_clear(void* context) {
    (void)context;
    shellapp_clear_history();
    shellapp_invalidate_content();
    shellapp_request_redraw(1);
}

static void shellapp_submit_input(void) {
    struct shell_capture_sink sink = {
        .emit = shellapp_sink_emit,
        .clear = shellapp_sink_clear,
        .context = &g_shellapp,
    };
    char line[sizeof(g_shellapp.input)] = {};
    size_t index = 0;

    while (g_shellapp.input[index] != '\0' && index + 1 < sizeof(line)) {
        line[index] = g_shellapp.input[index];
        ++index;
    }
    line[index] = '\0';

    shellapp_append_text_line(1, g_shellapp.input[0] != '\0' ? g_shellapp.input : " ");
    if (g_lines[g_shellapp.line_count - 1].text[0] == ' ') {
        strcpy(g_lines[g_shellapp.line_count - 1].text, ">");
    } else {
        char prompt_line[SHELLAPP_LINE_LENGTH] = "> ";
        size_t prompt_length = 2;
        index = 0;
        while (line[index] != '\0' && prompt_length + 1 < sizeof(prompt_line)) {
            prompt_line[prompt_length++] = line[index++];
        }
        prompt_line[prompt_length] = '\0';
        strcpy(g_lines[g_shellapp.line_count - 1].text, prompt_line);
    }

    memset(g_shellapp.input, 0, sizeof(g_shellapp.input));
    g_shellapp.cursor_visible = 1;
    g_shellapp.next_cursor_toggle_ms = uptime_ms() + SHELLAPP_CURSOR_PERIOD_MS;
    shellapp_invalidate_content();
    shellapp_request_redraw(1);

    if (line[0] == '\0') {
        return;
    }

    if (shell_execute_line(line, SHELL_EXEC_CAPTURE, &sink) == SHELL_EXEC_RESULT_EXIT) {
        gfx_close(&g_shellapp.gfx);
        exit(0);
    }

    shellapp_invalidate_header();
    shellapp_invalidate_content();
    shellapp_request_redraw(1);
}

static void shellapp_handle_key(const struct savanxp_input_event* event) {
    size_t length = strlen(g_shellapp.input);

    if (event->type != SAVANXP_INPUT_EVENT_KEY_DOWN) {
        return;
    }

    if (event->key == SAVANXP_KEY_BACKSPACE) {
        if (length > 0) {
            g_shellapp.input[length - 1] = '\0';
            shellapp_invalidate_prompt();
            g_shellapp.needs_redraw = 1;
        }
        return;
    }

    if (event->key == SAVANXP_KEY_ENTER) {
        shellapp_submit_input();
        return;
    }

    if (event->key == SAVANXP_KEY_TAB) {
        int spaces = 0;
        while (spaces < 4 && length + 1 < sizeof(g_shellapp.input)) {
            g_shellapp.input[length++] = ' ';
            ++spaces;
        }
        g_shellapp.input[length] = '\0';
        shellapp_invalidate_prompt();
        g_shellapp.needs_redraw = 1;
        return;
    }

    if (event->ascii >= 32 && event->ascii <= 126 && length + 1 < sizeof(g_shellapp.input)) {
        g_shellapp.input[length] = (char)event->ascii;
        g_shellapp.input[length + 1] = '\0';
        shellapp_invalidate_prompt();
        g_shellapp.needs_redraw = 1;
    }
}

int main(void) {
    struct savanxp_input_event event = {0};
    struct savanxp_pollfd pollfd = {0};

    memset(&g_shellapp, 0, sizeof(g_shellapp));
    if (gfx_open(&g_shellapp.gfx) < 0) {
        puts_fd(2, "shellapp: gfx_open failed\n");
        return 1;
    }
    if (g_shellapp.gfx.info.width > SHELLAPP_MAX_WIDTH ||
        g_shellapp.gfx.info.height > SHELLAPP_MAX_HEIGHT ||
        (g_shellapp.gfx.info.pitch / 4u) > SHELLAPP_MAX_WIDTH) {
        puts_fd(2, "shellapp: surface too large\n");
        gfx_close(&g_shellapp.gfx);
        return 1;
    }
    if (gfx_acquire(&g_shellapp.gfx) < 0) {
        puts_fd(2, "shellapp: gfx_acquire failed\n");
        gfx_close(&g_shellapp.gfx);
        return 1;
    }

    g_shellapp.frame = g_shellapp.gfx.pixels != 0 ? g_shellapp.gfx.pixels : g_backbuffer;
    g_shellapp.cursor_visible = 1;
    g_shellapp.next_cursor_toggle_ms = uptime_ms() + SHELLAPP_CURSOR_PERIOD_MS;
    shellapp_append_text_line(1, SAVANXP_DISPLAY_NAME " shell app");
    shellapp_append_text_line(1, "Desktop compositor session. Press Super for the launcher.");
    shellapp_append_text_line(1, "Builtins keep state here; external commands run through /bin/sh -c.");
    shellapp_invalidate_full();

    for (;;) {
        long timeout_ms = (long)(g_shellapp.next_cursor_toggle_ms > uptime_ms()
            ? (g_shellapp.next_cursor_toggle_ms - uptime_ms())
            : 0UL);
        long ready = 0;

        if (g_shellapp.needs_redraw) {
            shellapp_redraw();
        }

        pollfd.fd = g_shellapp.gfx.input_fd;
        pollfd.events = SAVANXP_POLLIN | SAVANXP_POLLHUP;
        pollfd.revents = 0;
        ready = poll(&pollfd, 1, timeout_ms);
        if (ready < 0) {
            break;
        }
        if (ready == 0) {
            g_shellapp.cursor_visible = !g_shellapp.cursor_visible;
            g_shellapp.next_cursor_toggle_ms = uptime_ms() + SHELLAPP_CURSOR_PERIOD_MS;
            shellapp_invalidate_prompt();
            continue;
        }
        if ((pollfd.revents & SAVANXP_POLLHUP) != 0) {
            break;
        }
        if ((pollfd.revents & SAVANXP_POLLIN) == 0) {
            continue;
        }

        if (gfx_poll_event(&g_shellapp.gfx, &event) <= 0) {
            break;
        }

        g_shellapp.cursor_visible = 1;
        g_shellapp.next_cursor_toggle_ms = uptime_ms() + SHELLAPP_CURSOR_PERIOD_MS;
        shellapp_invalidate_prompt();
        shellapp_handle_key(&event);
    }

    gfx_close(&g_shellapp.gfx);
    return 0;
}
