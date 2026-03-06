#pragma once

#include <stdarg.h>

#include "boot/boot_info.hpp"

namespace console {

void early_init();
void init(const boot::BootInfo& boot_info);
void write(const char* text);
void write_line(const char* text);
void vprintf(const char* format, va_list args);
void printf(const char* format, ...);
void show_welcome_screen(const char* const* lines, size_t line_count);

} // namespace console
