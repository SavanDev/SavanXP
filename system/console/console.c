/* Command Prompt: the console host (docs/desktop/CONSOLE.md).
 *
 * The first application written against SUN (docs/userland/SUN.md): a
 * native process that owns the window, the screen buffer and the keyboard,
 * and runs /bin/sh as a separate POSIX child over three pipes -- conhost and
 * cmd.exe. It never parses a command; sh never draws a pixel.
 *
 *   sh's 0  <- input pipe    the lines the user enters
 *   sh's 1  -> output pipe   what sh and its children write
 *   sh's 2  -> output pipe   the same, so stdout and stderr keep their order
 *   sh's 3  -> control pipe  "fg <pids>", "fg"
 *
 * One sxn_wait_many covers the window, the output and the control channel,
 * so the window answers while a command runs.
 */

#include <stdint.h>
#include <string.h>

#include "sxgfx/gfx2d.h"
#include "sxgfx/sxchrome.h"
#include "savanxp/syscall.h"
#include "savanxp/wm_protocol.h"
#include "sxn/sxn.h"
#include "sxn/sxnwm.h"

#include "console.sunres.h"

/* The Windows 2000 defaults: an 80 x 300 screen buffer seen through an 80 x 25
 * window. */
#define CON_COLUMNS 80
#define CON_BUFFER_LINES 300
#define CON_WINDOW_LINES 25
#define CON_SCROLLBAR 16
#define CON_INPUT_CAPACITY 255
#define CON_HISTORY 50
#define CON_BLINK_NS 500000000ull
#define CON_CONTROL_CAPACITY 256
#define CON_MAX_FOREGROUND 8
#define CON_WHEEL_LINES 3

/* Attribute 07: light grey on black. */
#define CON_FOREGROUND 0x00C0C0C0u
#define CON_BACKGROUND 0x00000000u

#define CON_SHELL_PATH "/bin/sh"
/* The status a process ended by Ctrl+C reports: 128 + SIGINT, as a POSIX
 * shell would see it. */
#define CON_INTERRUPT_STATUS 130

struct console {
    struct sxnwm_client wm;
    int cell_width;
    int cell_height;
    int window_lines;

    /* Screen buffer: a ring of lines. Lines are numbered from the start of the
     * session (absolute); line L lives in cells[L % CON_BUFFER_LINES]. 0 is a
     * blank cell. */
    uint32_t cells[CON_BUFFER_LINES][CON_COLUMNS];
    uint8_t dirty[CON_BUFFER_LINES];
    /* The oldest line still in the ring. Only grows: erasing an echo lowers
     * last_line, and must not bring back a line whose row was reused. */
    long oldest_line;
    long last_line;
    long cursor_line;
    long cursor_col;
    long view_top;
    int follow;
    int full_repaint;

    /* Output decoding: UTF-8 and the escape sequences we skip. */
    uint32_t utf8_value;
    int utf8_pending;
    int escape_state;

    /* The line being edited, echoed into the buffer from echo_line/echo_col. */
    uint32_t input[CON_INPUT_CAPACITY];
    int input_length;
    int caret;
    int overwrite;
    long echo_line;
    long echo_col;
    long echo_end_line;
    long echo_end_col;

    char history[CON_HISTORY][CON_INPUT_CAPACITY * 4 + 1];
    int history_count;
    int history_cursor;

    int caret_on;
    uint64_t next_blink_ns;

    /* The shell and its pipes. */
    long shell_pid;
    long input_write;
    long output_read;
    long control_read;
    char control[CON_CONTROL_CAPACITY];
    int control_length;
    long foreground[CON_MAX_FOREGROUND];
    int foreground_count;

    /* Scroll bar drag. */
    uint32_t buttons;
    int dragging;
    int drag_offset;
};

static struct console g_con;

/* --- Screen buffer ----------------------------------------------------- */

static long first_line(void) {
    return g_con.oldest_line;
}

static uint32_t* line_cells(long line) {
    return g_con.cells[line % CON_BUFFER_LINES];
}

static void mark_line(long line) {
    if (line >= 0) {
        g_con.dirty[line % CON_BUFFER_LINES] = 1;
    }
}

/* Moves the output cursor to the start of the next line, opening it. */
static void new_line(void) {
    g_con.cursor_col = 0;
    g_con.cursor_line += 1;
    if (g_con.cursor_line > g_con.last_line) {
        g_con.last_line = g_con.cursor_line;
        memset(line_cells(g_con.cursor_line), 0, sizeof(g_con.cells[0]));
        if (g_con.last_line - g_con.oldest_line >= CON_BUFFER_LINES) {
            g_con.oldest_line = g_con.last_line - CON_BUFFER_LINES + 1;
        }
    }
    mark_line(g_con.cursor_line);
}

/* Writes one character at the cursor. The wrap is deferred, as in the Windows
 * console: a full line wraps when the next character arrives, so a newline
 * right after the 80th column does not leave an empty line. */
