#include "kernel/fb_gpu.hpp"

#include <stddef.h>
#include <stdint.h>

#include "kernel/object.hpp"
#include "kernel/physical_memory.hpp"
#include "kernel/process.hpp"
#include "kernel/string.hpp"
#include "kernel/vmm.hpp"
#include "savanxp/syscall.h"

namespace fb_gpu {
namespace {

constexpr uint32_t kImportedSurfaceCount = 8u;

struct ImportedSurface {
    bool in_use;
    uint32_t surface_id;
    uint32_t flags;
    uint64_t page_count;
    savanxp_fb_info info;
    object::SectionObject* section;
    void* virtual_address;
};

// Pixeles del scanout: el framebuffer lineal del firmware. Las escrituras van
// directo aca, no hay paso de "flush al dispositivo".
void* g_fb_base = nullptr;
savanxp_fb_info g_fb_info = {};
savanxp_gpu_info g_gpu_info = {};
ImportedSurface g_imported[kImportedSurfaceCount] = {};
uint64_t g_next_present_sequence = 1;
uint64_t g_last_submitted_present_sequence = 0;
uint64_t g_last_retired_present_sequence = 0;

bool ready() {
    return g_fb_base != nullptr && g_fb_info.bpp == 32u && g_fb_info.buffer_size != 0;
}

void poll() {}

const savanxp_fb_info& framebuffer_info() {
    return g_fb_info;
}

void* framebuffer_address() {
    return g_fb_base;
}

void wait_for_idle() {}

void retire_synchronous_present(uint64_t requested_sequence) {
    const uint64_t sequence =
        requested_sequence != 0 && requested_sequence == g_next_present_sequence
            ? requested_sequence
            : g_next_present_sequence;
    g_next_present_sequence = sequence + 1u;
    g_last_submitted_present_sequence = sequence;
    g_last_retired_present_sequence = sequence;
}

// Copia un rectangulo (en coordenadas del scanout) desde una superficie origen
// al framebuffer. source == nullptr usa el propio framebuffer como origen (no-op
// util para los flush, que aca no necesitan transferir nada al hardware).
bool blit_rect(const void* source, uint32_t source_pitch, uint32_t x, uint32_t y, uint32_t width, uint32_t height) {
    if (!ready()) {
        return false;
    }
    if (width == 0 || height == 0 ||
        x >= g_fb_info.width || y >= g_fb_info.height ||
        width > (g_fb_info.width - x) || height > (g_fb_info.height - y)) {
        return false;
    }
    if (source == nullptr) {
        return true;
    }
    if (source_pitch < (width * sizeof(uint32_t))) {
        return false;
    }

    auto* destination = static_cast<uint8_t*>(g_fb_base);
    const auto* origin = static_cast<const uint8_t*>(source);
    const uint64_t row_bytes = static_cast<uint64_t>(width) * sizeof(uint32_t);
    for (uint32_t row = 0; row < height; ++row) {
        uint8_t* destination_row = destination + (static_cast<uint64_t>(y + row) * g_fb_info.pitch) +
            (static_cast<uint64_t>(x) * sizeof(uint32_t));
        const uint8_t* source_row = origin + (static_cast<uint64_t>(row) * source_pitch);
        memcpy(destination_row, source_row, row_bytes);
    }
    return true;
}

bool flush() {
    return ready();
}

bool flush_rect(uint32_t x, uint32_t y, uint32_t width, uint32_t height) {
    // Sin dispositivo: escribir el framebuffer ya es "presentar". Solo validamos.
    return blit_rect(nullptr, 0, x, y, width, height);
}

bool present(const void* pixels, size_t byte_count) {
    if (!ready() || pixels == nullptr || byte_count != g_fb_info.buffer_size) {
        return false;
    }
    if (!blit_rect(pixels, g_fb_info.pitch, 0, 0, g_fb_info.width, g_fb_info.height)) {
        return false;
    }
    retire_synchronous_present(0);
    return true;
}

bool present_region(const void* pixels, uint32_t source_pitch, uint32_t x, uint32_t y, uint32_t width, uint32_t height) {
    if (pixels == nullptr || source_pitch == 0) {
        return false;
    }
    if (!blit_rect(pixels, source_pitch, x, y, width, height)) {
        return false;
    }
    retire_synchronous_present(0);
    return true;
}

bool get_info(savanxp_gpu_info& info) {
    if (!ready()) {
        return false;
    }
    info = g_gpu_info;
    return true;
}

bool get_connector_properties(savanxp_gpu_connector_properties& properties) {
    if (!ready()) {
        return false;
    }
    // Sin cambio de modo (resolucion fija) ni plano de cursor: el compositor cae
    // a su cursor por software. Si advertimos PARTIAL_PRESENT para que use
    // present_surface_batch con rects sucios.
    properties = {
        .flags = SAVANXP_GPU_CONNECTOR_FLAG_PARTIAL_PRESENT | SAVANXP_GPU_CONNECTOR_FLAG_SAFE_MODE,
        .active_scanout_id = 0,
        .preferred_width = g_fb_info.width,
        .preferred_height = g_fb_info.height,
        .batch_capacity = kImportedSurfaceCount,
        .max_dirty_rects = SAVANXP_GPU_SURFACE_PRESENT_BATCH_MAX_RECTS,
    };
    return true;
}

bool set_mode(savanxp_gpu_mode& mode) {
    if (!ready()) {
        return false;
    }
    const uint32_t requested_width = mode.width != 0 ? mode.width : g_fb_info.width;
    const uint32_t requested_height = mode.height != 0 ? mode.height : g_fb_info.height;
    if (mode.bpp != 0 && mode.bpp != 32u) {
        return false;
    }
    // Resolucion fija: solo aceptamos la nativa. Cualquier otra falla (el
    // compositor consulta el modo nativo via get_info y pide ese mismo).
    if (requested_width != g_fb_info.width || requested_height != g_fb_info.height) {
        return false;
    }
    mode = {
        .width = g_fb_info.width,
        .height = g_fb_info.height,
        .pitch = g_fb_info.pitch,
        .bpp = g_fb_info.bpp,
        .buffer_size = g_fb_info.buffer_size,
    };
    return true;
}

ImportedSurface* imported_surface_at(uint32_t surface_id) {
    if (surface_id == 0) {
        return nullptr;
    }
    for (ImportedSurface& surface : g_imported) {
        if (surface.in_use && surface.surface_id == surface_id) {
            return &surface;
        }
    }
    return nullptr;
}

ImportedSurface* allocate_surface_slot() {
    for (uint32_t index = 0; index < kImportedSurfaceCount; ++index) {
        ImportedSurface& surface = g_imported[index];
        if (!surface.in_use) {
            memset(&surface, 0, sizeof(surface));
            surface.in_use = true;
            surface.surface_id = index + 1u;
            return &surface;
        }
    }
    return nullptr;
}

void release_imported_surface(ImportedSurface& surface) {
    if (!surface.in_use) {
        return;
    }
    if (surface.virtual_address != nullptr && surface.page_count != 0) {
        (void)vm::unmap_kernel_pages(surface.virtual_address, surface.page_count);
    }
    if (surface.section != nullptr) {
        object::Header* header = &surface.section->header;
        object::release(header);
    }
    memset(&surface, 0, sizeof(surface));
}

bool normalize_import_info(const savanxp_gpu_surface_import& request, savanxp_fb_info& info) {
    const uint32_t width = request.width != 0 ? request.width : g_fb_info.width;
    const uint32_t height = request.height != 0 ? request.height : g_fb_info.height;
    const uint32_t bpp = request.bpp != 0 ? request.bpp : 32u;
    const uint32_t pitch = request.pitch != 0 ? request.pitch : static_cast<uint32_t>(width * sizeof(uint32_t));
    const uint32_t buffer_size = request.buffer_size != 0 ? request.buffer_size : static_cast<uint32_t>(pitch * height);

    if (width == 0 || height == 0 || bpp != 32u || pitch < (width * sizeof(uint32_t))) {
        return false;
    }
    if (buffer_size < (pitch * height)) {
        return false;
    }
    if (width != g_fb_info.width || height != g_fb_info.height) {
        return false;
    }

    info = {
        .width = width,
        .height = height,
        .pitch = pitch,
        .bpp = bpp,
        .buffer_size = buffer_size,
    };
    return true;
}

bool import_surface(savanxp_gpu_surface_import& request) {
    process::Process* current = process::current();
    if (!ready() || current == nullptr || request.section_handle < 0) {
        return false;
    }
    if (static_cast<uint64_t>(request.section_handle) >= process::kMaxFileHandles) {
        return false;
    }

    process::HandleEntry& entry = current->handles[request.section_handle];
    if (entry.object == nullptr || (entry.granted_access & object::access_query) == 0) {
        return false;
    }
    object::SectionObject* section = object::as_section(entry.object);
    if (section == nullptr || section->physical_pages == nullptr) {
        return false;
    }

    savanxp_fb_info info = {};
    if (!normalize_import_info(request, info)) {
        return false;
    }
    if ((request.pixels_offset % memory::kPageSize) != 0) {
        return false;
    }
    const uint64_t page_count = (static_cast<uint64_t>(info.buffer_size) + memory::kPageSize - 1u) / memory::kPageSize;
    const uint64_t page_offset = static_cast<uint64_t>(request.pixels_offset) / memory::kPageSize;
    if (static_cast<uint64_t>(request.pixels_offset) + info.buffer_size > section->size_bytes ||
        page_offset + page_count > section->page_count) {
        return false;
    }
    uint64_t* backing_pages = section->physical_pages + page_offset;

    ImportedSurface* imported = allocate_surface_slot();
    if (imported == nullptr) {
        return false;
    }

    imported->flags = request.flags;
    imported->page_count = page_count;
    imported->info = info;
    imported->section = section;
    object::retain(&section->header);

    if (!vm::map_kernel_pages(backing_pages, page_count, vm::kPageWrite, &imported->virtual_address)) {
        release_imported_surface(*imported);
        return false;
    }

    request.surface_id = static_cast<int32_t>(imported->surface_id);
    request.width = imported->info.width;
    request.height = imported->info.height;
    request.pitch = imported->info.pitch;
    request.bpp = imported->info.bpp;
    request.buffer_size = imported->info.buffer_size;
    return true;
}

bool release_surface(uint32_t surface_id) {
    ImportedSurface* surface = imported_surface_at(surface_id);
    if (surface == nullptr) {
        return false;
    }
    release_imported_surface(*surface);
    return true;
}

bool present_surface_rect(const ImportedSurface& surface, uint32_t x, uint32_t y, uint32_t width, uint32_t height) {
    if (surface.virtual_address == nullptr) {
        return false;
    }
    const auto* origin = static_cast<const uint8_t*>(surface.virtual_address) +
        (static_cast<uint64_t>(y) * surface.info.pitch) + (static_cast<uint64_t>(x) * sizeof(uint32_t));
    return blit_rect(origin, surface.info.pitch, x, y, width, height);
}

bool present_surface_region(const savanxp_gpu_surface_present& request) {
    ImportedSurface* surface = imported_surface_at(request.surface_id);
    if (surface == nullptr) {
        return false;
    }
    if (request.width == 0 || request.height == 0 ||
        request.x >= surface->info.width || request.y >= surface->info.height ||
        request.width > (surface->info.width - request.x) ||
        request.height > (surface->info.height - request.y)) {
        return false;
    }
    if (!present_surface_rect(*surface, request.x, request.y, request.width, request.height)) {
        return false;
    }
    retire_synchronous_present(0);
    return true;
}

bool present_surface_batch(const savanxp_gpu_surface_present_batch& request) {
    if ((request.flags & ~SAVANXP_GPU_SURFACE_PRESENT_BATCH_FLAG_FULL_SURFACE) != 0 ||
        request.rect_count > SAVANXP_GPU_SURFACE_PRESENT_BATCH_MAX_RECTS) {
        return false;
    }
    ImportedSurface* surface = imported_surface_at(request.surface_id);
    if (surface == nullptr) {
        return false;
    }

    if ((request.flags & SAVANXP_GPU_SURFACE_PRESENT_BATCH_FLAG_FULL_SURFACE) != 0 || request.rect_count == 0) {
        if (!present_surface_rect(*surface, 0, 0, surface->info.width, surface->info.height)) {
            return false;
        }
        retire_synchronous_present(request.present_cookie);
        return true;
    }

    for (uint32_t index = 0; index < request.rect_count; ++index) {
        const savanxp_gpu_dirty_rect& rect = request.rects[index];
        if (rect.width == 0 || rect.height == 0 ||
            rect.x >= surface->info.width || rect.y >= surface->info.height ||
            rect.width > (surface->info.width - rect.x) ||
            rect.height > (surface->info.height - rect.y)) {
            continue;
        }
        if (!present_surface_rect(*surface, rect.x, rect.y, rect.width, rect.height)) {
            return false;
        }
    }
    retire_synchronous_present(request.present_cookie);
    return true;
}

bool get_stats(savanxp_gpu_stats& stats) {
    if (!ready()) {
        return false;
    }
    memset(&stats, 0, sizeof(stats));
    return true;
}

bool get_scanouts(savanxp_gpu_scanout_state& state) {
    if (!ready()) {
        return false;
    }
    memset(&state, 0, sizeof(state));
    state.count = 1;
    state.active_scanout_id = 0;
    state.scanouts[0] = {
        .scanout_id = 0,
        .flags = SAVANXP_GPU_SCANOUT_FLAG_ENABLED | SAVANXP_GPU_SCANOUT_FLAG_ACTIVE |
            SAVANXP_GPU_SCANOUT_FLAG_PRIMARY | SAVANXP_GPU_SCANOUT_FLAG_PREFERRED,
        .native_width = g_fb_info.width,
        .native_height = g_fb_info.height,
        .preferred_width = g_fb_info.width,
        .preferred_height = g_fb_info.height,
        .active_width = g_fb_info.width,
        .active_height = g_fb_info.height,
    };
    return true;
}

bool refresh_scanouts() {
    return ready();
}

bool set_cursor(const savanxp_gpu_cursor_image& /*image*/) {
    // Sin plano de cursor de hardware: el compositor dibuja el cursor por software.
    return false;
}

bool move_cursor(const savanxp_gpu_cursor_position& /*position*/) {
    return false;
}

bool get_present_timeline(savanxp_gpu_present_timeline& timeline) {
    if (!ready()) {
        return false;
    }
    // Todo es sincrono: lo enviado ya esta retirado, nada pendiente.
    timeline = {
        .submitted_sequence = g_last_submitted_present_sequence,
        .retired_sequence = g_last_retired_present_sequence,
        .pending_count = 0,
        .flags = 0,
    };
    return true;
}

bool wait_present(savanxp_gpu_present_wait& request) {
    if (!ready()) {
        return false;
    }
    if (request.target_sequence != 0 &&
        request.target_sequence > g_last_submitted_present_sequence) {
        return false;
    }
    // Presentacion sincrona: cualquier secuencia pedida ya esta retirada.
    request.retired_sequence = g_last_retired_present_sequence;
    request.pending_count = 0;
    request.flags = 0;
    return true;
}

void release_session_resources() {
    for (ImportedSurface& surface : g_imported) {
        release_imported_surface(surface);
    }
}

int create_present_event() {
    return -static_cast<int>(SAVANXP_ENOSYS);
}

int create_scanout_event() {
    return -static_cast<int>(SAVANXP_ENOSYS);
}

const display::Backend kBackend = {
    ready,
    poll,
    framebuffer_info,
    framebuffer_address,
    wait_for_idle,
    flush,
    flush_rect,
    present,
    present_region,
    get_info,
    get_connector_properties,
    set_mode,
    import_surface,
    release_surface,
    present_surface_region,
    present_surface_batch,
    get_stats,
    get_scanouts,
    refresh_scanouts,
    set_cursor,
    move_cursor,
    get_present_timeline,
    wait_present,
    release_session_resources,
    create_present_event,
    create_scanout_event,
};

} // namespace

void initialize(const boot::FramebufferInfo& framebuffer) {
    g_fb_base = framebuffer.address;
    g_fb_info = {
        .width = static_cast<uint32_t>(framebuffer.width),
        .height = static_cast<uint32_t>(framebuffer.height),
        .pitch = static_cast<uint32_t>(framebuffer.pitch),
        .bpp = framebuffer.bpp,
        .buffer_size = static_cast<uint32_t>(framebuffer.pitch * framebuffer.height),
    };
    g_gpu_info = {
        .width = g_fb_info.width,
        .height = g_fb_info.height,
        .pitch = g_fb_info.pitch,
        .bpp = g_fb_info.bpp,
        .buffer_size = g_fb_info.buffer_size,
        .backend = SAVANXP_GPU_BACKEND_FRAMEBUFFER,
        .flags = 0,
    };
    g_next_present_sequence = 1;
    g_last_submitted_present_sequence = 0;
    g_last_retired_present_sequence = 0;
}

const display::Backend& backend() {
    return kBackend;
}

} // namespace fb_gpu
