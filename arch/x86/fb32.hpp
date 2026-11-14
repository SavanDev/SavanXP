#pragma once

#include <stdint.h>

#include "savanxp/syscall.h"

namespace i386 {

namespace fb32 {

/* Linear Multiboot2 framebuffer: kernel console/cursor and ring 3
 * presentation through /dev/gpu0. */

/* Accept 32-bpp BGRX video only. Unsupported modes leave serial available. */
bool initialize(uint64_t address, uint32_t pitch, uint32_t width, uint32_t height, uint8_t bpp,
                uint8_t color_type);

bool ready();

const savanxp_fb_info& info();
uint64_t address();

/* BGRX colours: 0x00RRGGBB, matching gfx_rgb(). */
void clear(uint32_t colour);
void fill_rect(int x, int y, int width, int height, uint32_t colour);

/* Copy a client region, clipped to the screen. source_pitch is in pixels. */
bool present(const uint32_t* pixels, uint32_t source_pitch, uint32_t x, uint32_t y, uint32_t width,
             uint32_t height);

/* 8x16 text console mirroring serial output until a client acquires video. */
void console_clear();
void console_set_enabled(bool enabled);
void console_write_char(char character);

/* Accumulate PS/2 deltas and compose the cursor over the backing store. */
void pointer_move(int32_t delta_x, int32_t delta_y);
void pointer_set_buttons(uint32_t buttons);
void pointer_position(int32_t& x, int32_t& y);
uint32_t pointer_buttons();

}  // namespace fb32

}  // namespace i386
