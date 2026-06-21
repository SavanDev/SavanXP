#include "kernel/console.hpp"

#include <stddef.h>
#include <stdint.h>

#include "kernel/display.hpp"
#include "kernel/string.hpp"

namespace {

constexpr uint16_t kCom1Port = 0x3f8;
constexpr uint16_t kDebugConPort = 0x00e9;
constexpr uint32_t kBackground = 0x00060b10U;
constexpr uint32_t kText = 0x00d5f4dfU;
constexpr uint32_t kCursor = 0x0027d37bU;
constexpr uint32_t kPaddingX = 16;
constexpr uint32_t kPaddingY = 18;
constexpr uint32_t kScale = 1;

#include "kernel/console_font_unifont.inc"

constexpr uint32_t kGlyphWidth = SX_CONSOLE_GLYPH_W;
constexpr uint32_t kGlyphHeight = SX_CONSOLE_GLYPH_H;
constexpr uint32_t kCellWidth = kGlyphWidth * kScale;
constexpr uint32_t kCellHeight = kGlyphHeight * kScale;
constexpr size_t kMaxColumns = 128;
constexpr size_t kMaxRows = 64;

bool g_serial_ready = false;
boot::FramebufferInfo g_framebuffer = {};
boot::FramebufferInfo g_boot_framebuffer = {};
bool g_console_visible = true;
char g_cells[kMaxRows][kMaxColumns] = {};
size_t g_columns = 0;
size_t g_rows = 0;
size_t g_cursor_row = 0;
size_t g_cursor_column = 0;
bool g_cursor_drawn = false;
bool g_batch_redraw = false;

bool using_gpu_backend() {
    return display::ready() &&
        g_framebuffer.address != nullptr &&
        g_framebuffer.address == display::framebuffer_address();
}

void flush_region(uint32_t x, uint32_t y, uint32_t width, uint32_t height) {
    if (!g_console_visible || !using_gpu_backend() || g_batch_redraw) {
        return;
    }
    (void)display::flush_rect(x, y, width, height);
}

void flush_full() {
    if (!g_console_visible || !using_gpu_backend()) {
        return;
    }
    (void)display::flush();
}

inline void out8(uint16_t port, uint8_t value) {
    asm volatile("outb %0, %1" : : "a"(value), "Nd"(port));
}

inline uint8_t in8(uint16_t port) {
    uint8_t value = 0;
    asm volatile("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

void serial_write_char(char value) {
    out8(kDebugConPort, static_cast<uint8_t>(value));

    if (!g_serial_ready) {
        return;
    }

    while ((in8(kCom1Port + 5) & 0x20) == 0) {
    }

    out8(kCom1Port, static_cast<uint8_t>(value));
}

void plot_pixel(uint64_t x, uint64_t y, uint32_t colour) {
    if (!g_framebuffer.available || g_framebuffer.address == nullptr || g_framebuffer.bpp != 32) {
        return;
    }
    if (x >= g_framebuffer.width || y >= g_framebuffer.height) {
        return;
    }

    auto* pixels = static_cast<uint8_t*>(g_framebuffer.address);
    auto* row = reinterpret_cast<uint32_t*>(pixels + (y * g_framebuffer.pitch));
    row[x] = colour;
}

void fill_rect(uint64_t x, uint64_t y, uint64_t width, uint64_t height, uint32_t colour) {
    const uint64_t end_x = (x + width) < g_framebuffer.width ? (x + width) : g_framebuffer.width;
    const uint64_t end_y = (y + height) < g_framebuffer.height ? (y + height) : g_framebuffer.height;
    for (uint64_t row = y; row < end_y; ++row) {
        for (uint64_t column = x; column < end_x; ++column) {
            plot_pixel(column, row, colour);
        }
    }
}

void draw_background() {
    if (!g_console_visible || !g_framebuffer.available || g_framebuffer.address == nullptr || g_framebuffer.bpp != 32) {
        return;
    }

    fill_rect(0, 0, g_framebuffer.width, g_framebuffer.height, kBackground);
}

const uint8_t* glyph_for(char character) {
    // busybox emits Latin-1 bytes, which map 1:1 onto codepoints 0x20-0xFF.
    // Control bytes and anything unmapped fall back to .notdef inside the table.
    return sx_console_glyph(static_cast<unsigned char>(character));
}

void draw_glyph(uint64_t x, uint64_t y, char character, uint32_t colour) {
    const uint8_t* glyph = glyph_for(character);
    for (uint64_t row = 0; row < kGlyphHeight; ++row) {
        for (uint64_t column = 0; column < kGlyphWidth; ++column) {
            if ((glyph[row] & (1u << (kGlyphWidth - 1 - column))) == 0) {
                continue;
            }
            fill_rect(
                x + (column * kScale),
                y + (row * kScale),
                kScale,
                kScale,
                colour
            );
        }
    }
}

void erase_cursor() {
    if (!g_cursor_drawn || !g_console_visible || !g_framebuffer.available) {
        return;
    }

    const uint64_t x = kPaddingX + (g_cursor_column * kCellWidth);
    const uint64_t y = kPaddingY + (g_cursor_row * kCellHeight) + (kCellHeight - 3);
    fill_rect(x, y, kCellWidth - 2, 2, kBackground);
    flush_region(static_cast<uint32_t>(x), static_cast<uint32_t>(y), kCellWidth - 2, 2);
    g_cursor_drawn = false;
}

void render_cell(size_t row, size_t column) {
    if (!g_console_visible || !g_framebuffer.available || row >= g_rows || column >= g_columns) {
        return;
    }

    const uint64_t x = kPaddingX + (column * kCellWidth);
    const uint64_t y = kPaddingY + (row * kCellHeight);
    fill_rect(x, y, kCellWidth, kCellHeight, kBackground);
    draw_glyph(x, y, g_cells[row][column], kText);
    flush_region(static_cast<uint32_t>(x), static_cast<uint32_t>(y), kCellWidth, kCellHeight);
}

void draw_cursor() {
    if (!g_console_visible || !g_framebuffer.available || g_cursor_row >= g_rows || g_cursor_column >= g_columns) {
        return;
    }

    const uint64_t x = kPaddingX + (g_cursor_column * kCellWidth);
    const uint64_t y = kPaddingY + (g_cursor_row * kCellHeight) + (kCellHeight - 3);
    fill_rect(x, y, kCellWidth - 2, 2, kCursor);
    flush_region(static_cast<uint32_t>(x), static_cast<uint32_t>(y), kCellWidth - 2, 2);
    g_cursor_drawn = true;
}

void redraw_all() {
    if (!g_console_visible || !g_framebuffer.available) {
        return;
    }

    g_batch_redraw = true;
    draw_background();
    for (size_t row = 0; row < g_rows; ++row) {
        for (size_t column = 0; column < g_columns; ++column) {
            render_cell(row, column);
        }
    }
    draw_cursor();
    g_batch_redraw = false;
    flush_full();
}

void scroll_up() {
    if (g_rows == 0) {
        return;
    }

    erase_cursor();
    for (size_t row = 1; row < g_rows; ++row) {
        memcpy(g_cells[row - 1], g_cells[row], g_columns);
    }
    memset(g_cells[g_rows - 1], ' ', g_columns);
    g_cursor_row = g_rows - 1;
    redraw_all();
}

void newline() {
    erase_cursor();
    g_cursor_column = 0;
    ++g_cursor_row;
    if (g_cursor_row >= g_rows) {
        scroll_up();
    }
    draw_cursor();
}

void write_unsigned(uint64_t value, uint32_t base) {
    char buffer[32] = {};
    size_t index = 0;

    if (value == 0) {
        console::write_char('0');
        return;
    }

    while (value != 0 && index < sizeof(buffer)) {
        const uint64_t digit = value % base;
        buffer[index++] = static_cast<char>(digit < 10 ? ('0' + digit) : ('a' + (digit - 10)));
        value /= base;
    }

    while (index > 0) {
        console::write_char(buffer[--index]);
    }
}

void write_signed(int64_t value) {
    if (value < 0) {
        console::write_char('-');
        write_unsigned(static_cast<uint64_t>(-value), 10);
        return;
    }
    write_unsigned(static_cast<uint64_t>(value), 10);
}

} // namespace

namespace console {

void early_init() {
    out8(kCom1Port + 1, 0x00);
    out8(kCom1Port + 3, 0x80);
    out8(kCom1Port + 0, 0x03);
    out8(kCom1Port + 1, 0x00);
    out8(kCom1Port + 3, 0x03);
    out8(kCom1Port + 2, 0xc7);
    out8(kCom1Port + 4, 0x0b);
    g_serial_ready = true;
}

void init(const boot::BootInfo& boot_info) {
    g_framebuffer = boot_info.framebuffer;
    g_boot_framebuffer = boot_info.framebuffer;
    memset(g_cells, ' ', sizeof(g_cells));
    g_cursor_row = 0;
    g_cursor_column = 0;
    g_cursor_drawn = false;
    g_console_visible = true;

    if (g_framebuffer.available && g_framebuffer.width > (kPaddingX * 2) && g_framebuffer.height > (kPaddingY * 2)) {
        g_columns = (g_framebuffer.width - (kPaddingX * 2)) / kCellWidth;
        g_rows = (g_framebuffer.height - (kPaddingY * 2)) / kCellHeight;
        if (g_columns > kMaxColumns) {
            g_columns = kMaxColumns;
        }
        if (g_rows > kMaxRows) {
            g_rows = kMaxRows;
        }
        redraw_all();
    }
}

const boot::FramebufferInfo& framebuffer_info() {
    return g_framebuffer;
}

bool framebuffer_ready() {
    return g_framebuffer.available && g_framebuffer.address != nullptr && g_framebuffer.bpp == 32;
}

void set_framebuffer_console_enabled(bool enabled) {
    g_console_visible = enabled;
    if (enabled) {
        g_cursor_drawn = false;
        redraw_all();
    } else {
        g_cursor_drawn = false;
    }
}

void redraw() {
    redraw_all();
}

void set_external_framebuffer(void* address, const savanxp_fb_info& info) {
    if (address == nullptr || info.width == 0 || info.height == 0 || info.pitch == 0 || info.bpp != 32) {
        return;
    }

    g_framebuffer.address = address;
    g_framebuffer.width = info.width;
    g_framebuffer.height = info.height;
    g_framebuffer.pitch = info.pitch;
    g_framebuffer.bpp = static_cast<uint8_t>(info.bpp);
    g_framebuffer.available = true;

    if (g_framebuffer.width > (kPaddingX * 2) && g_framebuffer.height > (kPaddingY * 2)) {
        g_columns = (g_framebuffer.width - (kPaddingX * 2)) / kCellWidth;
        g_rows = (g_framebuffer.height - (kPaddingY * 2)) / kCellHeight;
        if (g_columns > kMaxColumns) {
            g_columns = kMaxColumns;
        }
        if (g_rows > kMaxRows) {
            g_rows = kMaxRows;
        }
    } else {
        g_columns = 0;
        g_rows = 0;
    }
    if (g_rows != 0 && g_cursor_row >= g_rows) {
        g_cursor_row = g_rows - 1;
    }
    if (g_columns != 0 && g_cursor_column >= g_columns) {
        g_cursor_column = g_columns - 1;
    }
    g_cursor_drawn = false;
    redraw_all();
}

bool present_pixels(const void* pixels, size_t byte_count) {
    if (!framebuffer_ready() || pixels == nullptr) {
        return false;
    }

    const uint64_t framebuffer_bytes = g_framebuffer.pitch * g_framebuffer.height;
    if (byte_count != framebuffer_bytes) {
        return false;
    }

    memcpy(g_framebuffer.address, pixels, byte_count);
    flush_full();
    return true;
}

bool present_region(const void* pixels, uint32_t source_pitch, uint32_t x, uint32_t y, uint32_t width, uint32_t height) {
    if (!framebuffer_ready() || pixels == nullptr || source_pitch == 0 || width == 0 || height == 0) {
        return false;
    }
    if (x >= g_framebuffer.width || y >= g_framebuffer.height) {
        return false;
    }
    if (width > (g_framebuffer.width - x) || height > (g_framebuffer.height - y)) {
        return false;
    }

    const uint32_t row_bytes = width * sizeof(uint32_t);
    if (source_pitch < row_bytes) {
        return false;
    }

    auto* destination = static_cast<uint8_t*>(g_framebuffer.address);
    const auto* source = static_cast<const uint8_t*>(pixels);
    for (uint32_t row = 0; row < height; ++row) {
        const size_t source_offset = static_cast<size_t>(y + row) * source_pitch + (static_cast<size_t>(x) * sizeof(uint32_t));
        const size_t destination_offset = static_cast<size_t>(y + row) * g_framebuffer.pitch + (static_cast<size_t>(x) * sizeof(uint32_t));
        memcpy(destination + destination_offset, source + source_offset, row_bytes);
    }
    flush_region(x, y, width, height);
    return true;
}

void clear() {
    memset(g_cells, ' ', sizeof(g_cells));
    g_cursor_row = 0;
    g_cursor_column = 0;
    g_cursor_drawn = false;
    redraw_all();
}

void write_char(char character) {
    if (character == '\r') {
        return;
    }

    if (character == '\n') {
        serial_write_char('\r');
        serial_write_char('\n');
        newline();
        return;
    }

    if (character == '\b') {
        serial_write_char('\b');
        serial_write_char(' ');
        serial_write_char('\b');

        if (g_cursor_column == 0) {
            return;
        }

        erase_cursor();
        --g_cursor_column;
        g_cells[g_cursor_row][g_cursor_column] = ' ';
        render_cell(g_cursor_row, g_cursor_column);
        draw_cursor();
        return;
    }

    serial_write_char(character);

    if (!g_framebuffer.available || g_rows == 0 || g_columns == 0) {
        return;
    }

    erase_cursor();
    if (g_cursor_column >= g_columns) {
        newline();
        erase_cursor();
    }

    g_cells[g_cursor_row][g_cursor_column] = character;
    render_cell(g_cursor_row, g_cursor_column);
    ++g_cursor_column;
    if (g_cursor_column >= g_columns) {
        newline();
        return;
    }
    draw_cursor();
}

void write(const char* text) {
    if (text == nullptr) {
        return;
    }

    for (const char* cursor = text; *cursor != '\0'; ++cursor) {
        write_char(*cursor);
    }
}

void write_line(const char* text) {
    write(text);
    write_char('\n');
}

void vprintf(const char* format, va_list args) {
    for (const char* cursor = format; *cursor != '\0'; ++cursor) {
        if (*cursor != '%') {
            write_char(*cursor);
            continue;
        }

        ++cursor;
        switch (*cursor) {
            case '%':
                write_char('%');
                break;
            case 'c':
                write_char(static_cast<char>(va_arg(args, int)));
                break;
            case 's': {
                const char* string = va_arg(args, const char*);
                write(string != nullptr ? string : "(null)");
                break;
            }
            case 'd':
            case 'i':
                write_signed(va_arg(args, int));
                break;
            case 'u':
                write_unsigned(va_arg(args, unsigned int), 10);
                break;
            case 'x':
                write_unsigned(va_arg(args, unsigned int), 16);
                break;
            case 'l':
                ++cursor;
                if (*cursor == 'l') {
                    ++cursor;
                    if (*cursor == 'u') {
                        write_unsigned(va_arg(args, uint64_t), 10);
                    } else if (*cursor == 'x') {
                        write_unsigned(va_arg(args, uint64_t), 16);
                    }
                } else if (*cursor == 'u') {
                    write_unsigned(va_arg(args, unsigned long), 10);
                } else if (*cursor == 'x') {
                    write_unsigned(va_arg(args, unsigned long), 16);
                }
                break;
            case 'p':
                write("0x");
                write_unsigned(reinterpret_cast<uint64_t>(va_arg(args, void*)), 16);
                break;
            default:
                write("%?");
                break;
        }
    }
}

void printf(const char* format, ...) {
    va_list args;
    va_start(args, format);
    vprintf(format, args);
    va_end(args);
}

} // namespace console