static void put_codepoint(uint32_t value) {
    if (g_con.cursor_col >= CON_COLUMNS) {
        new_line();
    }
    line_cells(g_con.cursor_line)[g_con.cursor_col] = value;
    mark_line(g_con.cursor_line);
    g_con.cursor_col += 1;
}

static void clear_buffer(void) {
    memset(g_con.cells, 0, sizeof(g_con.cells));
    g_con.oldest_line = 0;
    g_con.last_line = 0;
    g_con.cursor_line = 0;
    g_con.cursor_col = 0;
    g_con.view_top = 0;
    g_con.follow = 1;
    g_con.full_repaint = 1;
}

/* One byte of output from the shell side. */
static void output_byte(uint8_t byte) {
    /* Escape sequences: a program written for a VT terminal must not leave
     * garbage on the screen. ESC [ ... final, and ESC ] ... BEL are skipped;
     * ESC [ 2 J clears, the one a POSIX `clear` would send. */
    if (g_con.escape_state == 1) {
        g_con.escape_state = byte == '[' ? 2 : (byte == ']' ? 4 : 0);
        return;
    }
    if (g_con.escape_state == 2 || g_con.escape_state == 3) {
        if (byte == '2' && g_con.escape_state == 2) {
            g_con.escape_state = 3;
            return;
        }
        if (byte >= 0x40 && byte <= 0x7e) {
            if (byte == 'J' && g_con.escape_state == 3) {
                clear_buffer();
            }
            g_con.escape_state = 0;
        } else {
            g_con.escape_state = 2;
        }
        return;
    }
    if (g_con.escape_state == 4) {
        if (byte == 0x07 || byte == 0x1b) {
            g_con.escape_state = 0;
        }
        return;
    }

    if (g_con.utf8_pending > 0) {
        if ((byte & 0xc0) == 0x80) {
            g_con.utf8_value = (g_con.utf8_value << 6) | (byte & 0x3f);
            g_con.utf8_pending -= 1;
            if (g_con.utf8_pending == 0) {
                put_codepoint(g_con.utf8_value);
            }
            return;
        }
        g_con.utf8_pending = 0;
        put_codepoint('?');
    }
    if (byte >= 0x80) {
        if ((byte & 0xe0) == 0xc0) {
            g_con.utf8_value = byte & 0x1f;
            g_con.utf8_pending = 1;
        } else if ((byte & 0xf0) == 0xe0) {
            g_con.utf8_value = byte & 0x0f;
            g_con.utf8_pending = 2;
        } else if ((byte & 0xf8) == 0xf0) {
            g_con.utf8_value = byte & 0x07;
            g_con.utf8_pending = 3;
        } else {
            put_codepoint('?');
        }
        return;
    }

    switch (byte) {
        case '\n':
            new_line();
            return;
        case '\r':
            g_con.cursor_col = 0;
            return;
        case '\b':
            if (g_con.cursor_col > 0) {
                g_con.cursor_col -= 1;
            }
            return;
        case '\t': {
            long next = (g_con.cursor_col / 8 + 1) * 8;
            while (g_con.cursor_col < next && g_con.cursor_col < CON_COLUMNS) {
                put_codepoint(' ');
            }
            return;
        }
        case 0x07:
            return;
        case 0x1b:
            g_con.escape_state = 1;
            return;
        default:
            break;
    }
    if (byte < 0x20 || byte == 0x7f) {
        return;
    }
    put_codepoint(byte);
}

/* --- The line being edited ---------------------------------------------- */

/* Removes the echo of the edited line from the buffer and puts the output
 * cursor back where it began. */
static void echo_erase(void) {
    long line = g_con.echo_line;
    long col = g_con.echo_col;

    while (line < g_con.echo_end_line || (line == g_con.echo_end_line && col < g_con.echo_end_col)) {
        if (col >= CON_COLUMNS) {
            line += 1;
            col = 0;
            continue;
        }
        if (line >= first_line() && line <= g_con.last_line) {
            line_cells(line)[col] = 0;
            mark_line(line);
        }
        col += 1;
    }
    for (line = g_con.echo_line + 1; line <= g_con.last_line; ++line) {
        mark_line(line);
    }
    if (g_con.last_line > g_con.echo_line) {
        g_con.last_line = g_con.echo_line;
    }
    g_con.cursor_line = g_con.echo_line;
    g_con.cursor_col = g_con.echo_col;
}

/* Echoes the edited line at the output cursor. */
static void echo_write(void) {
    int index = 0;

    g_con.echo_line = g_con.cursor_line;
    g_con.echo_col = g_con.cursor_col;
    for (index = 0; index < g_con.input_length; ++index) {
        put_codepoint(g_con.input[index]);
    }
    g_con.echo_end_line = g_con.cursor_line;
    g_con.echo_end_col = g_con.cursor_col;
}

static void caret_position(long* line, long* col) {
    const long offset = g_con.echo_col + g_con.caret;
    *line = g_con.echo_line + offset / CON_COLUMNS;
    *col = offset % CON_COLUMNS;
}

static void mark_caret(void) {
    long line = 0;
    long col = 0;

    caret_position(&line, &col);
    mark_line(line);
}

