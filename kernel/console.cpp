#include "kernel/console.hpp"

#include <stddef.h>
#include <stdint.h>

#include "kernel/string.hpp"

namespace {

constexpr uint16_t kCom1Port = 0x3f8;
constexpr uint16_t kDebugConPort = 0x00e9;
constexpr uint32_t kBackground = 0x00060b10U;
constexpr uint32_t kHeader = 0x00133b2dU;
constexpr uint32_t kText = 0x00d5f4dfU;
constexpr uint32_t kCursor = 0x0027d37bU;
constexpr uint32_t kShadow = 0x00020406U;
constexpr uint32_t kPaddingX = 16;
constexpr uint32_t kPaddingY = 18;
constexpr uint32_t kScale = 2;
constexpr uint32_t kGlyphWidth = 5;
constexpr uint32_t kGlyphHeight = 7;
constexpr uint32_t kCellWidth = (kGlyphWidth + 1) * kScale;
constexpr uint32_t kCellHeight = (kGlyphHeight + 1) * kScale;
constexpr size_t kMaxColumns = 128;
constexpr size_t kMaxRows = 64;

struct Glyph {
    char character;
    uint8_t rows[kGlyphHeight];
};

constexpr Glyph kGlyphs[] = {
    {' ', {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}},
    {'!', {0x04, 0x04, 0x04, 0x04, 0x04, 0x00, 0x04}},
    {'"', {0x0a, 0x0a, 0x0a, 0x00, 0x00, 0x00, 0x00}},
    {'#', {0x0a, 0x1f, 0x0a, 0x0a, 0x1f, 0x0a, 0x00}},
    {'(', {0x02, 0x04, 0x08, 0x08, 0x08, 0x04, 0x02}},
    {')', {0x08, 0x04, 0x02, 0x02, 0x02, 0x04, 0x08}},
    {'*', {0x00, 0x04, 0x15, 0x0e, 0x15, 0x04, 0x00}},
    {'+', {0x00, 0x04, 0x04, 0x1f, 0x04, 0x04, 0x00}},
    {',', {0x00, 0x00, 0x00, 0x00, 0x0c, 0x0c, 0x08}},
    {'-', {0x00, 0x00, 0x00, 0x1f, 0x00, 0x00, 0x00}},
    {'.', {0x00, 0x00, 0x00, 0x00, 0x00, 0x0c, 0x0c}},
    {'/', {0x01, 0x02, 0x04, 0x08, 0x10, 0x00, 0x00}},
    {'<', {0x01, 0x02, 0x04, 0x08, 0x04, 0x02, 0x01}},
    {':', {0x00, 0x0c, 0x0c, 0x00, 0x0c, 0x0c, 0x00}},
    {'>', {0x10, 0x08, 0x04, 0x02, 0x04, 0x08, 0x10}},
    {'0', {0x0e, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0e}},
    {'1', {0x04, 0x0c, 0x04, 0x04, 0x04, 0x04, 0x0e}},
    {'2', {0x0e, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1f}},
    {'3', {0x1e, 0x01, 0x01, 0x0e, 0x01, 0x01, 0x1e}},
    {'4', {0x02, 0x06, 0x0a, 0x12, 0x1f, 0x02, 0x02}},
    {'5', {0x1f, 0x10, 0x10, 0x1e, 0x01, 0x01, 0x1e}},
    {'6', {0x0e, 0x10, 0x10, 0x1e, 0x11, 0x11, 0x0e}},
    {'7', {0x1f, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08}},
    {'8', {0x0e, 0x11, 0x11, 0x0e, 0x11, 0x11, 0x0e}},
    {'9', {0x0e, 0x11, 0x11, 0x0f, 0x01, 0x01, 0x0e}},
    {'=', {0x00, 0x1f, 0x00, 0x1f, 0x00, 0x00, 0x00}},
    {'?', {0x0e, 0x11, 0x01, 0x02, 0x04, 0x00, 0x04}},
    {'A', {0x0e, 0x11, 0x11, 0x1f, 0x11, 0x11, 0x11}},
    {'B', {0x1e, 0x11, 0x11, 0x1e, 0x11, 0x11, 0x1e}},
    {'C', {0x0e, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0e}},
    {'D', {0x1e, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1e}},
    {'E', {0x1f, 0x10, 0x10, 0x1e, 0x10, 0x10, 0x1f}},
    {'F', {0x1f, 0x10, 0x10, 0x1e, 0x10, 0x10, 0x10}},
    {'G', {0x0e, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0f}},
    {'H', {0x11, 0x11, 0x11, 0x1f, 0x11, 0x11, 0x11}},
    {'I', {0x1f, 0x04, 0x04, 0x04, 0x04, 0x04, 0x1f}},
    {'J', {0x01, 0x01, 0x01, 0x01, 0x11, 0x11, 0x0e}},
    {'K', {0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11}},
    {'L', {0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1f}},
    {'M', {0x11, 0x1b, 0x15, 0x15, 0x11, 0x11, 0x11}},
    {'N', {0x11, 0x11, 0x19, 0x15, 0x13, 0x11, 0x11}},
    {'O', {0x0e, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0e}},
    {'P', {0x1e, 0x11, 0x11, 0x1e, 0x10, 0x10, 0x10}},
    {'Q', {0x0e, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0d}},
    {'R', {0x1e, 0x11, 0x11, 0x1e, 0x14, 0x12, 0x11}},
    {'S', {0x0f, 0x10, 0x10, 0x0e, 0x01, 0x01, 0x1e}},
    {'T', {0x1f, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04}},
    {'U', {0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0e}},
    {'V', {0x11, 0x11, 0x11, 0x11, 0x11, 0x0a, 0x04}},
    {'W', {0x11, 0x11, 0x11, 0x15, 0x15, 0x15, 0x0a}},
    {'X', {0x11, 0x11, 0x0a, 0x04, 0x0a, 0x11, 0x11}},
    {'Y', {0x11, 0x11, 0x0a, 0x04, 0x04, 0x04, 0x04}},
    {'Z', {0x1f, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1f}},
    {'[', {0x0e, 0x08, 0x08, 0x08, 0x08, 0x08, 0x0e}},
    {'\\', {0x10, 0x08, 0x04, 0x02, 0x01, 0x00, 0x00}},
    {']', {0x0e, 0x02, 0x02, 0x02, 0x02, 0x02, 0x0e}},
    {'_', {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x1f}},
    {'|', {0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04}},
    {'a', {0x0e, 0x11, 0x11, 0x1f, 0x11, 0x11, 0x11}},
    {'b', {0x1e, 0x11, 0x11, 0x1e, 0x11, 0x11, 0x1e}},
    {'c', {0x0e, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0e}},
    {'d', {0x1e, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1e}},
    {'e', {0x1f, 0x10, 0x10, 0x1e, 0x10, 0x10, 0x1f}},
    {'f', {0x1f, 0x10, 0x10, 0x1e, 0x10, 0x10, 0x10}},
    {'g', {0x0e, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0f}},
    {'h', {0x11, 0x11, 0x11, 0x1f, 0x11, 0x11, 0x11}},
    {'i', {0x1f, 0x04, 0x04, 0x04, 0x04, 0x04, 0x1f}},
    {'j', {0x01, 0x01, 0x01, 0x01, 0x11, 0x11, 0x0e}},
    {'k', {0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11}},
    {'l', {0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1f}},
    {'m', {0x11, 0x1b, 0x15, 0x15, 0x11, 0x11, 0x11}},
    {'n', {0x11, 0x11, 0x19, 0x15, 0x13, 0x11, 0x11}},
    {'o', {0x0e, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0e}},
    {'p', {0x1e, 0x11, 0x11, 0x1e, 0x10, 0x10, 0x10}},
    {'q', {0x0e, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0d}},
    {'r', {0x1e, 0x11, 0x11, 0x1e, 0x14, 0x12, 0x11}},
    {'s', {0x0f, 0x10, 0x10, 0x0e, 0x01, 0x01, 0x1e}},
    {'t', {0x1f, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04}},
    {'u', {0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0e}},
    {'v', {0x11, 0x11, 0x11, 0x11, 0x11, 0x0a, 0x04}},
    {'w', {0x11, 0x11, 0x11, 0x15, 0x15, 0x15, 0x0a}},
    {'x', {0x11, 0x11, 0x0a, 0x04, 0x0a, 0x11, 0x11}},
    {'y', {0x11, 0x11, 0x0a, 0x04, 0x04, 0x04, 0x04}},
    {'z', {0x1f, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1f}},
};

bool g_serial_ready = false;
boot::FramebufferInfo g_framebuffer = {};
char g_cells[kMaxRows][kMaxColumns] = {};
size_t g_columns = 0;
size_t g_rows = 0;
size_t g_cursor_row = 0;
size_t g_cursor_column = 0;
bool g_cursor_drawn = false;

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
    if (!g_framebuffer.available || g_framebuffer.address == nullptr || g_framebuffer.bpp != 32) {
        return;
    }

