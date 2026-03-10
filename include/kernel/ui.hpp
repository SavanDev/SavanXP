#pragma once

#include "boot/boot_info.hpp"

namespace ui {

void initialize(const boot::FramebufferInfo& framebuffer);
bool ready();
bool graphics_active();
void handle_key_event(uint32_t key, bool pressed, char ascii);
bool framebuffer_available();

} // namespace ui