static void caret_reset_blink(void) {
    g_con.caret_on = 1;
    g_con.next_blink_ns = sxn_clock_ns() + CON_BLINK_NS;
}

/* Output arrives: the echo steps aside, the output lands where the line was
 * being typed, and the echo goes back after it. */
static void output_bytes(const uint8_t* bytes, long count) {
    long index = 0;

    mark_caret();
    echo_erase();
    for (index = 0; index < count; ++index) {
        output_byte(bytes[index]);
    }
    echo_write();
    mark_caret();
}

static void output_text(const char* text) {
    long length = 0;
    while (text[length] != '\0') {
        ++length;
    }
    output_bytes((const uint8_t*)text, length);
}

static int encode_utf8(uint32_t value, char* out) {
    if (value < 0x80u) {
        out[0] = (char)value;
        return 1;
    }
    if (value < 0x800u) {
        out[0] = (char)(0xc0u | (value >> 6));
        out[1] = (char)(0x80u | (value & 0x3fu));
        return 2;
    }
    if (value < 0x10000u) {
        out[0] = (char)(0xe0u | (value >> 12));
        out[1] = (char)(0x80u | ((value >> 6) & 0x3fu));
        out[2] = (char)(0x80u | (value & 0x3fu));
        return 3;
    }
    out[0] = (char)(0xf0u | (value >> 18));
    out[1] = (char)(0x80u | ((value >> 12) & 0x3fu));
    out[2] = (char)(0x80u | ((value >> 6) & 0x3fu));
    out[3] = (char)(0x80u | (value & 0x3fu));
    return 4;
}

/* The edited line as UTF-8, without the newline. */
static int input_utf8(char* out, int capacity) {
    int length = 0;
    int index = 0;

    for (index = 0; index < g_con.input_length && length + 4 < capacity; ++index) {
        length += encode_utf8(g_con.input[index], out + length);
    }
    out[length] = '\0';
    return length;
}

static void edit_begin(void) {
    mark_caret();
    echo_erase();
}

static void edit_end(void) {
    echo_write();
    mark_caret();
    caret_reset_blink();
    g_con.follow = 1;
}

static void set_input_utf8(const char* text) {
    g_con.input_length = 0;
    while (*text != '\0' && g_con.input_length < CON_INPUT_CAPACITY) {
        const uint8_t byte = (uint8_t)*text;
        uint32_t value = byte;
        int extra = 0;

        if (byte >= 0xf0) {
            value = byte & 0x07;
            extra = 3;
        } else if (byte >= 0xe0) {
            value = byte & 0x0f;
            extra = 2;
        } else if (byte >= 0xc0) {
            value = byte & 0x1f;
            extra = 1;
        }
        ++text;
        while (extra > 0 && *text != '\0') {
            value = (value << 6) | ((uint8_t)*text & 0x3f);
            ++text;
            --extra;
        }
        g_con.input[g_con.input_length++] = value;
    }
    g_con.caret = g_con.input_length;
}

static void write_to_shell(const char* bytes, long length) {
    if (g_con.input_write >= 0 && length > 0) {
        (void)sxn_write(g_con.input_write, bytes, (unsigned long)length);
    }
}

/* Enter: the echo stays in the buffer as typed, the line goes to the shell. */
static void submit_line(void) {
    char line[CON_INPUT_CAPACITY * 4 + 2];
    int length = input_utf8(line, (int)sizeof(line) - 1);

    if (length > 0) {
        if (g_con.history_count == 0 ||
            strcmp(g_con.history[(g_con.history_count - 1) % CON_HISTORY], line) != 0) {
            memcpy(g_con.history[g_con.history_count % CON_HISTORY], line, (unsigned long)length + 1);
            g_con.history_count += 1;
        }
    }
    g_con.history_cursor = g_con.history_count;
    mark_caret();
    g_con.cursor_line = g_con.echo_end_line;
    g_con.cursor_col = g_con.echo_end_col;
    new_line();
    g_con.input_length = 0;
    g_con.caret = 0;
    echo_write();
    line[length] = '\n';
    write_to_shell(line, length + 1);
    caret_reset_blink();
    g_con.follow = 1;
}

/* Ctrl+C. With a foreground job its processes end, which is what Ctrl+C means
 * when the kernel has no signals; the shell is never one of them. Without a
 * job the line is abandoned and the shell is given an empty one, so it prints
 * a fresh prompt. Either way ^C stays on the screen, as in Windows. */
static void interrupt(void) {
    int index = 0;

    mark_caret();
    g_con.cursor_line = g_con.echo_end_line;
    g_con.cursor_col = g_con.echo_end_col;
    put_codepoint('^');
    put_codepoint('C');
    new_line();
    g_con.input_length = 0;
    g_con.caret = 0;
    echo_write();
    caret_reset_blink();
    g_con.follow = 1;

    if (g_con.foreground_count > 0) {
        for (index = 0; index < g_con.foreground_count; ++index) {
            (void)sxn_terminate(g_con.foreground[index], CON_INTERRUPT_STATUS);
        }
        return;
    }
    write_to_shell("\n", 1);
}

