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
    // Bit 63 de las entradas de paging x86-64. Con EFER.NXE activo vuelve
    // NX un permiso de solo paginas, no un flag de CPU completo: el hardware
    // haceUltimo el control en la TLB.
    kPageNoExecute = 1ULL << 63,
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
// Cambia el punto de partida de las vistas de seccion para este espacio. El
// rango es una ventana de 16 GiB reservada para mappings compartidos; el kernel
// lo siembra con entropia de CPU para que dos procesos no-usarios no reciban
// siempre la misma secuencia de direcciones.
void randomize_section_base(VmSpace& space, uint64_t seed);
bool map_page(VmSpace& space, uint64_t virtual_address, uint64_t physical_address, uint64_t flags);
bool unmap_page(VmSpace& space, uint64_t virtual_address, uint64_t* physical_address);
bool clone_address_space(const VmSpace& source, VmSpace& destination);
// Bytes de usuario mapeados en este espacio ahora mismo. Es lo que el
// administrador de tareas muestra como uso de memoria de un proceso: paginas
// presentes con el bit de usuario, contadas recorriendo las tablas.
uint64_t resident_user_bytes(const VmSpace& space);
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
// Suma permisos a una pagina de usuario ya mapeada; false si no lo esta. La
// usa el cargador de ELF cuando dos PT_LOAD comparten una pagina.
bool add_user_page_flags(VmSpace& space, uint64_t address, uint64_t flags);
bool ensure_user_stack_page(VmSpace& space, uint64_t address);
bool is_user_range_accessible(const VmSpace& space, uint64_t virtual_address, size_t size, bool require_write);

/* --- TLB con varios cores (docs/SMP_ROADMAP.md, fase 3) ---------------------
 *
 * La mitad de kernel del espacio de direcciones la comparten todos los cores:
 * cuando un core desmapea una pagina de kernel, el invlpg solo limpia SU TLB, y
 * los demas conservan la traduccion vieja.
 *
 * Se resuelve sin IPIs, apoyado en el lock grande del kernel. Todo acceso a
 * memoria del kernel que pueda desmapearse ocurre con el lock tomado, asi que
 * alcanza con que cada core se ponga al dia ANTES de tocar nada: desmapear sube
 * una generacion global, y smp::lock_kernel() llama a sync_kernel_tlb(), que
 * vacia la TLB de este core si su generacion quedo atras. Un IPI con espera de
 * acuse, en cambio, no funciona mientras haya un solo lock: el que desmapea lo
 * tiene, y los cores que esperan ese lock giran con IF=0 y nunca atienden el
 * IPI.
 *
 * La mitad de usuario no necesita nada de esto, y conviene saber por que para
 * no romperlo: un espacio de usuario esta cargado en un solo core a la vez (no
 * hay threads), cambiar de proceso recarga CR3 -- lo que vacia las traducciones
 * no globales --, y solo se desmapea el espacio del proceso que corre en este
 * core o el de uno que no corre en ninguno (un proceso muerto; matar a uno que
 * corre en otro core se difiere). Cualquier camino nuevo que toque el espacio de
 * un proceso que puede estar corriendo en otro core rompe esa regla.
 *
 * La regla del lado del kernel es la de arriba: si algun dia se toca memoria del
 * kernel desmapeable sin el lock (fase 4), esto deja de alcanzar. */

// Vacia la TLB de este core si una pagina del kernel se desmapeo desde la ultima
// vez. La llama smp::lock_kernel() apenas toma el lock.
void sync_kernel_tlb();

// Apunta una pagina de kernel ya mapeada a otra pagina fisica, en la misma
// direccion virtual. Es exactamente el caso que deja una traduccion vieja en los
// otros cores, y hoy solo lo usa el autotest de arranque que lo comprueba
// (smp::selftest_tlb): el resto del kernel nunca reusa una VA.
bool retarget_kernel_page(void* virtual_address, uint64_t physical_address, uint64_t flags);

} // namespace vm
