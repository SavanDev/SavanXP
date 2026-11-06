#include "kernel/display.hpp"

#include "kernel/ui.hpp"

namespace display {

namespace {
const Backend* g_backend = nullptr;
savanxp_fb_info g_empty_fb_info = {};

const Driver* g_drivers[kMaxDrivers] = {};
size_t g_driver_count = 0;
const Driver* g_bound_driver = nullptr;
} // namespace

void set_backend(const Backend& backend) { g_backend = &backend; }

bool register_driver(const Driver& driver) {
    if (g_driver_count >= kMaxDrivers || driver.probe == nullptr || driver.backend == nullptr) {
        return false;
    }
    g_drivers[g_driver_count] = &driver;
    ++g_driver_count;
    return true;
}

const Driver* bind_best(const boot::FramebufferInfo& framebuffer) {
    // Seleccion directa sobre el array en vez de ordenarlo: son un punado de
    // drivers y asi la tabla registrada queda intacta. Cada vuelta toma el de
    // mayor prioridad que todavia no se probo.
    bool probed[kMaxDrivers] = {};
    for (size_t attempt = 0; attempt < g_driver_count; ++attempt) {
        size_t best = kMaxDrivers;
        for (size_t i = 0; i < g_driver_count; ++i) {
            if (probed[i]) {
                continue;
            }
            if (best == kMaxDrivers || g_drivers[i]->priority > g_drivers[best]->priority) {
                best = i;
            }
        }
        if (best == kMaxDrivers) {
            break;
        }

        probed[best] = true;
        const Driver* candidate = g_drivers[best];
        if (candidate->probe(framebuffer)) {
            set_backend(candidate->backend());
            g_bound_driver = candidate;
            return candidate;
        }
    }
    return nullptr;
}

const Driver* bound_driver() { return g_bound_driver; }

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
int create_present_event() { return g_backend != nullptr ? g_backend->create_present_event() : -static_cast<int>(SX_ENOSYS); }
int create_scanout_event() { return g_backend != nullptr ? g_backend->create_scanout_event() : -static_cast<int>(SX_ENOSYS); }

namespace {

constexpr int kNo3d = -static_cast<int>(SX_ENODEV);

} // namespace

int gpu3d_capset(uint32_t& capset_id, uint32_t& capset_version, const uint8_t*& data, uint32_t& size) {
    return g_backend != nullptr && g_backend->gpu3d_capset != nullptr
        ? g_backend->gpu3d_capset(capset_id, capset_version, data, size) : kNo3d;
}
int gpu3d_create_context(uint32_t pid, const char* debug_name, uint32_t& context_id) {
    return g_backend != nullptr && g_backend->gpu3d_create_context != nullptr
        ? g_backend->gpu3d_create_context(pid, debug_name, context_id) : kNo3d;
}
int gpu3d_destroy_context(uint32_t pid, uint32_t context_id) {
    return g_backend != nullptr && g_backend->gpu3d_destroy_context != nullptr
        ? g_backend->gpu3d_destroy_context(pid, context_id) : kNo3d;
}
int gpu3d_create_resource(uint32_t pid, savanxp_gpu_3d_resource& request) {
    return g_backend != nullptr && g_backend->gpu3d_create_resource != nullptr
        ? g_backend->gpu3d_create_resource(pid, request) : kNo3d;
}
int gpu3d_destroy_resource(uint32_t pid, uint32_t context_id, uint32_t resource_id) {
    return g_backend != nullptr && g_backend->gpu3d_destroy_resource != nullptr
        ? g_backend->gpu3d_destroy_resource(pid, context_id, resource_id) : kNo3d;
}
int gpu3d_submit(uint32_t pid, uint32_t context_id, const void* commands, uint32_t size) {
    return g_backend != nullptr && g_backend->gpu3d_submit != nullptr
        ? g_backend->gpu3d_submit(pid, context_id, commands, size) : kNo3d;
}
int gpu3d_transfer(uint32_t pid, const savanxp_gpu_3d_transfer& request) {
    return g_backend != nullptr && g_backend->gpu3d_transfer != nullptr
        ? g_backend->gpu3d_transfer(pid, request) : kNo3d;
}
void gpu3d_release_for(uint32_t pid) {
    if (g_backend != nullptr && g_backend->gpu3d_release_for != nullptr) {
        g_backend->gpu3d_release_for(pid);
    }
}

} // namespace display
