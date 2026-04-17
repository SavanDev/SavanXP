#include "kernel/display.hpp"

#include "kernel/ui.hpp"
#include "kernel/virtio_gpu.hpp"

namespace display {

bool ready() { return virtio_gpu::ready(); }
void poll() { virtio_gpu::poll(); }
const savanxp_fb_info& framebuffer_info() { return virtio_gpu::framebuffer_info(); }
void* framebuffer_address() { return virtio_gpu::framebuffer_address(); }

bool acquire_session(uint32_t pid) { return ui::acquire_graphics_session(pid); }
void release_session(uint32_t pid) { ui::release_graphics_session(pid); }
bool owns_session(uint32_t pid) { return ui::owns_graphics_session(pid); }

bool wait_for_idle()
{
    virtio_gpu::wait_for_idle();
    return true;
}

bool flush() { return virtio_gpu::flush(); }
bool flush_rect(uint32_t x, uint32_t y, uint32_t width, uint32_t height) { return virtio_gpu::flush_rect(x, y, width, height); }
bool present(const void* pixels, size_t byte_count) { return virtio_gpu::present_from_kernel(pixels, byte_count); }
bool present_region(const void* pixels, uint32_t source_pitch, uint32_t x, uint32_t y, uint32_t width, uint32_t height)
{
    return virtio_gpu::present_region_from_kernel(pixels, source_pitch, x, y, width, height);
}

bool get_info(savanxp_gpu_info& info) { return virtio_gpu::get_info(info); }
bool get_connector_properties(savanxp_gpu_connector_properties& properties) { return virtio_gpu::get_connector_properties(properties); }
bool set_mode(savanxp_gpu_mode& mode) { return virtio_gpu::set_mode(mode); }
bool import_surface(savanxp_gpu_surface_import& request) { return virtio_gpu::import_surface(request); }
bool release_surface(uint32_t surface_id) { return virtio_gpu::release_surface(surface_id); }
bool present_surface_region(const savanxp_gpu_surface_present& request) { return virtio_gpu::present_surface_region(request); }
bool present_surface_batch(const savanxp_gpu_surface_present_batch& request) { return virtio_gpu::present_surface_batch(request); }
bool get_stats(savanxp_gpu_stats& stats) { return virtio_gpu::get_stats(stats); }
bool get_scanouts(savanxp_gpu_scanout_state& state) { return virtio_gpu::get_scanouts(state); }
bool refresh_scanouts() { return virtio_gpu::refresh_scanouts_now(); }
bool set_cursor(const savanxp_gpu_cursor_image& image) { return virtio_gpu::set_cursor(image); }
bool move_cursor(const savanxp_gpu_cursor_position& position) { return virtio_gpu::move_cursor(position); }
bool get_present_timeline(savanxp_gpu_present_timeline& timeline) { return virtio_gpu::get_present_timeline(timeline); }
bool wait_present(savanxp_gpu_present_wait& request) { return virtio_gpu::wait_present(request); }

} // namespace display
