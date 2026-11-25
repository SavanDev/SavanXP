#pragma once

#include <stdint.h>

namespace ui {

bool graphics_active();
bool acquire_graphics_session(uint32_t pid);
void release_graphics_session(uint32_t pid);
bool owns_graphics_session(uint32_t pid);
// Propagate the display geometry to the framebuffer console and absolute pointer.
void sync_framebuffer_geometry();

} // namespace ui
