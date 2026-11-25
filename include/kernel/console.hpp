#pragma once

#include <stdarg.h>
#include <stddef.h>

#include "savanxp/syscall.h"

namespace console {

void set_external_framebuffer(void* address, const savanxp_fb_info& info);
// Write only to the serial port, including while a graphics session is active.
void serial_write(const char* text, size_t length);
void write(const char* text);
void write_line(const char* text);
void vprintf(const char* format, va_list args);
void printf(const char* format, ...);

} // namespace console
