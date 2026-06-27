#pragma once

#include <stddef.h>

#include "boot/boot_info.hpp"
#include "kernel/display.hpp"
#include "savanxp/syscall.h"

namespace virtio_gpu {

void initialize(const boot::FramebufferInfo& framebuffer);
const display::Backend& backend();
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
bool get_info(savanxp_gpu_info& info);
bool set_mode(savanxp_gpu_mode& mode);
bool import_surface(savanxp_gpu_surface_import& request);
bool release_surface(uint32_t surface_id);
bool present_surface_region(const savanxp_gpu_surface_present& request);
bool get_stats(savanxp_gpu_stats& stats);
bool get_scanouts(savanxp_gpu_scanout_state& state);
bool refresh_scanouts_now();
bool get_connector_properties(savanxp_gpu_connector_properties& properties);
bool set_cursor(const savanxp_gpu_cursor_image& image);
bool move_cursor(const savanxp_gpu_cursor_position& position);
bool get_present_timeline(savanxp_gpu_present_timeline& timeline);
bool wait_present(savanxp_gpu_present_wait& request);
bool present_surface_batch(const savanxp_gpu_surface_present_batch& request);
void release_session_resources();
int create_present_event();
int create_scanout_event();

} // namespace virtio_gpu
