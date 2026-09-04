#pragma once

#include <stddef.h>
#include <stdint.h>

#include "boot/boot_info.hpp"

namespace object {
struct SectionObject;
}

namespace vm {

constexpr uint64_t kUserBase = 0x0000000000400000ULL;
constexpr uint64_t kUserStackTop = 0x0000007000000000ULL;

/* Region del stack de usuario.
 *
 * kUserStackPages es lo RESERVADO, no lo mapeado: al arrancar el proceso solo
 * existen kUserStackInitialPages y el resto aparece por demanda, cuando el
 * fault handler ve un acceso adentro de la region (process::grow_user_stack).
 * Asi un tope de 1 MiB no le cuesta 1 MiB residente a cada proceso, que sobre
 * ~133 MiB utiles seria el gasto mas grande de todos.
 *
 * Debajo de todo queda una pagina de guarda que NUNCA se mapea: sin ella un
 * desborde de stack no fallaba, se comia lo que hubiera abajo en silencio.
 * Con ella el acceso cae fuera de la region, no lo atiende grow_user_stack y
 * el proceso muere con un #PF, que es un sintoma que se puede leer. */
constexpr uint64_t kPageSizeBytes = 4096;
constexpr uint64_t kUserStackPages = 256;
constexpr uint64_t kUserStackInitialPages = 8;
constexpr uint64_t kUserStackGuardPages = 1;
constexpr uint64_t kUserStackBottom = kUserStackTop - (kUserStackPages * kPageSizeBytes);
constexpr uint64_t kUserStackGuardBottom =
    kUserStackBottom - (kUserStackGuardPages * kPageSizeBytes);

constexpr uint64_t kSectionViewBase = 0x0000001000000000ULL;
constexpr size_t kMaxSectionViews = 32;

enum PageFlags : uint64_t {
    kPagePresent = 1ULL << 0,
    kPageWrite = 1ULL << 1,
    kPageUser = 1ULL << 2,
    kPageWriteThrough = 1ULL << 3,
    kPageCacheDisable = 1ULL << 4,
    // Bit PAT. SOLO valido en la entrada de ultimo nivel: en un PDE/PDPTE el
    // mismo bit es PS y crearia una pagina grande. map_kernel_page lo aplica
    // unicamente a la hoja, que es lo que lo hace seguro de pasar aca.
    kPagePat = 1ULL << 7,
};

// Los tres bits que eligen el tipo de memoria (indice en IA32_PAT).
constexpr uint64_t kPageCacheMask = kPageWriteThrough | kPageCacheDisable | kPagePat;

struct VmSpace {
    struct SectionView {
        uint64_t base_address;
        uint64_t size_bytes;
        object::SectionObject* section;
        uint32_t access_mask;
        uint8_t share_on_fork;
        uint8_t reserved0;
        uint16_t reserved1;
    };

    uint64_t pml4_physical;
    uint64_t* pml4_virtual;
    uint64_t next_section_base;
    SectionView section_views[kMaxSectionViews];
};

void initialize(const boot::BootInfo& boot_info);
bool ready();
bool create_address_space(VmSpace& space);
void destroy_address_space(VmSpace& space);
bool map_page(VmSpace& space, uint64_t virtual_address, uint64_t physical_address, uint64_t flags);
bool unmap_page(VmSpace& space, uint64_t virtual_address, uint64_t* physical_address);
bool clone_address_space(const VmSpace& source, VmSpace& destination);
bool map_section_view(VmSpace& space, object::SectionObject& section, uint32_t access_mask, uint64_t& base_address, bool share_on_fork);
bool unmap_section_view(VmSpace& space, uint64_t base_address);
bool map_kernel_pages(const uint64_t* physical_pages, uint64_t page_count, uint64_t flags, void** virtual_base);
bool unmap_kernel_pages(void* virtual_base, uint64_t page_count);
bool map_kernel_mmio(uint64_t physical_base, size_t size, uint64_t flags, void** virtual_base);
// Como map_kernel_mmio pero sin forzar cache-disable: el que llama elige el
// tipo de memoria via los bits de kPageCacheMask. Para un framebuffer eso
// importa -- mapearlo UC lo vuelve inutilizablemente lento.
bool map_kernel_device_memory(uint64_t physical_base, size_t size, uint64_t flags, void** virtual_base);
// Bits de cacheo (kPageCacheMask) de un mapeo de kernel que ya existe. Sirve
// para mapear mas de un dispositivo exactamente como lo dejo el firmware, sin
// tener que interpretar el layout de IA32_PAT que este haya elegido.
bool kernel_page_cache_flags(uint64_t virtual_address, uint64_t& flags);
// Bits de pagina que seleccionan write-combining, buscando en IA32_PAT que
// indice quedo configurado asi. false si ninguno lo esta.
bool write_combining_page_flags(uint64_t& flags);
uint64_t current_pml4();
uint64_t hhdm_offset();
uint64_t* physical_to_virtual(uint64_t physical_address);
// Materializa la pagina de la region del stack que contiene `address`. Devuelve
// true si quedo mapeada (o ya lo estaba), false si la direccion cae afuera de la
// region -- por ejemplo en la pagina de guarda -- o no habia memoria.
bool ensure_user_stack_page(VmSpace& space, uint64_t address);
bool is_user_range_accessible(const VmSpace& space, uint64_t virtual_address, size_t size, bool require_write);

} // namespace vm
