#include "kernel/console.hpp"

#include <stddef.h>
#include <stdint.h>

namespace {

constexpr uint16_t kCom1Port = 0x3f8;
constexpr uint16_t kDebugConPort = 0x00e9;
constexpr uint32_t kBackgroundTop = 0x00111b20U;
constexpr uint32_t kBackgroundBottom = 0x00070d10U;
constexpr uint32_t kPanelColour = 0x0014252cU;
constexpr uint32_t kPanelAccent = 0x001f8b4cU;
constexpr uint32_t kPanelAccentMuted = 0x0011452aU;
constexpr uint32_t kPrimaryText = 0x00f1f7f2U;
constexpr uint32_t kSecondaryText = 0x0096c9aaU;

struct Glyph {
    char character;
    uint8_t rows[7];
};

constexpr Glyph kGlyphs[] = {
    {' ', {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}},
    {'+', {0x00, 0x04, 0x04, 0x1f, 0x04, 0x04, 0x00}},
    {'-', {0x00, 0x00, 0x00, 0x1f, 0x00, 0x00, 0x00}},
    {'.', {0x00, 0x00, 0x00, 0x00, 0x00, 0x0c, 0x0c}},
    {'/', {0x01, 0x02, 0x04, 0x08, 0x10, 0x00, 0x00}},
    {':', {0x00, 0x04, 0x04, 0x00, 0x04, 0x04, 0x00}},
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
};

bool g_serial_ready = false;
boot::FramebufferInfo g_framebuffer = {};

inline void out8(uint16_t port, uint8_t value) {
    asm volatile("outb %0, %1" : : "a"(value), "Nd"(port));
}

inline uint8_t in8(uint16_t port) {
    uint8_t value = 0;
    asm volatile("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

char to_upper(char value) {
    return (value >= 'a' && value <= 'z')
        ? static_cast<char>(value - ('a' - 'A'))
        : value;
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
    if (!g_framebuffer.available || g_framebuffer.address == nullptr || g_framebuffer.bpp != 32) {
        return;
    }

    const uint64_t end_x = (x + width) < g_framebuffer.width ? (x + width) : g_framebuffer.width;
    const uint64_t end_y = (y + height) < g_framebuffer.height ? (y + height) : g_framebuffer.height;

    for (uint64_t row = y; row < end_y; ++row) {
        for (uint64_t column = x; column < end_x; ++column) {
            plot_pixel(column, row, colour);
        }
    }
}

uint32_t blend_channel(uint32_t from, uint32_t to, uint64_t position, uint64_t total) {
    if (total == 0) {
        return to;
    }

    return static_cast<uint32_t>(((from * (total - position)) + (to * position)) / total);
}

uint32_t interpolate_colour(uint32_t top, uint32_t bottom, uint64_t position, uint64_t total) {
    const uint32_t top_blue = top & 0xff;
    const uint32_t top_green = (top >> 8) & 0xff;
    const uint32_t top_red = (top >> 16) & 0xff;
    const uint32_t bottom_blue = bottom & 0xff;
    const uint32_t bottom_green = (bottom >> 8) & 0xff;
    const uint32_t bottom_red = (bottom >> 16) & 0xff;

    return blend_channel(top_blue, bottom_blue, position, total) |
        (blend_channel(top_green, bottom_green, position, total) << 8) |
        (blend_channel(top_red, bottom_red, position, total) << 16);
}

void draw_background() {
    if (!g_framebuffer.available || g_framebuffer.address == nullptr || g_framebuffer.bpp != 32) {
        return;
    }

    for (uint64_t y = 0; y < g_framebuffer.height; ++y) {
        const uint32_t colour = interpolate_colour(
            kBackgroundTop,
            kBackgroundBottom,
            y,
            g_framebuffer.height == 0 ? 1 : g_framebuffer.height
        );
        fill_rect(0, y, g_framebuffer.width, 1, colour);
    }

    fill_rect(0, 0, 24, g_framebuffer.height, kPanelAccent);
    fill_rect(24, 0, 12, g_framebuffer.height, kPanelAccentMuted);
}

const uint8_t* glyph_for(char character) {
    const char glyph_key = to_upper(character);
    for (const Glyph& glyph : kGlyphs) {
        if (glyph.character == glyph_key) {
            return glyph.rows;
        }
    }

    return kGlyphs[0].rows;
}

void draw_glyph(uint64_t x, uint64_t y, uint32_t colour, uint32_t scale, char character) {
    const uint8_t* glyph = glyph_for(character);
    for (uint64_t row = 0; row < 7; ++row) {
        for (uint64_t column = 0; column < 5; ++column) {
            if ((glyph[row] & (1u << (4 - column))) == 0) {
                continue;
            }

            fill_rect(
                x + (column * scale),
                y + (row * scale),
                scale,
                scale,
                colour
            );
        }
    }
}

void draw_text(uint64_t x, uint64_t y, uint32_t colour, uint32_t scale, const char* text) {
    if (text == nullptr) {
        return;
    }

    uint64_t cursor_x = x;
    for (const char* cursor = text; *cursor != '\0'; ++cursor) {
        draw_glyph(cursor_x, y, colour, scale, *cursor);
        cursor_x += 6 * scale;
    }
}

void draw_card(uint64_t x, uint64_t y, uint64_t width, uint64_t height, const char* text) {
    fill_rect(x + 6, y + 6, width, height, 0x0003080aU);
    fill_rect(x, y, width, height, kPanelColour);
    fill_rect(x, y, 8, height, kPanelAccent);
    draw_text(x + 22, y + 14, kPrimaryText, 2, text);
}

void framebuffer_fill(uint32_t colour) {
    fill_rect(0, 0, g_framebuffer.width, g_framebuffer.height, colour);
}

void framebuffer_draw_boot_mark() {
    if (!g_framebuffer.available) {
        return;
    }

    const uint64_t band_height = g_framebuffer.height > 24 ? 24 : g_framebuffer.height;
    for (uint64_t x = 0; x < g_framebuffer.width; ++x) {
        const uint32_t colour = (x % 32 < 16) ? kPanelAccent : kPanelAccentMuted;
        fill_rect(x, 0, 1, band_height, colour);
    }
}

void write_unsigned(uint64_t value, uint32_t base) {
    char buffer[32] = {};
    size_t index = 0;

    if (value == 0) {
        serial_write_char('0');
        return;
    }

    while (value != 0 && index < sizeof(buffer)) {
        const uint64_t digit = value % base;
        buffer[index++] = static_cast<char>(digit < 10 ? ('0' + digit) : ('a' + (digit - 10)));
        value /= base;
    }

    while (index > 0) {
        serial_write_char(buffer[--index]);
    }
}

void write_signed(int64_t value) {
    if (value < 0) {
        serial_write_char('-');
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
    if (g_framebuffer.available) {
        framebuffer_fill(kBackgroundTop);
        framebuffer_draw_boot_mark();
    }
}

void write(const char* text) {
    if (text == nullptr) {
        return;
    }

    for (const char* cursor = text; *cursor != '\0'; ++cursor) {
        if (*cursor == '\n') {
            serial_write_char('\r');
        }
        serial_write_char(*cursor);
    }
}

void write_line(const char* text) {
    write(text);
    write("\n");
}

void vprintf(const char* format, va_list args) {
    for (const char* cursor = format; *cursor != '\0'; ++cursor) {
        if (*cursor != '%') {
            if (*cursor == '\n') {
                serial_write_char('\r');
            }
            serial_write_char(*cursor);
            continue;
        }

        ++cursor;
        switch (*cursor) {
            case '%':
                serial_write_char('%');
                break;
            case 'c':
                serial_write_char(static_cast<char>(va_arg(args, int)));
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
            case 'l':
                ++cursor;
                if (*cursor == 'l') {
                    ++cursor;
                    if (*cursor == 'u') {
                        write_unsigned(va_arg(args, uint64_t), 10);
                    } else if (*cursor == 'x') {
                        write_unsigned(va_arg(args, uint64_t), 16);
                    } else {
                        write("<?>");
                    }
                } else if (*cursor == 'u') {
                    write_unsigned(va_arg(args, unsigned long), 10);
                } else if (*cursor == 'x') {
                    write_unsigned(va_arg(args, unsigned long), 16);
                } else {
                    write("<?>");
                }
                break;
            case 'x':
                write_unsigned(va_arg(args, unsigned int), 16);
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

void show_welcome_screen(const char* const* lines, size_t line_count) {
    if (!g_framebuffer.available || g_framebuffer.address == nullptr || g_framebuffer.bpp != 32) {
        return;
    }

    draw_background();

    fill_rect(54, 44, g_framebuffer.width > 120 ? (g_framebuffer.width - 108) : g_framebuffer.width, 162, 0x00050a0cU);
    fill_rect(48, 38, g_framebuffer.width > 120 ? (g_framebuffer.width - 108) : g_framebuffer.width, 162, kPanelColour);
    fill_rect(48, 38, 10, 162, kPanelAccent);

    draw_text(78, 60, kSecondaryText, 2, "WELCOME TO");
    draw_text(78, 94, kPrimaryText, 5, "SAVANXP");
    draw_text(78, 158, kSecondaryText, 2, "AI BUILT HOBBY OS EXPERIMENT");

    draw_text(78, 226, kSecondaryText, 2, "CURRENT MILESTONES");

    uint64_t card_y = 268;
    for (size_t index = 0; index < line_count; ++index) {
        draw_card(78, card_y, g_framebuffer.width > 180 ? (g_framebuffer.width - 156) : g_framebuffer.width, 44, lines[index]);
        card_y += 54;
    }

    draw_text(78, card_y + 18, kSecondaryText, 2, "SERIAL CONSOLE ACTIVE");
}

} // namespace console
