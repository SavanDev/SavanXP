#include "kernel/fb_gpu.hpp"

#include <stddef.h>
#include <stdint.h>

#include "kernel/console.hpp"
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
savanxp_gpu_stats g_gpu_stats = {};
uint64_t g_next_present_sequence = 1;
uint64_t g_last_submitted_present_sequence = 0;
uint64_t g_last_retired_present_sequence = 0;

// Modo que dejo el firmware. Es el techo de lo que podemos pedir -- ver
// mode_fits_mapping() -- y el modo al que se vuelve.
savanxp_fb_info g_native_info = {};
// El adaptador expone la interfaz VBE de Bochs y podemos cambiar de modo.
bool g_dispi_available = false;

// --- VBE de Bochs (dispi) ---------------------------------------------------
// Par de puertos indice/dato del "Bochs Graphics Adaptor", que implementan
// tanto la VGA estandar de QEMU (-device VGA) como VBoxVGA. Es la unica palanca
// de mode-setting cuando no hay virtio-gpu: el scanout que entrega el firmware
// es lineal pero de resolucion fija.
constexpr uint16_t kDispiIndexPort = 0x01CE;
constexpr uint16_t kDispiDataPort = 0x01CF;

constexpr uint16_t kDispiRegisterId = 0x0;
constexpr uint16_t kDispiRegisterXres = 0x1;
constexpr uint16_t kDispiRegisterYres = 0x2;
constexpr uint16_t kDispiRegisterBpp = 0x3;
constexpr uint16_t kDispiRegisterEnable = 0x4;
constexpr uint16_t kDispiRegisterVirtWidth = 0x6;
constexpr uint16_t kDispiRegisterVirtHeight = 0x7;
constexpr uint16_t kDispiRegisterXOffset = 0x8;
constexpr uint16_t kDispiRegisterYOffset = 0x9;

// Los ID validos van de ID0 a ID5; VirtualBox reporta uno de los bajos y QEMU
// el mas alto, asi que se acepta el rango entero.
constexpr uint16_t kDispiIdMin = 0xB0C0;
constexpr uint16_t kDispiIdMax = 0xB0C5;

constexpr uint16_t kDispiDisabled = 0x00;
constexpr uint16_t kDispiEnabled = 0x01;
constexpr uint16_t kDispiLfbEnabled = 0x40;

inline void out16(uint16_t port, uint16_t value) {
    asm volatile("outw %0, %1" : : "a"(value), "Nd"(port));
}

