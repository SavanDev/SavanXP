#pragma once

#include <stdint.h>

#include "savanxp/syscall.h"

namespace i386 {

namespace fb32 {

/* Native text console and pointer coordinates; the shared display backend
 * owns user presentation and the compositor draws its own cursor. */

/* Accept 32-bpp BGRX video only. Unsupported modes leave serial available. */
bool initialize(uint64_t address, uint32_t pitch, uint32_t width, uint32_t height, uint8_t bpp,
                uint8_t color_type);

bool ready();
void set_external(void* address, const savanxp_fb_info& info);

const savanxp_fb_info& info();
uint64_t address();

/* BGRX colours: 0x00RRGGBB, matching gfx_rgb(). */
void clear(uint32_t colour);
/* 8x16 text console mirroring serial output until a client acquires video. */
void console_clear();
void console_set_enabled(bool enabled);
void console_write_char(char character);

/* Accumulate PS/2 deltas for native input diagnostics. */
void pointer_move(int32_t delta_x, int32_t delta_y);
void pointer_set_buttons(uint32_t buttons);
void pointer_position(int32_t& x, int32_t& y);
uint32_t pointer_buttons();

}  // namespace fb32

}  // namespace i386