static void history_recall(int direction) {
    int oldest = g_con.history_count > CON_HISTORY ? g_con.history_count - CON_HISTORY : 0;
    int target = g_con.history_cursor + direction;

    if (g_con.history_count == 0) {
        return;
    }
    if (target < oldest) {
        target = oldest;
    }
    if (target >= g_con.history_count) {
        target = g_con.history_count - 1;
    }
    g_con.history_cursor = target;
    set_input_utf8(g_con.history[target % CON_HISTORY]);
}

/* F3: the rest of the previous line, past what is typed. */
static void history_rest(void) {
    uint32_t saved[CON_INPUT_CAPACITY];
    int saved_length = g_con.input_length;
    int index = 0;

    if (g_con.history_count == 0) {
        return;
    }
    memcpy(saved, g_con.input, sizeof(saved));
    set_input_utf8(g_con.history[(g_con.history_count - 1) % CON_HISTORY]);
    if (g_con.input_length < saved_length) {
        g_con.input_length = saved_length;
    }
    for (index = 0; index < saved_length; ++index) {
        g_con.input[index] = saved[index];
    }
    g_con.caret = g_con.input_length;
}

/* --- View and scroll bar --------------------------------------------------- */

static long bottom_line(void) {
    long line = 0;
    long col = 0;

    caret_position(&line, &col);
    return line > g_con.last_line ? line : g_con.last_line;
}

static long highest_view_top(void) {
    const long top = bottom_line() - g_con.window_lines + 1;
    return top > first_line() ? top : first_line();
}

static void scroll_to(long top) {
    const long highest = highest_view_top();

    if (top < first_line()) {
        top = first_line();
    }
    if (top > highest) {
        top = highest;
    }
    g_con.follow = top == highest;
    if (top != g_con.view_top) {
        g_con.view_top = top;
        g_con.full_repaint = 1;
    }
}

static void update_view(void) {
    if (g_con.follow) {
        scroll_to(highest_view_top());
    } else if (g_con.view_top < first_line()) {
        scroll_to(first_line());
    }
}

static struct sx_rect scrollbar_rect(void) {
    return sx_rect_make(CON_COLUMNS * g_con.cell_width, 0, CON_SCROLLBAR, (int)g_con.wm.height);
}

/* The thumb, or an empty rect when the whole buffer fits the window. */
static struct sx_rect scrollbar_thumb(void) {
    const struct sx_rect bar = scrollbar_rect();
    const long total = bottom_line() - first_line() + 1;
    const int track = bar.height - 2 * CON_SCROLLBAR;
    int size = 0;
    int position = 0;

    if (total <= g_con.window_lines || track <= 8) {
        return sx_rect_make(0, 0, 0, 0);
    }
    size = (int)((long)track * g_con.window_lines / total);
    if (size < 8) {
        size = 8;
    }
    position = (int)((long)(track - size) * (g_con.view_top - first_line()) / (total - g_con.window_lines));
    return sx_rect_make(bar.x, bar.y + CON_SCROLLBAR + position, CON_SCROLLBAR, size);
}

/* --- Painting -------------------------------------------------------------- */

static struct savanxp_fb_info surface_info(void) {
    struct savanxp_fb_info info = g_con.wm.header->info;
    info.width = g_con.wm.width;
    info.height = g_con.wm.height;
    return info;
}

static void paint_row(struct sx_painter* painter, int row) {
    const long line = g_con.view_top + row;
    const int y = row * g_con.cell_height;
    char text[CON_COLUMNS * 4 + 1];
    int length = 0;
    int used = 0;
    long caret_line = 0;
    long caret_col = 0;
    int column = 0;

    sx_painter_fill_rect(painter, sx_rect_make(0, y, CON_COLUMNS * g_con.cell_width, g_con.cell_height), CON_BACKGROUND);
    if (line <= g_con.last_line && line >= first_line()) {
        const uint32_t* cells = line_cells(line);
        for (column = 0; column < CON_COLUMNS; ++column) {
            if (cells[column] != 0) {
                used = column + 1;
            }
        }
        for (column = 0; column < used; ++column) {
            length += encode_utf8(cells[column] != 0 ? cells[column] : ' ', text + length);
        }
        text[length] = '\0';
        if (length > 0) {
            sx_painter_draw_text(painter, 0, y, text, CON_FOREGROUND);
        }
    }

    caret_position(&caret_line, &caret_col);
    if (g_con.caret_on && caret_line == line) {
        const int x = (int)caret_col * g_con.cell_width;
        if (g_con.overwrite) {
            /* Overwrite mode: the big cursor, the character inverted on it. */
            sx_painter_fill_rect(painter, sx_rect_make(x, y, g_con.cell_width, g_con.cell_height), CON_FOREGROUND);
            if (line <= g_con.last_line && line_cells(line)[caret_col] != 0) {
                char glyph[5];
                glyph[encode_utf8(line_cells(line)[caret_col], glyph)] = '\0';
                sx_painter_draw_text(painter, x, y, glyph, CON_BACKGROUND);
            }
        } else {
            sx_painter_fill_rect(painter, sx_rect_make(x, y + g_con.cell_height - 3, g_con.cell_width, 2), CON_FOREGROUND);
        }
    }
}