inline uint16_t in16(uint16_t port) {
    uint16_t value = 0;
    asm volatile("inw %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

void dispi_write(uint16_t index, uint16_t value) {
    out16(kDispiIndexPort, index);
    out16(kDispiDataPort, value);
}

uint16_t dispi_read(uint16_t index) {
    out16(kDispiIndexPort, index);
    return in16(kDispiDataPort);
}

bool detect_dispi() {
    const uint16_t id = dispi_read(kDispiRegisterId);
    return id >= kDispiIdMin && id <= kDispiIdMax;
}

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
    g_gpu_stats.present_enqueued += 1u;
    g_gpu_stats.present_completed += 1u;
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
    uint8_t* destination_origin = destination + (static_cast<uint64_t>(y) * g_fb_info.pitch) +
        (static_cast<uint64_t>(x) * sizeof(uint32_t));

    // Si las filas quedan pegadas de los dos lados -- el rect ocupa el ancho
    // completo y ningun pitch trae padding -- el rectangulo entero es un solo
    // tramo lineal y se copia de una. Es el caso de present() a pantalla
    // completa y del batch con FULL_SURFACE, que si no pagan una llamada por
    // fila para mover memoria que ya venia contigua.
    if (row_bytes == source_pitch && row_bytes == g_fb_info.pitch) {
        memcpy(destination_origin, origin, row_bytes * height);
        return true;
    }

    for (uint32_t row = 0; row < height; ++row) {
        memcpy(
            destination_origin + (static_cast<uint64_t>(row) * g_fb_info.pitch),
            origin + (static_cast<uint64_t>(row) * source_pitch),
            row_bytes);
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
    // Contrato del ioctl (mismo que virtio y console): pixels apunta a la base
    // de una superficie completa y el rect se lee en su offset (x,y), no en la
    // fila 0.
    const auto* origin = static_cast<const uint8_t*>(pixels) +
        (static_cast<uint64_t>(y) * source_pitch) + (static_cast<uint64_t>(x) * sizeof(uint32_t));
    if (!blit_rect(origin, source_pitch, x, y, width, height)) {
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
    // Sin plano de cursor de hardware: el compositor cae a su cursor por
    // software. Anunciamos PARTIAL_PRESENT para que use present_surface_batch
    // con rects sucios, y MUTABLE_MODE_SETTING solo si el adaptador expone
    // dispi -- con el framebuffer pelado la resolucion sigue siendo fija.
    properties = {
        .flags = SAVANXP_GPU_CONNECTOR_FLAG_PARTIAL_PRESENT | SAVANXP_GPU_CONNECTOR_FLAG_SAFE_MODE |
            (g_dispi_available ? SAVANXP_GPU_CONNECTOR_FLAG_MUTABLE_MODE_SETTING : 0u),
        .active_scanout_id = 0,
        // El modo preferido es el nativo, no el que este puesto ahora.
        .preferred_width = g_native_info.width,
        .preferred_height = g_native_info.height,
        .batch_capacity = kImportedSurfaceCount,
        .max_dirty_rects = SAVANXP_GPU_SURFACE_PRESENT_BATCH_MAX_RECTS,
    };
    return true;
}

bool has_live_imports() {
    for (const ImportedSurface& surface : g_imported) {
        if (surface.in_use) {
            return true;
        }
    }
    return false;
}

// El unico mapeo de scanout que tenemos es el que armo el firmware, del tamano
// del modo nativo. Un modo que necesite mas bytes que ese escribiria fuera del
// mapeo, asi que el techo es el nativo. Alcanza para lo que interesa (bajar a
// una resolucion mas chica); subir de ahi -- o el doble buffer por panning --
// pide primero mapear la apertura de VRAM entera.
bool mode_fits_mapping(uint32_t pitch, uint32_t height) {
    if (pitch == 0 || height == 0) {
        return false;
    }
    const uint64_t bytes = static_cast<uint64_t>(pitch) * height;
    return bytes != 0 && bytes <= g_native_info.buffer_size;
}

// Programa el modo por dispi y devuelve en `result` la geometria que quedo de
// verdad. Se relee del dispositivo en vez de confiar en lo pedido: la
// implementacion puede redondear el ancho virtual (o sea el pitch) o rechazar
// la resolucion sin avisar.
bool apply_dispi_mode(uint32_t width, uint32_t height, savanxp_fb_info& result) {
    dispi_write(kDispiRegisterEnable, kDispiDisabled);
    dispi_write(kDispiRegisterXres, static_cast<uint16_t>(width));
    dispi_write(kDispiRegisterYres, static_cast<uint16_t>(height));
    dispi_write(kDispiRegisterBpp, 32u);
    dispi_write(kDispiRegisterVirtWidth, static_cast<uint16_t>(width));
    dispi_write(kDispiRegisterVirtHeight, static_cast<uint16_t>(height));
    dispi_write(kDispiRegisterXOffset, 0);
    dispi_write(kDispiRegisterYOffset, 0);
    dispi_write(kDispiRegisterEnable, kDispiEnabled | kDispiLfbEnabled);

    const uint32_t actual_width = dispi_read(kDispiRegisterXres);
    const uint32_t actual_height = dispi_read(kDispiRegisterYres);
    const uint32_t actual_bpp = dispi_read(kDispiRegisterBpp);
    const uint32_t virtual_width = dispi_read(kDispiRegisterVirtWidth);

    if (actual_width != width || actual_height != height || actual_bpp != 32u) {
        return false;
    }
    // El ancho virtual manda sobre el pitch, y nunca puede ser menor al visible.
    const uint32_t pitch_pixels = virtual_width >= actual_width ? virtual_width : actual_width;
    const uint32_t pitch = static_cast<uint32_t>(pitch_pixels * sizeof(uint32_t));
    if (!mode_fits_mapping(pitch, actual_height)) {
        return false;
    }

    result = {
        .width = actual_width,
        .height = actual_height,
        .pitch = pitch,
        .bpp = 32u,
        .buffer_size = pitch * actual_height,
    };
    return true;
}

void publish_mode(const savanxp_fb_info& info) {
    g_fb_info = info;
    g_gpu_info.width = info.width;
    g_gpu_info.height = info.height;
    g_gpu_info.pitch = info.pitch;
    g_gpu_info.bpp = info.bpp;
    g_gpu_info.buffer_size = info.buffer_size;
    // Misma juntura que usa virtio-gpu: la consola cachea su propia geometria y
    // con el pitch viejo dibujaria torcido (importa en un panic despues de un
    // cambio de modo, que es cuando la consola vuelve a aparecer).
    console::set_external_framebuffer(g_fb_base, g_fb_info);
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

    if (requested_width != g_fb_info.width || requested_height != g_fb_info.height) {
        // Sin dispi la resolucion es la que dejo el firmware y punto.
        if (!g_dispi_available) {
            return false;
        }
        // Mismo contrato que virtio: cambiar de modo rehace el scanout, asi que
        // no puede haber superficies importadas apuntando al anterior.
        if (has_live_imports()) {
            return false;
        }

        savanxp_fb_info applied = {};
        if (!apply_dispi_mode(requested_width, requested_height, applied)) {
            // Reponer lo que habia: quedarse en un modo a medio programar deja
            // la pantalla inutilizable y sin forma de volver.
            savanxp_fb_info restored = {};
            if (apply_dispi_mode(g_fb_info.width, g_fb_info.height, restored)) {
                publish_mode(restored);
            }
            return false;
        }
        publish_mode(applied);
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
    stats = g_gpu_stats;
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
        .native_width = g_native_info.width,
        .native_height = g_native_info.height,
        .preferred_width = g_native_info.width,
        .preferred_height = g_native_info.height,
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
    // El modo es propiedad de la sesion grafica: si el cliente la suelta (o se
    // muere y el kernel la reclama por el) la pantalla vuelve a la resolucion
    // del firmware, en vez de quedar en el modo bajo de la app que se fue.
    if (g_dispi_available && ready() &&
        (g_fb_info.width != g_native_info.width || g_fb_info.height != g_native_info.height)) {
        savanxp_fb_info restored = {};
        if (apply_dispi_mode(g_native_info.width, g_native_info.height, restored)) {
            publish_mode(restored);
        }
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
    // El modo del firmware es el techo y el punto de retorno de cualquier
    // cambio posterior, asi que se guarda antes de tocar nada.
    g_native_info = g_fb_info;
    g_dispi_available = detect_dispi();
    console::printf("fb_gpu: %ux%u nativo, mode-setting %s\n",
        g_native_info.width,
        g_native_info.height,
        g_dispi_available ? "por VBE dispi" : "no disponible");

    memset(&g_gpu_stats, 0, sizeof(g_gpu_stats));
    g_next_present_sequence = 1;
    g_last_submitted_present_sequence = 0;
    g_last_retired_present_sequence = 0;
}

const display::Backend& backend() {
    return kBackend;
}

namespace {
// Prioridad baja: es el fallback, solo gana si no hubo nada paravirtual.
constexpr int kDriverPriority = 10;

bool driver_probe(const boot::FramebufferInfo& framebuffer) {
    initialize(framebuffer);
    // ready() ya exige scanout lineal de 32bpp con tamano no nulo, asi que en
    // una maquina sin framebuffer utilizable este driver declina en vez de
    // atarse igual y dejar un backend muerto.
    return ready();
}

const display::Driver kDriver = {
    "framebuffer",
    kDriverPriority,
    &driver_probe,
    &backend,
};
} // namespace

const display::Driver& driver() { return kDriver; }

} // namespace fb_gpu
