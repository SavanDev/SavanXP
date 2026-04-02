#pragma once

#include <stdarg.h>

#include "boot/boot_info.hpp"
#include "savanxp/syscall.h"

namespace console {

void early_init();
void init(const boot::BootInfo& boot_info);
const boot::FramebufferInfo& framebuffer_info();
bool framebuffer_ready();
void set_framebuffer_console_enabled(bool enabled);
void set_external_framebuffer(void* address, const savanxp_fb_info& info);
void redraw();
bool present_pixels(const void* pixels, size_t byte_count);
bool present_region(const void* pixels, uint32_t source_pitch, uint32_t x, uint32_t y, uint32_t width, uint32_t height);
void clear();
void write_char(char character);
void write(const char* text);
void write_line(const char* text);
void vprintf(const char* format, va_list args);
void printf(const char* format, ...);

} // namespace console