static void paint_scrollbar(struct sx_painter* painter) {
    const struct sx_rect bar = scrollbar_rect();
    const struct sx_rect up = sx_rect_make(bar.x, bar.y, CON_SCROLLBAR, CON_SCROLLBAR);
    const struct sx_rect down = sx_rect_make(bar.x, bar.y + bar.height - CON_SCROLLBAR, CON_SCROLLBAR, CON_SCROLLBAR);
    const struct sx_rect track = sx_rect_make(bar.x, bar.y + CON_SCROLLBAR, CON_SCROLLBAR, bar.height - 2 * CON_SCROLLBAR);
    const struct sx_rect thumb = scrollbar_thumb();
    const int enabled = !sx_rect_is_empty(thumb);
    const uint32_t arrow = enabled ? SXCHROME_COLOR_TEXT : SXCHROME_COLOR_DISABLED_TEXT;
    struct sx_brush rail = sx_brush_pattern(sx_pattern_checker_50, SXCHROME_COLOR_LIGHT, SXCHROME_COLOR_FACE);

    sx_painter_fill_rect_brush(painter, track, &rail);
    sxchrome_fill_raised(painter, up, SXCHROME_COLOR_FACE, 0);
    sxchrome_fill_raised(painter, down, SXCHROME_COLOR_FACE, 0);
    sxchrome_draw_arrow(painter, up, SXCHROME_ARROW_UP, arrow);
    sxchrome_draw_arrow(painter, down, SXCHROME_ARROW_DOWN, arrow);
    if (enabled) {
        sxchrome_fill_raised(painter, thumb, SXCHROME_COLOR_FACE, 0);
    }
}

/* Repaints what changed and presents it: the dirty rows, and the scroll bar
 * whenever anything moved. */
static void paint(void) {
    struct savanxp_fb_info info;
    struct sx_bitmap bitmap;
    struct sx_painter painter;
    struct savanxp_gpu_dirty_rect rects[SAVANXP_GPU_CLIENT_BATCH_MAX_RECTS];
    uint32_t rect_count = 0;
    int full = g_con.full_repaint;
    int any = 0;
    int row = 0;

    update_view();
    full = full || g_con.full_repaint;
    for (row = 0; row < g_con.window_lines && !any; ++row) {
        if (g_con.dirty[(g_con.view_top + row) % CON_BUFFER_LINES]) {
            any = 1;
        }
    }
    if (!full && !any) {
        return;
    }
    if (sxnwm_wait_idle(&g_con.wm, 200) != 0) {
        return;
    }

    info = surface_info();
    sx_bitmap_wrap(&bitmap, g_con.wm.pixels, &info, SX_PIXEL_FORMAT_BGRX8888);
    sx_painter_init(&painter, &bitmap);
    (void)sx_painter_set_font(&painter, SX_FONT_MONO);

    if (full) {
        sx_painter_fill_rect(&painter, sx_rect_make(0, 0, (int)info.width, (int)info.height), CON_BACKGROUND);
    }
    for (row = 0; row < g_con.window_lines; ++row) {
        const int slot = (int)((g_con.view_top + row) % CON_BUFFER_LINES);
        if (!full && !g_con.dirty[slot]) {
            continue;
        }
        paint_row(&painter, row);
        if (!full && rect_count + 1 < SAVANXP_GPU_CLIENT_BATCH_MAX_RECTS) {
            rects[rect_count].x = 0;
            rects[rect_count].y = (uint32_t)(row * g_con.cell_height);
            rects[rect_count].width = (uint32_t)(CON_COLUMNS * g_con.cell_width);
            rects[rect_count].height = (uint32_t)g_con.cell_height;
            rect_count += 1;
        } else {
            full = 1;
        }
    }
    paint_scrollbar(&painter);
    memset(g_con.dirty, 0, sizeof(g_con.dirty));
    g_con.full_repaint = 0;

    if (full) {
        (void)sxnwm_submit_full(&g_con.wm);
        return;
    }
    rects[rect_count].x = (uint32_t)scrollbar_rect().x;
    rects[rect_count].y = 0;
    rects[rect_count].width = CON_SCROLLBAR;
    rects[rect_count].height = g_con.wm.height;
    rect_count += 1;
    (void)sxnwm_submit_rects(&g_con.wm, rects, rect_count);
}

/* --- Input from the window ------------------------------------------------- */

