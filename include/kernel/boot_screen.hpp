#pragma once

#include <stdint.h>

#include "boot/boot_info.hpp"

namespace boot_screen {

void initialize(const boot::FramebufferInfo& framebuffer);
bool ready();
void show(uint32_t progress_percent, const char* status);

} // namespace boot_screen
