#pragma once

#include <stddef.h>
#include <stdint.h>

#include "savanxp/syscall.h"

namespace display {

// Vtable de operaciones dependientes de hardware. Cada backend (virtio-gpu, framebuffer
// plano) llena una instancia estática con punteros a sus propias funciones públicas;
// display:: solo despacha. Las funciones de sesión (acquire/release/owns) no están acá
// porque gestionan quién tiene la sesión gráfica, no el hardware, y van directo a ui::.
struct Backend {
    bool (*ready)();
    void (*poll)();
    const savanxp_fb_info& (*framebuffer_info)();
    void* (*framebuffer_address)();
    void (*wait_for_idle)();
    bool (*flush)();
    bool (*flush_rect)(uint32_t x, uint32_t y, uint32_t width, uint32_t height);
    bool (*present)(const void* pixels, size_t byte_count);
    bool (*present_region)(const void* pixels, uint32_t source_pitch, uint32_t x, uint32_t y, uint32_t width, uint32_t height);
    bool (*get_info)(savanxp_gpu_info& info);
    bool (*get_connector_properties)(savanxp_gpu_connector_properties& properties);
    bool (*set_mode)(savanxp_gpu_mode& mode);
    bool (*import_surface)(savanxp_gpu_surface_import& request);
    bool (*release_surface)(uint32_t surface_id);
    bool (*present_surface_region)(const savanxp_gpu_surface_present& request);
    bool (*present_surface_batch)(const savanxp_gpu_surface_present_batch& request);
    bool (*get_stats)(savanxp_gpu_stats& stats);
    bool (*get_scanouts)(savanxp_gpu_scanout_state& state);
    bool (*refresh_scanouts)();
    bool (*set_cursor)(const savanxp_gpu_cursor_image& image);
    bool (*move_cursor)(const savanxp_gpu_cursor_position& position);
    bool (*get_present_timeline)(savanxp_gpu_present_timeline& timeline);
    bool (*wait_present)(savanxp_gpu_present_wait& request);
    // Limpieza de recursos atados a la sesión gráfica (superficies importadas,
    // plano de cursor). La llaman GPU_IOC_RELEASE y el close del device.
    void (*release_session_resources)();
    // Exportan un handle de evento al proceso actual para presentación/scanout
    // asíncronos. Backends síncronos (framebuffer plano) devuelven -ENOSYS.
    int (*create_present_event)();
    int (*create_scanout_event)();
};

void set_backend(const Backend& backend);

bool ready();
void poll();

const savanxp_fb_info& framebuffer_info();
void* framebuffer_address();

bool acquire_session(uint32_t pid);
void release_session(uint32_t pid);
bool owns_session(uint32_t pid);

bool wait_for_idle();
bool flush();
bool flush_rect(uint32_t x, uint32_t y, uint32_t width, uint32_t height);

bool present(const void* pixels, size_t byte_count);
bool present_region(const void* pixels, uint32_t source_pitch, uint32_t x, uint32_t y, uint32_t width, uint32_t height);

bool get_info(savanxp_gpu_info& info);
bool get_connector_properties(savanxp_gpu_connector_properties& properties);
bool set_mode(savanxp_gpu_mode& mode);
bool import_surface(savanxp_gpu_surface_import& request);
bool release_surface(uint32_t surface_id);
bool present_surface_region(const savanxp_gpu_surface_present& request);
bool present_surface_batch(const savanxp_gpu_surface_present_batch& request);
bool get_stats(savanxp_gpu_stats& stats);
bool get_scanouts(savanxp_gpu_scanout_state& state);
bool refresh_scanouts();
bool set_cursor(const savanxp_gpu_cursor_image& image);
bool move_cursor(const savanxp_gpu_cursor_position& position);
bool get_present_timeline(savanxp_gpu_present_timeline& timeline);
bool wait_present(savanxp_gpu_present_wait& request);
void release_session_resources();
int create_present_event();
int create_scanout_event();

} // namespace display
