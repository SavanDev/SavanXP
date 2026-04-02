#pragma once

#include <stddef.h>

#include "boot/boot_info.hpp"
#include "savanxp/syscall.h"

namespace virtio_gpu {

void initialize(const boot::FramebufferInfo& framebuffer);
bool ready();
bool present();
void poll();
const savanxp_fb_info& framebuffer_info();
void* framebuffer_address();
void wait_for_idle();
bool flush();
bool flush_rect(uint32_t x, uint32_t y, uint32_t width, uint32_t height);
bool present_from_kernel(const void* pixels, size_t byte_count);
bool present_region_from_kernel(const void* pixels, uint32_t source_pitch, uint32_t x, uint32_t y, uint32_t width, uint32_t height);

} // namespace virtio_gpu