    fill_rect(0, 0, g_framebuffer.width, g_framebuffer.height, kBackground);
    fill_rect(0, 0, g_framebuffer.width, 6, kHeader);
    fill_rect(0, 6, g_framebuffer.width, 2, kShadow);
    fill_rect(0, 0, 8, g_framebuffer.height, kHeader);
}

const uint8_t* glyph_for(char character) {
    for (const Glyph& glyph : kGlyphs) {
        if (glyph.character == character) {
            return glyph.rows;
        }
    }
    return kGlyphs[0].rows;
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
    if (!g_cursor_drawn || !g_framebuffer.available) {
        return;
    }

    const uint64_t x = kPaddingX + (g_cursor_column * kCellWidth);
    const uint64_t y = kPaddingY + (g_cursor_row * kCellHeight) + (kCellHeight - 3);
    fill_rect(x, y, kCellWidth - 2, 2, kBackground);
    g_cursor_drawn = false;
}

void render_cell(size_t row, size_t column) {
    if (!g_framebuffer.available || row >= g_rows || column >= g_columns) {
        return;
    }

    const uint64_t x = kPaddingX + (column * kCellWidth);
    const uint64_t y = kPaddingY + (row * kCellHeight);
    fill_rect(x, y, kCellWidth, kCellHeight, kBackground);
    draw_glyph(x, y, g_cells[row][column], kText);
}

void draw_cursor() {
    if (!g_framebuffer.available || g_cursor_row >= g_rows || g_cursor_column >= g_columns) {
        return;
    }

    const uint64_t x = kPaddingX + (g_cursor_column * kCellWidth);
    const uint64_t y = kPaddingY + (g_cursor_row * kCellHeight) + (kCellHeight - 3);
    fill_rect(x, y, kCellWidth - 2, 2, kCursor);
    g_cursor_drawn = true;
}

void redraw_all() {
    if (!g_framebuffer.available) {
        return;
    }

    draw_background();
    for (size_t row = 0; row < g_rows; ++row) {
        for (size_t column = 0; column < g_columns; ++column) {
            render_cell(row, column);
        }
    }
    draw_cursor();
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
    memset(g_cells, ' ', sizeof(g_cells));
    g_cursor_row = 0;
    g_cursor_column = 0;
    g_cursor_drawn = false;

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
