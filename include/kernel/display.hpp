#pragma once

#include <stddef.h>
#include <stdint.h>

#include "savanxp/syscall.h"

// Solo se pasa por referencia a los probes: los consumidores de display.hpp no
// necesitan arrastrar la definicion completa de boot_info.
namespace boot {
struct FramebufferInfo;
}

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

// Un driver de display candidato. El core no conoce a ninguno por nombre: cada
// driver se registra y bind_best() corre los probes en orden de prioridad,
// quedandose con el primero que reclame el hardware. Agregar un backend nuevo
// pasa a ser una linea de register_driver() en vez de una rama en kernel_main.
struct Driver {
    const char* name;
    // Mayor gana. El orden de registro no importa.
    int priority;
    // Inicializa el driver y responde si reclama el hardware. bind_best() corta
    // en el primer true, asi que un driver descartado por prioridad nunca llega
    // a inicializarse: es exactamente lo que hoy hace a mano el else de
    // kernel_main al no tocar fb_gpu cuando virtio-gpu esta presente.
    bool (*probe)(const boot::FramebufferInfo& framebuffer);
    // Solo valido despues de un probe() que devolvio true.
    const Backend& (*backend)();
};

constexpr size_t kMaxDrivers = 8;

// false si la tabla esta llena o el driver viene incompleto.
bool register_driver(const Driver& driver);
// Corre los probes por prioridad descendente y ata el backend del primero que
// reclama el hardware. Devuelve el driver elegido, o nullptr si ninguno lo hizo
// (en cuyo caso no queda backend atado y display::ready() sigue en false).
const Driver* bind_best(const boot::FramebufferInfo& framebuffer);
// El driver que ato bind_best(), o nullptr.
const Driver* bound_driver();

// Mecanismo de bajo nivel detras de bind_best(). Queda expuesto para los
// caminos que atan un backend a mano (arranques headless, pruebas).
void set_backend(const Backend& backend);

bool ready();
void poll();

const savanxp_fb_info& framebuffer_info();
void* framebuffer_address();

bool acquire_session(uint32_t pid);
void release_session(uint32_t pid);
bool owns_session(uint32_t pid);
/* Release the graphics session and its backend resources, but only if `pid`
   currently owns the session. Safe to call for any exiting process: it is a
   no-op unless that pid held the session, so it reclaims the GPU when a client
   (e.g. the compositor daemon) is killed instead of closing its fd cleanly. */
void release_session_for(uint32_t pid);

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
