#pragma once

#include "boot/boot_info.hpp"

namespace ui {

void initialize(const boot::FramebufferInfo& framebuffer);
bool ready();
bool graphics_active();
void handle_key_event(uint32_t key, bool pressed, char ascii);
void handle_mouse_event(int32_t delta_x, int32_t delta_y, uint32_t buttons);
bool framebuffer_available();
bool mouse_available();

} // namespace ui
