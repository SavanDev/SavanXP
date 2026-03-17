#pragma once

#include "boot/boot_info.hpp"
#include "shared/syscall.h"

namespace ui {

void initialize(const boot::FramebufferInfo& framebuffer);
bool ready();
bool graphics_active();
bool acquire_graphics_session(uint32_t pid);
void release_graphics_session(uint32_t pid);
bool owns_graphics_session(uint32_t pid);
void handle_key_event(uint32_t key, bool pressed, char ascii);
void handle_mouse_event(int32_t delta_x, int32_t delta_y, uint32_t buttons);
bool framebuffer_available();
const savanxp_fb_info& framebuffer_info();
bool mouse_available();

} // namespace ui
