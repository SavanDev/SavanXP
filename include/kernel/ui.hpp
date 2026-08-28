#pragma once

#include "boot/boot_info.hpp"
#include "savanxp/syscall.h"

namespace ui {

void initialize(const boot::FramebufferInfo& framebuffer);
bool ready();
bool graphics_active();
bool acquire_graphics_session(uint32_t pid);
void release_graphics_session(uint32_t pid);
bool owns_graphics_session(uint32_t pid);
void handle_key_event(uint32_t key, bool pressed, char ascii, uint32_t modifiers);
void handle_mouse_event(int32_t delta_x, int32_t delta_y, uint32_t buttons);
// Re-lee la geometria del backend de display y la propaga a quien la tenga
// cacheada (hoy, el mapeo de coordenadas absolutas del puntero). La llama el
// dispatcher de /dev/gpu0 despues de un cambio de modo.
void sync_framebuffer_geometry();
bool framebuffer_available();
const savanxp_fb_info& framebuffer_info();
bool mouse_available();

} // namespace ui
