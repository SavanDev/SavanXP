#include "fb32.hpp"

#include <stddef.h>

#include "kernel/console_font_unifont.inc"
#include "paging.hpp"

namespace i386 {

namespace fb32 {

namespace {

/* BGRX pixels use 0x00RRGGBB, matching gfx_rgb() and x86_64. */
constexpr uint32_t kColourTypeRgb = 1;
constexpr uint32_t kBytesPerPixel = 4;

/* Bound the static backing store to the requested 1024x768 mode; no heap yet. */
constexpr uint32_t kDesktopMaxWidth = 1024;
constexpr uint32_t kDesktopMaxHeight = 768;

constexpr uint32_t kConsoleBackground = 0x00101824u;
constexpr uint32_t kConsoleForeground = 0x00C8D4E0u;

/* 11x18 cursor: white fill, dark outline, spaces transparent. */
constexpr uint32_t kPointerWidth = 11;
constexpr uint32_t kPointerHeight = 18;
constexpr uint32_t kPointerLight = 0x00F4F8FFu;
constexpr uint32_t kPointerDark = 0x00101418u;

const char* const kPointerSprite[kPointerHeight] = {
    "o          ",
    "oo         ",
    "o#o        ",
    "o##o       ",
    "o###o      ",
    "o####o     ",
    "o#####o    ",
    "o######o   ",
    "o#######o  ",
    "o########o ",
    "o#####oooo ",
    "o##o#o     ",
    "o#o o#o    ",
    "oo  o#o    ",
    "o    o#o   ",
    "     o#o   ",
    "      o#o  ",
    "       oo  ",
};

bool g_ready = false;
bool g_console_enabled = true;
uint64_t g_address = 0;
uint32_t* g_framebuffer = nullptr;
savanxp_fb_info g_info = {};

/* Console and user pixels share this backing store; the cursor is composed
 * only on output, so console scroll and user repaints cannot copy its image. */
alignas(16) uint32_t g_desktop[kDesktopMaxWidth * kDesktopMaxHeight] = {};

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

inline void set_desktop_pixel(uint32_t x, uint32_t y, uint32_t colour) {
    g_desktop[static_cast<size_t>(y) * g_info.width + x] = colour;
}

bool pointer_pixel(uint32_t x, uint32_t y, uint32_t& colour) {
    const int32_t local_x = static_cast<int32_t>(x) - g_pointer_x;
    const int32_t local_y = static_cast<int32_t>(y) - g_pointer_y;
    if (local_x < 0 || local_y < 0 || local_x >= static_cast<int32_t>(kPointerWidth) ||
        local_y >= static_cast<int32_t>(kPointerHeight)) {
        return false;
    }
    const char pixel = kPointerSprite[local_y][local_x];
    if (pixel == '#') {
        colour = kPointerLight;
        return true;
    }
    if (pixel == 'o') {
        colour = kPointerDark;
        return true;
    }
    return false;
}

/* Copy backing pixels to video memory with the cursor composed on top. */
void flush(int x, int y, int width, int height) {
    if (!g_ready || width <= 0 || height <= 0) {
        return;
    }
    const int left = clamp_int(x, 0, static_cast<int>(g_info.width));
    const int top = clamp_int(y, 0, static_cast<int>(g_info.height));
    const int right = clamp_int(x + width, 0, static_cast<int>(g_info.width));
    const int bottom = clamp_int(y + height, 0, static_cast<int>(g_info.height));
    const uint32_t stride = stride_pixels();
    for (int row = top; row < bottom; ++row) {
        uint32_t* destination = g_framebuffer + static_cast<size_t>(row) * stride;
        const uint32_t* source = g_desktop + static_cast<size_t>(row) * g_info.width;
        for (int column = left; column < right; ++column) {
            uint32_t colour = 0;
            destination[column] =
                pointer_pixel(static_cast<uint32_t>(column), static_cast<uint32_t>(row), colour)
                    ? colour
                    : source[column];
        }
    }
}

void flush_all() {
    flush(0, 0, static_cast<int>(g_info.width), static_cast<int>(g_info.height));
}

void fill_cell(uint32_t column, uint32_t row, char character) {
    const uint32_t x = column * SX_CONSOLE_GLYPH_W;
    const uint32_t y = row * SX_CONSOLE_GLYPH_H;
    for (uint32_t line = 0; line < SX_CONSOLE_GLYPH_H; ++line) {
        for (uint32_t pixel = 0; pixel < SX_CONSOLE_GLYPH_W; ++pixel) {
            set_desktop_pixel(x + pixel, y + line, kConsoleBackground);
        }
    }
    const uint8_t* glyph =
        sx_console_glyph(static_cast<unsigned int>(static_cast<uint8_t>(character)));
    for (uint32_t line = 0; line < SX_CONSOLE_GLYPH_H; ++line) {
        const uint8_t bits = glyph[line];
        for (uint32_t pixel = 0; pixel < SX_CONSOLE_GLYPH_W; ++pixel) {
            if ((bits & (1u << (SX_CONSOLE_GLYPH_W - 1u - pixel))) != 0) {
                set_desktop_pixel(x + pixel, y + line, kConsoleForeground);
            }
        }
    }
}

void scroll_up() {
    const uint32_t cells_bytes = static_cast<uint32_t>(SX_CONSOLE_GLYPH_H) * g_info.width * 4u;
    uint8_t* base = reinterpret_cast<uint8_t*>(g_desktop);
    for (uint32_t row = 1; row < g_rows; ++row) {
        uint8_t* destination = base + static_cast<size_t>(row - 1) * cells_bytes;
        const uint8_t* source = base + static_cast<size_t>(row) * cells_bytes;
        for (uint32_t index = 0; index < cells_bytes; ++index) {
            destination[index] = source[index];
        }
    }
    for (uint32_t column = 0; column < g_columns; ++column) {
        fill_cell(column, g_rows - 1, ' ');
    }
    if (g_console_row > 0) {
        --g_console_row;
    }
}

}  // namespace

bool initialize(uint64_t address, uint32_t pitch, uint32_t width, uint32_t height, uint8_t bpp,
                uint8_t color_type) {
    g_ready = false;
    if (bpp != 32 || color_type != kColourTypeRgb || width < SX_CONSOLE_GLYPH_W || height < SX_CONSOLE_GLYPH_H ||
        width > kDesktopMaxWidth || height > kDesktopMaxHeight ||
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
        uint32_t* line = g_desktop + static_cast<size_t>(row) * g_info.width;
        for (uint32_t column = 0; column < g_info.width; ++column) {
            line[column] = kConsoleBackground;
        }
    }
    flush_all();
    return true;
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
    if (!g_ready) {
        return;
    }
    for (uint32_t row = 0; row < g_info.height; ++row) {
        uint32_t* line = g_desktop + static_cast<size_t>(row) * g_info.width;
        for (uint32_t column = 0; column < g_info.width; ++column) {
            line[column] = colour;
        }
    }
    g_console_column = 0;
    g_console_row = 0;
    flush_all();
}

void fill_rect(int x, int y, int width, int height, uint32_t colour) {
    if (!g_ready || width <= 0 || height <= 0) {
        return;
    }
    const int left = clamp_int(x, 0, static_cast<int>(g_info.width));
    const int top = clamp_int(y, 0, static_cast<int>(g_info.height));
    const int right = clamp_int(x + width, 0, static_cast<int>(g_info.width));
    const int bottom = clamp_int(y + height, 0, static_cast<int>(g_info.height));
    for (int row = top; row < bottom; ++row) {
        uint32_t* line = g_desktop + static_cast<size_t>(row) * g_info.width;
        for (int column = left; column < right; ++column) {
            line[column] = colour;
        }
    }
    flush(left, top, right - left, bottom - top);
}

bool present(const uint32_t* pixels, uint32_t source_pitch, uint32_t x, uint32_t y, uint32_t width,
             uint32_t height) {
    if (!g_ready || pixels == nullptr || width == 0 || height == 0 || source_pitch < width) {
        return false;
    }
    if (x >= g_info.width || y >= g_info.height) {
        return false;
    }
    const uint32_t copy_width = (width > g_info.width - x) ? g_info.width - x : width;
    const uint32_t copy_height = (height > g_info.height - y) ? g_info.height - y : height;
    for (uint32_t row = 0; row < copy_height; ++row) {
        const uint32_t* source = pixels + static_cast<size_t>(row) * source_pitch;
        uint32_t* destination = g_desktop + static_cast<size_t>(y + row) * g_info.width + x;
        for (uint32_t column = 0; column < copy_width; ++column) {
            destination[column] = source[column];
        }
    }
    flush(static_cast<int>(x), static_cast<int>(y), static_cast<int>(copy_width),
          static_cast<int>(copy_height));
    return true;
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
            flush_all();
        }
        return;
    }
    if (character == '\b') {
        if (g_console_column > 0) {
            --g_console_column;
            fill_cell(g_console_column, g_console_row, ' ');
            flush(static_cast<int>(g_console_column * SX_CONSOLE_GLYPH_W),
                  static_cast<int>(g_console_row * SX_CONSOLE_GLYPH_H), SX_CONSOLE_GLYPH_W,
                  SX_CONSOLE_GLYPH_H);
        }
        return;
    }
    fill_cell(g_console_column, g_console_row, character);
    flush(static_cast<int>(g_console_column * SX_CONSOLE_GLYPH_W),
          static_cast<int>(g_console_row * SX_CONSOLE_GLYPH_H), SX_CONSOLE_GLYPH_W,
          SX_CONSOLE_GLYPH_H);
    if (++g_console_column >= g_columns) {
        g_console_column = 0;
        if (++g_console_row >= g_rows) {
            scroll_up();
            flush_all();
        }
    }
}

void pointer_move(int32_t delta_x, int32_t delta_y) {
    if (!g_ready || (delta_x == 0 && delta_y == 0)) {
        return;
    }
    const int32_t previous_x = g_pointer_x;
    const int32_t previous_y = g_pointer_y;
    g_pointer_x = clamp_int(g_pointer_x + delta_x, 0, static_cast<int>(g_info.width) - 1);
    g_pointer_y = clamp_int(g_pointer_y + delta_y, 0, static_cast<int>(g_info.height) - 1);
    /* Restore the old cursor rectangle and compose the new one. */
    flush(previous_x, previous_y, static_cast<int>(kPointerWidth),
          static_cast<int>(kPointerHeight));
    flush(g_pointer_x, g_pointer_y, static_cast<int>(kPointerWidth),
          static_cast<int>(kPointerHeight));
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
