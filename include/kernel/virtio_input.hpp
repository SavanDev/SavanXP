#pragma once

#include "boot/boot_info.hpp"

namespace virtio_input {

void initialize(const boot::FramebufferInfo& framebuffer);
void poll();
bool mouse_ready();
void set_framebuffer_extent(uint32_t width, uint32_t height);
void begin_graphics_session();
void end_graphics_session();

} // namespace virtio_input
