#include "kernel/display.hpp"

#include "kernel/ui.hpp"

namespace display {

namespace {
const Backend* g_backend = nullptr;
savanxp_fb_info g_empty_fb_info = {};
} // namespace

void set_backend(const Backend& backend) { g_backend = &backend; }

bool ready() { return g_backend != nullptr && g_backend->ready(); }
void poll() { if (g_backend != nullptr) { g_backend->poll(); } }
const savanxp_fb_info& framebuffer_info() { return g_backend != nullptr ? g_backend->framebuffer_info() : g_empty_fb_info; }
void* framebuffer_address() { return g_backend != nullptr ? g_backend->framebuffer_address() : nullptr; }

bool acquire_session(uint32_t pid) { return ui::acquire_graphics_session(pid); }
void release_session(uint32_t pid) { ui::release_graphics_session(pid); }
bool owns_session(uint32_t pid) { return ui::owns_graphics_session(pid); }

void release_session_for(uint32_t pid) {
    if (!ui::owns_graphics_session(pid)) {
        return;
    }
    if (g_backend != nullptr) {
        g_backend->release_session_resources();
    }
    ui::release_graphics_session(pid);
}

bool wait_for_idle() { if (g_backend != nullptr) { g_backend->wait_for_idle(); } return true; }
bool flush() { return g_backend != nullptr && g_backend->flush(); }
bool flush_rect(uint32_t x, uint32_t y, uint32_t width, uint32_t height) { return g_backend != nullptr && g_backend->flush_rect(x, y, width, height); }
bool present(const void* pixels, size_t byte_count) { return g_backend != nullptr && g_backend->present(pixels, byte_count); }
bool present_region(const void* pixels, uint32_t source_pitch, uint32_t x, uint32_t y, uint32_t width, uint32_t height)
{
    return g_backend != nullptr && g_backend->present_region(pixels, source_pitch, x, y, width, height);
}

bool get_info(savanxp_gpu_info& info) { return g_backend != nullptr && g_backend->get_info(info); }
bool get_connector_properties(savanxp_gpu_connector_properties& properties) { return g_backend != nullptr && g_backend->get_connector_properties(properties); }
bool set_mode(savanxp_gpu_mode& mode) { return g_backend != nullptr && g_backend->set_mode(mode); }
bool import_surface(savanxp_gpu_surface_import& request) { return g_backend != nullptr && g_backend->import_surface(request); }
bool release_surface(uint32_t surface_id) { return g_backend != nullptr && g_backend->release_surface(surface_id); }
bool present_surface_region(const savanxp_gpu_surface_present& request) { return g_backend != nullptr && g_backend->present_surface_region(request); }
bool present_surface_batch(const savanxp_gpu_surface_present_batch& request) { return g_backend != nullptr && g_backend->present_surface_batch(request); }
bool get_stats(savanxp_gpu_stats& stats) { return g_backend != nullptr && g_backend->get_stats(stats); }
bool get_scanouts(savanxp_gpu_scanout_state& state) { return g_backend != nullptr && g_backend->get_scanouts(state); }
bool refresh_scanouts() { return g_backend != nullptr && g_backend->refresh_scanouts(); }
bool set_cursor(const savanxp_gpu_cursor_image& image) { return g_backend != nullptr && g_backend->set_cursor(image); }
bool move_cursor(const savanxp_gpu_cursor_position& position) { return g_backend != nullptr && g_backend->move_cursor(position); }
bool get_present_timeline(savanxp_gpu_present_timeline& timeline) { return g_backend != nullptr && g_backend->get_present_timeline(timeline); }
bool wait_present(savanxp_gpu_present_wait& request) { return g_backend != nullptr && g_backend->wait_present(request); }
void release_session_resources() { if (g_backend != nullptr) { g_backend->release_session_resources(); } }
int create_present_event() { return g_backend != nullptr ? g_backend->create_present_event() : -static_cast<int>(SAVANXP_ENOSYS); }
int create_scanout_event() { return g_backend != nullptr ? g_backend->create_scanout_event() : -static_cast<int>(SAVANXP_ENOSYS); }

} // namespace display
