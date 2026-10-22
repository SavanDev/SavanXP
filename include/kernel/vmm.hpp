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

/* Lugares de vista POR PROCESO.
 *
 * El arreglo es en linea dentro de VmSpace, que a su vez es miembro de Process,
 * asi que cada vista cuesta sizeof(SectionView) = 32 bytes en los kMaxProcesses
 * procesos de la tabla: 64 vistas x 32 bytes x 64 procesos son 128 KiB de kernel,
 * y subir mas no compra nada.
 *
 * El presupuesto que hay que cubrir, con el cargador de librerias en el medio:
 *   - hasta SX_ARENA_CAPACITY (8) arenas de heap por proceso
 *   - hasta 3 vistas por libreria (texto/rodata respaldado por el archivo,
 *     datos privados, bss): 6 librerias son 18
 *   - mmap anonimos explicitos y superficies de GPU, un puñado
 * Eso da ~40 en el peor caso realista, y 32 era el numero que lo rozaba. */
constexpr size_t kMaxSectionViews = 64;

enum PageFlags : uint64_t {
    kPagePresent = 1ULL << 0,
    kPageWrite = 1ULL << 1,
    kPageUser = 1ULL << 2,
    kPageWriteThrough = 1ULL << 3,
    kPageCacheDisable = 1ULL << 4,
    // Bit 63 de las entradas de paging x86-64. Con EFER.NXE activo vuelve
    // NX un permiso de solo paginas, no un flag de CPU completo: la CPU
    // aplica el permiso durante la traduccion de la direccion.
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
uint32_t last_view_failure();
bool create_address_space(VmSpace& space);
void destroy_address_space(VmSpace& space);
// Cambia el punto de partida de las vistas de seccion para este espacio. El
// rango es una ventana de 16 GiB reservada para mappings compartidos; el kernel
// lo siembra con entropia de CPU para que dos procesos no-usarios no reciban
// siempre la misma secuencia de direcciones.
void randomize_section_base(VmSpace& space, uint64_t seed);
bool map_page(VmSpace& space, uint64_t virtual_address, uint64_t physical_address, uint64_t flags);
// Only for pages of section views: the other cores holding the space are
// asked to flush when the view is released (see "TLB coherence" below).
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

/* --- TLB coherence across cores ----------------------------------------------
 *
 * KERNEL HALF (docs/kernel/SMP_ROADMAP.md, phase 3). Every core shares it, and
 * an invlpg only clears the TLB of the core that runs it. It is solved without
 * IPIs, on top of the big kernel lock: every access to kernel memory that can
 * be unmapped happens with the lock held, so a core only has to catch up
 * before it touches anything. Unmapping bumps a global generation, and
 * smp::lock_kernel() calls sync_kernel_tlb(), which flushes this core's TLB
 * when its generation is behind. An IPI waiting for an acknowledgement would
 * deadlock instead: the unmapper holds the lock, and the cores it waits on
 * spin for that lock with IF=0 and never take the IPI. If unmappable kernel
 * memory is ever touched without the lock (SMP phase 4), this stops being
 * enough.
 *
 * USER HALF (docs/kernel/THREADS_ROADMAP.md, phase 2). Threads of one process
 * run on several cores at once, so one address space can be loaded on several
 * cores. Every CR3 load of a thread's space goes through load_address_space(),
 * which records which space each core holds. Unmapping a user page from a
 * space that other cores hold marks those cores stale and sends each one a
 * reschedule IPI, without waiting: any entry to the kernel takes the lock, and
 * smp::lock_kernel() calls sync_user_tlb(), which reloads CR3 when this core
 * was marked. A core in user mode takes the IPI; one spinning for the lock or
 * asleep in the kernel catches up when it gets the lock, before it can touch
 * user memory. (sx's copy to or from another process's memory loads that
 * space for one memcpy and reloads the previous one, all within one hold of
 * the lock, so no unmap can fall in between and nothing is recorded.)
 *
 * Since nobody waits, the physical pages are freed late. The unmapped view's
 * reference to its section goes on a deferred list with the cores that must
 * flush, and is released by reclaim_deferred_views() only once they all
 * have. Until then a stale translation on another core still points at
 * memory the section owns, never at a page handed to someone else. A space
 * loaded on no other core -- every single-threaded process -- releases right
 * away, as before.
 *
 * Mapping needs none of this: x86 never caches a translation that is not
 * present. Nor does destroying a space: its process runs on no core by then
 * (terminate_process defers until it does not), which destroy_address_space
 * asserts. */

// Flushes this core's TLB if a kernel page was unmapped since its last sync.
// smp::lock_kernel() calls it as soon as it holds the lock.
void sync_kernel_tlb();

// Loads `space` into CR3 on this core unless it already is, and records it.
// The only way a thread's address space is loaded (process::switch_to_thread).
void load_address_space(const VmSpace& space);

// Reloads CR3 if another core unmapped user pages of the space loaded here.
// smp::lock_kernel() calls it right after sync_kernel_tlb().
void sync_user_tlb();

// Releases the section references of unmapped views whose pages no core can
// still reach through its TLB. The BSP's timer tick calls it; unmapping a view
// does too.
void reclaim_deferred_views();

// Points an already mapped kernel page at another physical page, at the same
// virtual address. That is exactly the case that leaves a stale translation on
// the other cores, and today only the boot self-test that checks it uses it
// (smp::selftest_tlb): the rest of the kernel never reuses a VA.
bool retarget_kernel_page(void* virtual_address, uint64_t physical_address, uint64_t flags);

} // namespace vm