static void handle_key(const struct savanxp_input_event* event) {
    const uint32_t modifiers = event->modifiers;
    const int ctrl = (modifiers & SAVANXP_KEY_MOD_CTRL) != 0 && (modifiers & SAVANXP_KEY_MOD_ALT_GR) == 0;
    const int alt = (modifiers & SAVANXP_KEY_MOD_ALT) != 0 && (modifiers & SAVANXP_KEY_MOD_ALT_GR) == 0;

    if (event->type != SAVANXP_INPUT_EVENT_KEY_DOWN) {
        return;
    }

    if (ctrl && (event->key == 'c' || event->key == 'C')) {
        interrupt();
        return;
    }

    switch (event->key) {
        case SAVANXP_KEY_PAGE_UP:
            scroll_to(g_con.view_top - (g_con.window_lines - 1));
            return;
        case SAVANXP_KEY_PAGE_DOWN:
            scroll_to(g_con.view_top + (g_con.window_lines - 1));
            return;
        case SAVANXP_KEY_ENTER:
            submit_line();
            return;
        default:
            break;
    }

    edit_begin();
    switch (event->key) {
        case SAVANXP_KEY_LEFT:
            if (g_con.caret > 0) {
                g_con.caret -= 1;
            }
            break;
        case SAVANXP_KEY_RIGHT:
            if (g_con.caret < g_con.input_length) {
                g_con.caret += 1;
            }
            break;
        case SAVANXP_KEY_HOME:
            g_con.caret = 0;
            break;
        case SAVANXP_KEY_END:
            g_con.caret = g_con.input_length;
            break;
        case SAVANXP_KEY_BACKSPACE:
            if (g_con.caret > 0) {
                memmove(&g_con.input[g_con.caret - 1], &g_con.input[g_con.caret],
                    (unsigned long)(g_con.input_length - g_con.caret) * sizeof(uint32_t));
                g_con.caret -= 1;
                g_con.input_length -= 1;
            }
            break;
        case SAVANXP_KEY_DELETE:
            if (g_con.caret < g_con.input_length) {
                memmove(&g_con.input[g_con.caret], &g_con.input[g_con.caret + 1],
                    (unsigned long)(g_con.input_length - g_con.caret - 1) * sizeof(uint32_t));
                g_con.input_length -= 1;
            }
            break;
        case SAVANXP_KEY_INSERT:
            g_con.overwrite = !g_con.overwrite;
            break;
        case SAVANXP_KEY_ESC:
            g_con.input_length = 0;
            g_con.caret = 0;
            break;
        case SAVANXP_KEY_UP:
            history_recall(-1);
            break;
        case SAVANXP_KEY_DOWN:
            history_recall(1);
            break;
        case SAVANXP_KEY_F3:
            history_rest();
            break;
        default: {
            const uint32_t value = (uint8_t)event->ascii;
            if (ctrl || alt || value < 0x20 || value == 0x7f) {
                break;
            }
            if (g_con.overwrite && g_con.caret < g_con.input_length) {
                g_con.input[g_con.caret++] = value;
            } else if (g_con.input_length < CON_INPUT_CAPACITY) {
                memmove(&g_con.input[g_con.caret + 1], &g_con.input[g_con.caret],
                    (unsigned long)(g_con.input_length - g_con.caret) * sizeof(uint32_t));
                g_con.input[g_con.caret++] = value;
                g_con.input_length += 1;
            }
            break;
        }
    }
    edit_end();
}

static void handle_pointer(const struct savanxp_gui_pointer_event* pointer) {
    const struct sx_rect bar = scrollbar_rect();
    const uint32_t pressed = pointer->buttons & ~g_con.buttons;
    const int inside_bar = sx_rect_contains_point(bar, pointer->x, pointer->y);

    if (pointer->wheel != 0) {
        scroll_to(g_con.view_top - (long)pointer->wheel * CON_WHEEL_LINES);
    }
    if ((pointer->buttons & SAVANXP_MOUSE_BUTTON_LEFT) == 0) {
        g_con.dragging = 0;
    } else if (g_con.dragging) {
        const struct sx_rect thumb = scrollbar_thumb();
        const int track = bar.height - 2 * CON_SCROLLBAR - thumb.height;
        const long total = bottom_line() - first_line() + 1;
        if (track > 0) {
            const int position = pointer->y - bar.y - CON_SCROLLBAR - g_con.drag_offset;
            scroll_to(first_line() + (long)position * (total - g_con.window_lines) / track);
        }
    }
    if ((pressed & SAVANXP_MOUSE_BUTTON_LEFT) != 0 && inside_bar) {
        const struct sx_rect thumb = scrollbar_thumb();
        if (pointer->y < bar.y + CON_SCROLLBAR) {
            scroll_to(g_con.view_top - 1);
        } else if (pointer->y >= bar.y + bar.height - CON_SCROLLBAR) {
            scroll_to(g_con.view_top + 1);
        } else if (!sx_rect_is_empty(thumb) && pointer->y < thumb.y) {
            scroll_to(g_con.view_top - (g_con.window_lines - 1));
        } else if (!sx_rect_is_empty(thumb) && pointer->y >= thumb.y + thumb.height) {
            scroll_to(g_con.view_top + (g_con.window_lines - 1));
        } else if (!sx_rect_is_empty(thumb)) {
            g_con.dragging = 1;
            g_con.drag_offset = pointer->y - thumb.y;
        }
    }
    g_con.buttons = pointer->buttons;
}

