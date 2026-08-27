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

// Modo que dejo el firmware: el modo al que se vuelve.
savanxp_fb_info g_native_info = {};
// Bytes mapeados detras del scanout (la apertura de VRAM, no solo el modo
// nativo). Es el techo real de mode_fits_mapping.
uint64_t g_mapped_bytes = 0;

// --- Doble buffer por panning -----------------------------------------------
// Con alto virtual = 2x el visible, la VRAM guarda dos frames y el registro
// Y_OFFSET de dispi elige cual se muestra. Se compone sobre el que NO esta a la
// vista y despues se flipea, asi el host nunca escanea un buffer a medio
// escribir: eso es lo que saca el tearing.
//
// Solo se activa si los dos buffers entran en lo que hay MAPEADO. Hoy eso pasa
// en el modo bajo de fullscreen (640x400 -> 2 MiB de los 4 MiB mapeados) y no
// en el nativo, que pediria mapear la apertura de VRAM entera.
//
// Con dos buffers, el que se va a componer quedo con el contenido de hace DOS
// frames, asi que un present parcial arrastraria pixeles viejos. En vez de
// llevar la cuenta del dano de frames anteriores para reaplicarlo, se copia la
// superficie ENTERA en cada present. Es deliberado: el modo bajo existe para
// apps a pantalla completa, que repintan todo y ya mandaban FULL_SURFACE, asi
// que en el caso real no cuesta nada -- y evita toda una maquinaria de
// bookkeeping que solo se puede verificar mirando la pantalla.
bool g_double_buffered = false;
// Offset en bytes del buffer que se esta mostrando.
uint64_t g_front_offset_bytes = 0;
// Offset en bytes del buffer sobre el que se compone. Igual al frente cuando no
// hay doble buffer, que es lo que deja el camino de un solo buffer intacto.
uint64_t g_back_offset_bytes = 0;
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
constexpr uint16_t kDispiRegisterVideoMemory64K = 0xa;

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

// El firmware mapea solo el modo visible, pero detras hay toda la apertura de
// VRAM: dispi dice cuanta hay. Mapear el resto es lo que habilita el doble
// buffer en resoluciones grandes, donde dos buffers no entran en lo que dejo
// Limine.
//
// Tipo de memoria: se prefiere write-combining, que es lo que corresponde a un
// framebuffer -- escrituras en rafaga que nadie relee. El indice de IA32_PAT
// configurado asi se busca en runtime en vez de asumirlo. Si no hay ninguno se
// copia el tipo que el firmware le puso al scanout, que es lo seguro.
//
// Queda un alias: los primeros bytes de la apertura estan mapeados dos veces,
// por el firmware y por nosotros, y si elegimos WC los dos tipos difieren.
// Despues del cambio de vista no se vuelve a tocar la del firmware, y la
// consola se muda con nosotros, asi que en la practica hay un solo escritor.
// Quedo la apertura mapeada write-combining (lo deseable) o con el tipo que le
// puso el firmware al scanout (el repliegue).
bool g_aperture_write_combining = false;

