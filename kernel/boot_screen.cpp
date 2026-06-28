#include "kernel/boot_screen.hpp"

#include <stddef.h>
#include <stdint.h>

#include "shared/version.h"

namespace boot_screen {
namespace {

constexpr uint32_t kBackground = 0x00070b14U;
constexpr uint32_t kPanel = 0x00101724U;
constexpr uint32_t kPanelBorder = 0x003d5568U;
constexpr uint32_t kPrimary = 0x00d8f3ffU;
constexpr uint32_t kMuted = 0x008aa0adU;
constexpr uint32_t kAccent = 0x0027d37bU;
constexpr uint32_t kAccentDark = 0x00116448U;
constexpr uint32_t kTrack = 0x001d2934U;
constexpr uint32_t kTrackEdge = 0x006d8492U;

#include "kernel/console_font_unifont.inc"

boot::FramebufferInfo g_framebuffer = {};
bool g_ready = false;

size_t text_length(const char* text) {
    size_t length = 0;
    while (text != nullptr && text[length] != '\0') {
        ++length;
    }
    return length;
}

uint64_t min_u64(uint64_t a, uint64_t b) {
    return a < b ? a : b;
}

void plot_pixel(uint64_t x, uint64_t y, uint32_t colour) {
    if (!g_ready || x >= g_framebuffer.width || y >= g_framebuffer.height) {
        return;
    }

    auto* pixels = static_cast<uint8_t*>(g_framebuffer.address);
    auto* row = reinterpret_cast<uint32_t*>(pixels + (y * g_framebuffer.pitch));
    row[x] = colour;
}

void fill_rect(uint64_t x, uint64_t y, uint64_t width, uint64_t height, uint32_t colour) {
    if (!g_ready || width == 0 || height == 0) {
        return;
    }

    const uint64_t end_x = min_u64(x + width, g_framebuffer.width);
    const uint64_t end_y = min_u64(y + height, g_framebuffer.height);
    for (uint64_t row = y; row < end_y; ++row) {
        for (uint64_t column = x; column < end_x; ++column) {
            plot_pixel(column, row, colour);
        }
    }
}

void draw_frame(uint64_t x, uint64_t y, uint64_t width, uint64_t height, uint32_t colour) {
    if (width < 2 || height < 2) {
        return;
    }
    fill_rect(x, y, width, 1, colour);
    fill_rect(x, y + height - 1, width, 1, colour);
    fill_rect(x, y, 1, height, colour);
    fill_rect(x + width - 1, y, 1, height, colour);
}

void draw_glyph(uint64_t x, uint64_t y, char character, uint32_t scale, uint32_t colour) {
    const auto* glyph = sx_console_glyph(static_cast<unsigned char>(character));
    for (uint64_t row = 0; row < SX_CONSOLE_GLYPH_H; ++row) {
        for (uint64_t column = 0; column < SX_CONSOLE_GLYPH_W; ++column) {
            if ((glyph[row] & (1u << (SX_CONSOLE_GLYPH_W - 1u - column))) == 0) {
                continue;
            }
            fill_rect(x + (column * scale), y + (row * scale), scale, scale, colour);
        }
    }
}

void draw_text(uint64_t x, uint64_t y, const char* text, uint32_t scale, uint32_t colour) {
    if (text == nullptr || scale == 0) {
        return;
    }

    for (size_t index = 0; text[index] != '\0'; ++index) {
        draw_glyph(x + (index * SX_CONSOLE_GLYPH_W * scale), y, text[index], scale, colour);
    }
}

void draw_centered_text(uint64_t y, const char* text, uint32_t scale, uint32_t colour) {
    const uint64_t width = text_length(text) * SX_CONSOLE_GLYPH_W * scale;
    const uint64_t x = g_framebuffer.width > width ? (g_framebuffer.width - width) / 2 : 0;
    draw_text(x, y, text, scale, colour);
}

} // namespace

void initialize(const boot::FramebufferInfo& framebuffer) {
    g_framebuffer = framebuffer;
    g_ready = framebuffer.available &&
        framebuffer.address != nullptr &&
        framebuffer.width >= 320 &&
        framebuffer.height >= 200 &&
        framebuffer.pitch >= framebuffer.width * sizeof(uint32_t) &&
        framebuffer.bpp == 32;
}

bool ready() {
    return g_ready;
}

void show(uint32_t progress_percent, const char* status) {
    if (!g_ready) {
        return;
    }
    if (progress_percent > 100u) {
        progress_percent = 100u;
    }

    const uint64_t panel_width = g_framebuffer.width > 760 ? 560 : (g_framebuffer.width - 48);
    const uint64_t panel_height = 176;
    const uint64_t panel_x = (g_framebuffer.width - panel_width) / 2;
    const uint64_t panel_y = g_framebuffer.height > panel_height
        ? (g_framebuffer.height - panel_height) / 2
        : 12;
    const uint64_t title_y = panel_y + 36;
    const uint64_t version_y = title_y + (SX_CONSOLE_GLYPH_H * 3) + 10;
    const uint64_t bar_x = panel_x + 44;
    const uint64_t bar_y = panel_y + panel_height - 58;
    const uint64_t bar_width = panel_width - 88;
    const uint64_t bar_height = 18;
    const uint64_t fill_width = (bar_width - 4) * progress_percent / 100u;

    fill_rect(0, 0, g_framebuffer.width, g_framebuffer.height, kBackground);
    fill_rect(panel_x, panel_y, panel_width, panel_height, kPanel);
    draw_frame(panel_x, panel_y, panel_width, panel_height, kPanelBorder);
    fill_rect(panel_x + 1, panel_y + 1, panel_width - 2, 2, kAccentDark);

    draw_centered_text(title_y, SAVANXP_SYSTEM_NAME, 3, kPrimary);
    draw_centered_text(version_y, SAVANXP_VERSION_STRING, 1, kMuted);

    draw_frame(bar_x, bar_y, bar_width, bar_height, kTrackEdge);
    fill_rect(bar_x + 2, bar_y + 2, bar_width - 4, bar_height - 4, kTrack);
    fill_rect(bar_x + 2, bar_y + 2, fill_width, bar_height - 4, kAccent);

    draw_centered_text(bar_y + bar_height + 14, status != nullptr ? status : "Iniciando", 1, kMuted);
}

} // namespace boot_screen