static void drain_window(void) {
    struct savanxp_wm_event record;

    if (sxnwm_check_resize(&g_con.wm) > 0) {
        g_con.window_lines = (int)g_con.wm.height / g_con.cell_height;
        if (g_con.window_lines < 1) {
            g_con.window_lines = 1;
        }
        g_con.full_repaint = 1;
    }

    while (sxnwm_poll_input(&g_con.wm, &record) == 0) {
        if (record.window_id != 0) {
            continue;
        }
        if (record.kind == SAVANXP_WM_EVENT_KEY) {
            handle_key(&record.payload.key);
        } else if (record.kind == SAVANXP_WM_EVENT_POINTER) {
            handle_pointer(&record.payload.pointer);
        }
    }
}

/* --- The shell side --------------------------------------------------------- */

static void set_title_for_foreground(void) {
    char title[SAVANXP_WM_PRESENTATION_TITLE_CAPACITY];
    struct sxn_process_query query;
    const char* base = sun_resources.title;
    int length = 0;
    int index = 0;

    while (base[length] != '\0' && length + 1 < (int)sizeof(title)) {
        title[length] = base[length];
        ++length;
    }
    /* "Command Prompt - ping", as Windows names the running program. */
    if (g_con.foreground_count > 0 && sxn_process_query(g_con.foreground[0], &query) == 0) {
        const char* separator = " - ";
        for (index = 0; separator[index] != '\0' && length + 1 < (int)sizeof(title); ++index) {
            title[length++] = separator[index];
        }
        for (index = 0; query.name[index] != '\0' && length + 1 < (int)sizeof(title); ++index) {
            title[length++] = query.name[index];
        }
    }
    title[length] = '\0';
    (void)sxnwm_set_title(&g_con.wm, title);
}

static void control_line(char* line) {
    if (line[0] == 'f' && line[1] == 'g' && (line[2] == '\0' || line[2] == ' ')) {
        const char* cursor = line + 2;
        g_con.foreground_count = 0;
        while (*cursor != '\0' && g_con.foreground_count < CON_MAX_FOREGROUND) {
            long value = 0;
            while (*cursor == ' ') {
                ++cursor;
            }
            if (*cursor < '0' || *cursor > '9') {
                break;
            }
            while (*cursor >= '0' && *cursor <= '9') {
                value = value * 10 + (*cursor - '0');
                ++cursor;
            }
            g_con.foreground[g_con.foreground_count++] = value;
        }
        set_title_for_foreground();
    }
}

/* Reads what the shell side wrote: the output, then the control channel.
 * Returns 0 once the control channel ended, which is the shell exiting (its children do not
 * inherit fd 3, so nothing else holds it open). */
static int drain_shell(void) {
    uint8_t buffer[2048];
    long count = 0;

    if (g_con.output_read >= 0) {
        for (;;) {
            count = sxn_read(g_con.output_read, buffer, sizeof(buffer));
            if (count <= 0) {
                break;
            }
            output_bytes(buffer, count);
        }
        if (count == 0) {
            (void)sxn_close(g_con.output_read);
            g_con.output_read = -1;
        }
    }
    if (g_con.control_read < 0) {
        return 0;
    }
    for (;;) {
        int index = 0;
        int start = 0;

        count = sxn_read(g_con.control_read, g_con.control + g_con.control_length,
            (unsigned long)(CON_CONTROL_CAPACITY - 1 - g_con.control_length));
        if (count == 0) {
            (void)sxn_close(g_con.control_read);
            g_con.control_read = -1;
            return 0;
        }
        if (count < 0) {
            return 1;
        }
        g_con.control_length += (int)count;
        for (index = 0; index < g_con.control_length; ++index) {
            if (g_con.control[index] == '\n') {
                g_con.control[index] = '\0';
                control_line(g_con.control + start);
                start = index + 1;
            }
        }
        if (start == 0 && g_con.control_length == CON_CONTROL_CAPACITY - 1) {
            /* A line longer than any the protocol has: drop it. */
            start = g_con.control_length;
        }
        memmove(g_con.control, g_con.control + start, (unsigned long)(g_con.control_length - start));
        g_con.control_length -= start;
    }
}

