#include "fb32.hpp"

#include <stddef.h>

#include "kernel/console_font_unifont.inc"
#include "paging.hpp"
#include "kernel/string.hpp"

namespace i386 {

namespace fb32 {

namespace {

/* BGRX pixels use 0x00RRGGBB, matching gfx_rgb() and x86_64. */
constexpr uint32_t kColourTypeRgb = 1;
constexpr uint32_t kBytesPerPixel = 4;

constexpr uint32_t kConsoleBackground = 0x00101824u;
constexpr uint32_t kConsoleForeground = 0x00C8D4E0u;

bool g_ready = false;
bool g_console_enabled = true;
uint64_t g_address = 0;
uint32_t* g_framebuffer = nullptr;
savanxp_fb_info g_info = {};

uint32_t g_columns = 0;
uint32_t g_rows = 0;
uint32_t g_console_column = 0;
uint32_t g_console_row = 0;

int32_t g_pointer_x = 0;
int32_t g_pointer_y = 0;
uint32_t g_pointer_buttons = 0;

inline uint32_t stride_pixels() {
    return g_info.pitch / kBytesPerPixel;
}

inline int clamp_int(int value, int low, int high) {
    if (value < low) {
        return low;
    }
    if (value > high) {
        return high;
    }
    return value;
}

inline void set_console_pixel(uint32_t x, uint32_t y, uint32_t colour) {
    g_framebuffer[static_cast<size_t>(y) * stride_pixels() + x] = colour;
}

void fill_cell(uint32_t column, uint32_t row, char character) {
    const uint32_t x = column * SX_CONSOLE_GLYPH_W;
    const uint32_t y = row * SX_CONSOLE_GLYPH_H;
    for (uint32_t line = 0; line < SX_CONSOLE_GLYPH_H; ++line) {
        for (uint32_t pixel = 0; pixel < SX_CONSOLE_GLYPH_W; ++pixel) {
            set_console_pixel(x + pixel, y + line, kConsoleBackground);
        }
    }
    const uint8_t* glyph =
        sx_console_glyph(static_cast<unsigned int>(static_cast<uint8_t>(character)));
    for (uint32_t line = 0; line < SX_CONSOLE_GLYPH_H; ++line) {
        const uint8_t bits = glyph[line];
        for (uint32_t pixel = 0; pixel < SX_CONSOLE_GLYPH_W; ++pixel) {
            if ((bits & (1u << (SX_CONSOLE_GLYPH_W - 1u - pixel))) != 0) {
                set_console_pixel(x + pixel, y + line, kConsoleForeground);
            }
        }
    }
}

void scroll_up() {
    const uint32_t row_bytes = SX_CONSOLE_GLYPH_H * g_info.pitch;
    // The real GPU backend owns its imported surfaces. The native console
    // needs no permanent full-screen RAM copy when graphics is inactive.
    memmove(g_framebuffer, reinterpret_cast<uint8_t*>(g_framebuffer) + row_bytes,
            static_cast<size_t>(g_rows - 1) * row_bytes);
    for (uint32_t column = 0; column < g_columns; ++column)
        fill_cell(column, g_rows - 1, ' ');
    if (g_console_row > 0) --g_console_row;
}

}  // namespace

bool initialize(uint64_t address, uint32_t pitch, uint32_t width, uint32_t height, uint8_t bpp,
                uint8_t color_type) {
    g_ready = false;
    if (bpp != 32 || color_type != kColourTypeRgb || width < SX_CONSOLE_GLYPH_W || height < SX_CONSOLE_GLYPH_H ||
        pitch % kBytesPerPixel != 0 || pitch < width * kBytesPerPixel) {
        return false;
    }
    const uint64_t bytes = static_cast<uint64_t>(pitch) * height;
    if (bytes == 0 || bytes > 0xFFFFFFFFull || !paging::map_identity(address, static_cast<uint32_t>(bytes))) {
        return false;
    }

    g_address = address;
    g_framebuffer = reinterpret_cast<uint32_t*>(static_cast<uintptr_t>(address));
    g_info.width = width;
    g_info.height = height;
    g_info.pitch = pitch;
    g_info.bpp = bpp;
    g_info.buffer_size = static_cast<uint32_t>(bytes);

    g_columns = width / SX_CONSOLE_GLYPH_W;
    g_rows = height / SX_CONSOLE_GLYPH_H;
    g_console_column = 0;
    g_console_row = 0;
    g_pointer_x = static_cast<int32_t>(width / 2);
    g_pointer_y = static_cast<int32_t>(height / 2);
    g_pointer_buttons = 0;
    g_ready = true;

    for (uint32_t row = 0; row < g_info.height; ++row) {
        uint32_t* line = g_framebuffer + static_cast<size_t>(row) * stride_pixels();
        for (uint32_t column = 0; column < g_info.width; ++column) {
            line[column] = kConsoleBackground;
        }
    }
    return true;
}

void set_external(void* address, const savanxp_fb_info& info) {
    g_address = reinterpret_cast<uintptr_t>(address);
    g_framebuffer = static_cast<uint32_t*>(address);
    g_info = info;
    g_columns = info.width / SX_CONSOLE_GLYPH_W;
    g_rows = info.height / SX_CONSOLE_GLYPH_H;
    g_console_column = g_console_row = 0;
    g_ready = address && info.bpp == 32 && info.width >= SX_CONSOLE_GLYPH_W &&
        info.height >= SX_CONSOLE_GLYPH_H && info.pitch % kBytesPerPixel == 0 &&
        static_cast<uint64_t>(info.pitch) >= static_cast<uint64_t>(info.width) * kBytesPerPixel;
    g_pointer_x = clamp_int(g_pointer_x, 0, info.width - 1);
    g_pointer_y = clamp_int(g_pointer_y, 0, info.height - 1);
}

bool ready() {
    return g_ready;
}

const savanxp_fb_info& info() {
    return g_info;
}

uint64_t address() {
    return g_address;
}

void clear(uint32_t colour) {
    if (!g_ready || !g_console_enabled) {
        return;
    }
    for (uint32_t row = 0; row < g_info.height; ++row) {
        uint32_t* line = g_framebuffer + static_cast<size_t>(row) * stride_pixels();
        for (uint32_t column = 0; column < g_info.width; ++column) {
            line[column] = colour;
        }
    }
    g_console_column = 0;
    g_console_row = 0;
}

void console_clear() {
    if (!g_ready) {
        return;
    }
    clear(kConsoleBackground);
}

void console_set_enabled(bool enabled) {
    g_console_enabled = enabled;
}

void console_write_char(char character) {
    if (!g_ready || !g_console_enabled) {
        return;
    }
    if (character == '\r') {
        g_console_column = 0;
        return;
    }
    if (character == '\n') {
        g_console_column = 0;
        if (++g_console_row >= g_rows) {
            scroll_up();
        }
        return;
    }
    if (character == '\b') {
        if (g_console_column > 0) {
            --g_console_column;
            fill_cell(g_console_column, g_console_row, ' ');
        }
        return;
    }
    fill_cell(g_console_column, g_console_row, character);
    if (++g_console_column >= g_columns) {
        g_console_column = 0;
        if (++g_console_row >= g_rows) {
            scroll_up();
        }
    }
}

void pointer_move(int32_t delta_x, int32_t delta_y) {
    if (!g_ready || (delta_x == 0 && delta_y == 0)) {
        return;
    }
    g_pointer_x = clamp_int(g_pointer_x + delta_x, 0, static_cast<int>(g_info.width) - 1);
    g_pointer_y = clamp_int(g_pointer_y + delta_y, 0, static_cast<int>(g_info.height) - 1);
}

void pointer_set_buttons(uint32_t buttons) {
    g_pointer_buttons = buttons;
}

void pointer_position(int32_t& x, int32_t& y) {
    x = g_pointer_x;
    y = g_pointer_y;
}

uint32_t pointer_buttons() {
    return g_pointer_buttons;
}

}  // namespace fb32

}  // namespace i386