bool remap_vram_aperture() {
    const uint64_t vram_blocks = dispi_read(kDispiRegisterVideoMemory64K);
    const uint64_t aperture_bytes = vram_blocks * 64u * 1024u;
    if (vram_blocks == 0 || aperture_bytes <= g_mapped_bytes) {
        return false;
    }

    void* const firmware_view = g_fb_base;
    const uint64_t firmware_virtual = reinterpret_cast<uint64_t>(firmware_view);
    if (firmware_virtual <= vm::hhdm_offset()) {
        return false;
    }

    uint64_t cache_flags = 0;
    const bool write_combining = vm::write_combining_page_flags(cache_flags);
    if (!write_combining && !vm::kernel_page_cache_flags(firmware_virtual, cache_flags)) {
        return false;
    }

    void* aperture = nullptr;
    if (!vm::map_kernel_device_memory(
            firmware_virtual - vm::hhdm_offset(), aperture_bytes, cache_flags, &aperture) ||
        aperture == nullptr) {
        return false;
    }

    // Confirmar que la vista nueva es de verdad la misma memoria antes de
    // confiar en ella: escribir por una y leer por la otra. Si la direccion
    // fisica que dedujimos no era la del scanout, no coinciden. El sfence esta
    // porque con write-combining la escritura puede quedar en un buffer y no
    // verse todavia desde el otro mapeo.
    auto* firmware_pixels = static_cast<volatile uint32_t*>(firmware_view);
    auto* aperture_pixels = static_cast<volatile uint32_t*>(aperture);
    const uint32_t saved = firmware_pixels[0];
    const uint32_t probe = saved ^ 0xA5A5A5A5u;
    aperture_pixels[0] = probe;
    asm volatile("sfence" ::: "memory");
    const bool aliases_same_memory = firmware_pixels[0] == probe;
    firmware_pixels[0] = saved;
    asm volatile("sfence" ::: "memory");

    if (!aliases_same_memory) {
        (void)vm::unmap_kernel_pages(
            aperture, (aperture_bytes + memory::kPageSize - 1u) / memory::kPageSize);
        return false;
    }

    g_fb_base = aperture;
    g_mapped_bytes = aperture_bytes;
    g_aperture_write_combining = write_combining;
    return true;
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
bool blit_rect(uint64_t destination_offset, const void* source, uint32_t source_pitch, uint32_t x, uint32_t y, uint32_t width, uint32_t height) {
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

    auto* destination = static_cast<uint8_t*>(g_fb_base) + destination_offset;
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

// Muestra el buffer recien compuesto y pasa a componer sobre el otro. No-op
// sin doble buffer. El flip es una sola escritura de registro, asi que el
// cambio de buffer visible es atomico desde el punto de vista del que escanea.
void flip_back_buffer() {
    if (!g_double_buffered || g_fb_info.pitch == 0) {
        return;
    }
    const uint32_t back_line = static_cast<uint32_t>(g_back_offset_bytes / g_fb_info.pitch);
    dispi_write(kDispiRegisterYOffset, static_cast<uint16_t>(back_line));

    const uint64_t shown = g_back_offset_bytes;
    g_back_offset_bytes = g_front_offset_bytes;
    g_front_offset_bytes = shown;
}

bool flush() {
    return ready();
}

bool flush_rect(uint32_t x, uint32_t y, uint32_t width, uint32_t height) {
    // Sin dispositivo: escribir el framebuffer ya es "presentar". Solo validamos.
    return blit_rect(g_front_offset_bytes, nullptr, 0, x, y, width, height);
}

bool present(const void* pixels, size_t byte_count) {
    if (!ready() || pixels == nullptr || byte_count != g_fb_info.buffer_size) {
        return false;
    }
    // Escribe el buffer entero, asi que puede componer sobre el de atras y
    // flipear como el camino del compositor: queda sin tearing tambien.
    if (!blit_rect(g_back_offset_bytes, pixels, g_fb_info.pitch, 0, 0, g_fb_info.width, g_fb_info.height)) {
        return false;
    }
    flip_back_buffer();
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
    if (!blit_rect(g_front_offset_bytes, origin, source_pitch, x, y, width, height)) {
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

// Techo de cualquier modo: los bytes que el mapa de memoria dice que hay
// mapeados detras del scanout. Escribir mas alla de eso saldria del mapeo.
bool mode_fits_mapping(uint32_t pitch, uint64_t height) {
    if (pitch == 0 || height == 0) {
        return false;
    }
    const uint64_t bytes = static_cast<uint64_t>(pitch) * height;
    return bytes != 0 && bytes <= g_mapped_bytes;
}

// Programa el modo por dispi y devuelve en `result` la geometria que quedo de
// verdad. Se relee del dispositivo en vez de confiar en lo pedido: la
// implementacion puede redondear el ancho virtual (o sea el pitch) o rechazar
// la resolucion sin avisar.
bool apply_dispi_mode(uint32_t width, uint32_t height, savanxp_fb_info& result) {
    // Se pide alto virtual doble de entrada: si la VRAM no da, la propia
    // implementacion lo recorta y el readback de abajo lo delata.
    dispi_write(kDispiRegisterEnable, kDispiDisabled);
    dispi_write(kDispiRegisterXres, static_cast<uint16_t>(width));
    dispi_write(kDispiRegisterYres, static_cast<uint16_t>(height));
    dispi_write(kDispiRegisterBpp, 32u);
    dispi_write(kDispiRegisterVirtWidth, static_cast<uint16_t>(width));
    dispi_write(kDispiRegisterVirtHeight, static_cast<uint16_t>(height * 2u));
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

    // Doble buffer: hacen falta las dos condiciones. Que el dispositivo pueda
    // escanear desde la segunda mitad (alto virtual), y que nosotros podamos
    // ESCRIBIRLA, o sea que las dos entren en lo mapeado -- que es la que hoy
    // falla en el modo nativo.
    const uint32_t virtual_height = dispi_read(kDispiRegisterVirtHeight);
    g_front_offset_bytes = 0;
    g_back_offset_bytes = 0;
    g_double_buffered =
        virtual_height >= (actual_height * 2u) &&
        mode_fits_mapping(pitch, static_cast<uint64_t>(actual_height) * 2u);

    if (g_double_buffered) {
        // Probar el panning de verdad antes de confiar en el. Un dispositivo
        // que acepte el alto virtual pero ignore Y_OFFSET dejaria el flip sin
        // efecto y la pantalla congelada en un buffer, que es peor que el
        // tearing. El parpadeo de mostrar un instante la mitad de abajo cae
        // dentro del cambio de modo, con la pantalla rearmandose igual.
        dispi_write(kDispiRegisterYOffset, static_cast<uint16_t>(actual_height));
        const bool panning_works = dispi_read(kDispiRegisterYOffset) == actual_height;
        dispi_write(kDispiRegisterYOffset, 0);
        g_double_buffered = panning_works;
    }

    if (g_double_buffered) {
        g_back_offset_bytes = result.buffer_size;
    }
    return true;
}

void publish_mode(const savanxp_fb_info& info) {
    console::printf("fb_gpu: modo %ux%u pitch=%u, doble buffer %s\n",
        info.width, info.height, info.pitch,
        g_double_buffered ? "si" : "no (no entra en lo mapeado)");
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

bool present_surface_rect(
    uint64_t destination_offset,
    const ImportedSurface& surface,
    uint32_t x,
    uint32_t y,
    uint32_t width,
    uint32_t height) {
    if (surface.virtual_address == nullptr) {
        return false;
    }
    const auto* origin = static_cast<const uint8_t*>(surface.virtual_address) +
        (static_cast<uint64_t>(y) * surface.info.pitch) + (static_cast<uint64_t>(x) * sizeof(uint32_t));
    return blit_rect(destination_offset, origin, surface.info.pitch, x, y, width, height);
}

// Compone un frame y lo muestra. Que camino toma lo decide el present, no el
// modo:
//
// - Superficie completa: se compone sobre el buffer que NO se ve y se flipea.
//   Es el camino sin tearing, y el que usan las apps a pantalla completa.
// - Dano parcial: se escribe directo sobre el buffer visible, sin flipear. Con
//   doble buffer el otro buffer esta dos frames atrasado, asi que para poder
//   flipear habria que copiar la superficie ENTERA por cada rect sucio -- 4 MiB
//   para mover el cursor. Ese precio es mucho peor que el tearing que evita, y
//   lo pagaria el escritorio en cada frame.
bool present_surface_damage(
    const ImportedSurface& surface,
    const savanxp_gpu_dirty_rect* rects,
    uint32_t rect_count) {
    if (rects == nullptr || rect_count == 0) {
        if (!present_surface_rect(
                g_back_offset_bytes, surface, 0, 0, surface.info.width, surface.info.height)) {
            return false;
        }
        flip_back_buffer();
        return true;
    }

    for (uint32_t index = 0; index < rect_count; ++index) {
        const savanxp_gpu_dirty_rect& rect = rects[index];
        if (rect.width == 0 || rect.height == 0 ||
            rect.x >= surface.info.width || rect.y >= surface.info.height ||
            rect.width > (surface.info.width - rect.x) ||
            rect.height > (surface.info.height - rect.y)) {
            continue;
        }
        if (!present_surface_rect(
                g_front_offset_bytes, surface, rect.x, rect.y, rect.width, rect.height)) {
            return false;
        }
    }
    return true;
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
    const savanxp_gpu_dirty_rect rect = {
        .x = request.x,
        .y = request.y,
        .width = request.width,
        .height = request.height,
    };
    if (!present_surface_damage(*surface, &rect, 1)) {
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

    const bool full_surface =
        (request.flags & SAVANXP_GPU_SURFACE_PRESENT_BATCH_FLAG_FULL_SURFACE) != 0 ||
        request.rect_count == 0;
    if (!present_surface_damage(
            *surface,
            full_surface ? nullptr : request.rects,
            full_surface ? 0u : request.rect_count)) {
        return false;
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
    // Volver a mostrar el buffer de abajo y apagar el doble buffer: fuera de la
    // sesion grafica el que dibuja es la consola, que escribe directo al inicio
    // del scanout y no sabe nada de paginas alternas.
    if (g_double_buffered) {
        dispi_write(kDispiRegisterYOffset, 0);
        g_double_buffered = false;
        g_front_offset_bytes = 0;
        g_back_offset_bytes = 0;
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
    g_mapped_bytes = framebuffer.mapped_bytes >= g_fb_info.buffer_size
        ? framebuffer.mapped_bytes
        : g_fb_info.buffer_size;
    g_dispi_available = detect_dispi();
    if (g_dispi_available && remap_vram_aperture()) {
        // Solo con dispi tiene sentido: es quien reporta el tamano de la VRAM y
        // el unico camino que puede usar lo que hay de mas. Si el scanout se
        // mudo a la vista nueva, la consola tiene que seguirlo o dibujaria en
        // el mapeo viejo.
        console::set_external_framebuffer(g_fb_base, g_fb_info);
    }
    console::printf("fb_gpu: %ux%u nativo, %u KiB mapeados (%s), mode-setting %s\n",
        g_native_info.width,
        g_native_info.height,
        static_cast<uint32_t>(g_mapped_bytes / 1024u),
        g_aperture_write_combining ? "write-combining" : "tipo del firmware",
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