/* Starts sh with the three pipes. -errno if it could not. */
static long start_shell(void) {
    int32_t input[2] = {-1, -1};
    int32_t output[2] = {-1, -1};
    int32_t control[2] = {-1, -1};
    char* argv[3];
    struct sxn_spawn_request request;
    long pid = 0;

    if (sxn_pipe_create(input) != 0 || sxn_pipe_create(output) != 0 || sxn_pipe_create(control) != 0) {
        return -SAVANXP_ENOMEM;
    }
    argv[0] = CON_SHELL_PATH;
    argv[1] = "--console";
    argv[2] = "3";
    sxn_spawn_request_init(&request, CON_SHELL_PATH, argv, 3);
    request.std_handles[0] = input[0];
    request.std_handles[1] = output[1];
    request.std_handles[2] = output[1];
    request.handles[0] = control[1];
    request.handle_count = 1;
    pid = sxn_spawn_ex(&request);
    /* The child holds its own ends; ours go, so its exit is end of stream. */
    (void)sxn_close(input[0]);
    (void)sxn_close(output[1]);
    (void)sxn_close(control[1]);
    if (pid < 0) {
        (void)sxn_close(input[1]);
        (void)sxn_close(output[0]);
        (void)sxn_close(control[0]);
        return pid;
    }
    (void)sxn_set_nonblocking(output[0], 1);
    (void)sxn_set_nonblocking(control[0], 1);
    g_con.shell_pid = pid;
    g_con.input_write = input[1];
    g_con.output_read = output[0];
    g_con.control_read = control[0];
    return 0;
}

static void end_session(void) {
    int index = 0;
    int status = 0;

    for (index = 0; index < g_con.foreground_count; ++index) {
        (void)sxn_terminate(g_con.foreground[index], CON_INTERRUPT_STATUS);
    }
    if (g_con.shell_pid > 0) {
        if (g_con.control_read >= 0) {
            /* The window closed with the shell alive: it goes with it, as
             * closing a Windows console ends what is attached to it. */
            (void)sxn_terminate(g_con.shell_pid, CON_INTERRUPT_STATUS);
        }
        (void)sxn_waitpid(g_con.shell_pid, &status);
    }
}

int main(void) {
    struct sx_bitmap probe_bitmap;
    struct sx_painter probe;
    struct savanxp_fb_info probe_info;
    uint32_t probe_pixel = 0;
    int ready_logged = 0;

    memset(&g_con, 0, sizeof(g_con));
    g_con.input_write = -1;
    g_con.output_read = -1;
    g_con.control_read = -1;
    g_con.follow = 1;
    g_con.full_repaint = 1;

    if (sxnwm_connect(&g_con.wm) != 0) {
        sxn_log("console: no window");
        sxn_exit(1);
    }
    (void)sxnwm_set_presentation(&g_con.wm, &sun_resources);

    /* The cell is the console face's (UniFont 8x16). */
    memset(&probe_info, 0, sizeof(probe_info));
    probe_info.width = 1;
    probe_info.height = 1;
    probe_info.pitch = 4;
    probe_info.bpp = 32;
    probe_info.buffer_size = 4;
    sx_bitmap_wrap(&probe_bitmap, &probe_pixel, &probe_info, SX_PIXEL_FORMAT_BGRX8888);
    sx_painter_init(&probe, &probe_bitmap);
    (void)sx_painter_set_font(&probe, SX_FONT_MONO);
    g_con.cell_width = sx_painter_text_width(&probe, "M");
    g_con.cell_height = sx_painter_text_height(&probe);
    if (g_con.cell_width <= 0 || g_con.cell_height <= 0) {
        g_con.cell_width = 8;
        g_con.cell_height = 16;
    }
    g_con.window_lines = (int)g_con.wm.height / g_con.cell_height;
    if (g_con.window_lines < 1) {
        g_con.window_lines = 1;
    }
    (void)sxnwm_request_size(&g_con.wm,
        (uint32_t)(CON_COLUMNS * g_con.cell_width + CON_SCROLLBAR),
        (uint32_t)(CON_WINDOW_LINES * g_con.cell_height));

    caret_reset_blink();
    if (start_shell() != 0) {
        output_text("console: cannot start " CON_SHELL_PATH "\n");
    }

    for (;;) {
        int32_t handles[3];
        unsigned long count = 0;
        uint64_t now = 0;
        long timeout_ms = 0;

        if (sxnwm_close_requested(&g_con.wm)) {
            break;
        }
        (void)sxn_event_reset(g_con.wm.wake_fd);
        drain_window();
        if (!drain_shell() && g_con.shell_pid > 0) {
            break;
        }

        now = sxn_clock_ns();
        if (now >= g_con.next_blink_ns) {
            g_con.caret_on = !g_con.caret_on;
            g_con.next_blink_ns = now + CON_BLINK_NS;
            mark_caret();
        }
        paint();
        if (!ready_logged) {
            sxn_log("console: ready");
            ready_logged = 1;
        }

        /* Something may have arrived while painting: the ring is not a wait
         * target, its wake event is. */
        if (g_con.wm.ring != 0 && g_con.wm.ring->tail < g_con.wm.ring->head) {
            continue;
        }
        handles[count++] = (int32_t)g_con.wm.wake_fd;
        if (g_con.output_read >= 0) {
            handles[count++] = (int32_t)g_con.output_read;
        }
        if (g_con.control_read >= 0) {
            handles[count++] = (int32_t)g_con.control_read;
        }
        now = sxn_clock_ns();
        timeout_ms = g_con.next_blink_ns > now ? (long)((g_con.next_blink_ns - now) / 1000000ull) + 1 : 0;
        (void)sxn_wait_many(handles, count, timeout_ms);
    }

    end_session();
    sxn_exit(0);
}
