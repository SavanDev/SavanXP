#pragma once

#include <stdarg.h>

#include "boot/boot_info.hpp"

namespace console {

void early_init();
void init(const boot::BootInfo& boot_info);
void clear();
void write_char(char character);
void write(const char* text);
void write_line(const char* text);
void vprintf(const char* format, va_list args);
void printf(const char* format, ...);

} // namespace console
