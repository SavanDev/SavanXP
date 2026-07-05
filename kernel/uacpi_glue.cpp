// uacpi_glue.cpp — Capa de host (uacpi_kernel_*) que uACPI necesita del kernel.
//
// Implementa la kernel-api de uACPI mapeandola a los subsistemas de SavanXP
// (heap, vmm, pci, timer, ioapic). uACPI vive vendorizado en vendor/uacpi/.
//
// Supuestos validos HOY en SavanXP (si cambian, revisar los bloques marcados):
//   * Monocore: no hay bring-up de CPUs secundarias -> sincronizacion trivial.
//   * Kernel no-preemptivo: el work de GPE/Notify puede correr sincrono (por
//     ahora) sin romper reentrancia grave.

extern "C" {
#include "uacpi/kernel_api.h"
#include "uacpi/namespace.h"
#include "uacpi/status.h"
#include "uacpi/uacpi.h"
#include "uacpi/utilities.h"
#include "uacpi/platform/arch_helpers.h"
}

#include <stddef.h>
#include <stdint.h>

#include "kernel/console.hpp"
#include "kernel/cpu.hpp"
#include "kernel/heap.hpp"
#include "kernel/ioapic.hpp"
#include "kernel/pci.hpp"
#include "kernel/timer.hpp"
#include "kernel/vmm.hpp"

namespace {

// --- Port I/O (mismo patron que acpi.cpp; falta out32/in32 alla) ---
inline void out8(uint16_t port, uint8_t v)  { asm volatile("outb %0,%1"::"a"(v),"Nd"(port)); }
inline void out16(uint16_t port, uint16_t v){ asm volatile("outw %0,%1"::"a"(v),"Nd"(port)); }
inline void out32(uint16_t port, uint32_t v){ asm volatile("outl %0,%1"::"a"(v),"Nd"(port)); }
inline uint8_t  in8(uint16_t port)  { uint8_t  v; asm volatile("inb %1,%0":"=a"(v):"Nd"(port)); return v; }
inline uint16_t in16(uint16_t port) { uint16_t v; asm volatile("inw %1,%0":"=a"(v):"Nd"(port)); return v; }
inline uint32_t in32(uint16_t port) { uint32_t v; asm volatile("inl %1,%0":"=a"(v):"Nd"(port)); return v; }

// --- Fuente de tiempo independiente de interrupciones (TSC) ---
// Durante el bringup de uACPI las interrupciones estan deshabilitadas, asi que
// timer::ticks() (que avanza por IRQ) no avanza. Calibramos el TSC contra el PIT
// canal 2 una sola vez, haciendo polling del bit OUT (0x61 bit5) sin IRQ.
uint64_t g_tsc_per_us = 0;
uint64_t g_tsc_base = 0;

inline uint64_t rdtsc() {
    uint32_t lo, hi;
    asm volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return (uint64_t(hi) << 32) | lo;
}

void calibrate_tsc() {
    constexpr uint16_t kPitCount = 11932; // ~10ms @ 1193182 Hz
    const uint8_t saved = in8(0x61);
    // Gate ch2 on (bit0=1), speaker off (bit1=0).
    out8(0x61, static_cast<uint8_t>((saved & ~0x02) | 0x01));
    // Ch2, lobyte/hibyte, modo 0 (interrupt on terminal count), binario.
    out8(0x43, 0xB0);
    out8(0x42, kPitCount & 0xFF);
    out8(0x42, static_cast<uint8_t>(kPitCount >> 8));
    const uint64_t start = rdtsc();
    while ((in8(0x61) & 0x20) == 0) { /* espera terminal count */ }
    const uint64_t end = rdtsc();
    out8(0x61, saved);
    g_tsc_per_us = (end - start) / 10000; // 10ms = 10000us
    if (g_tsc_per_us == 0) g_tsc_per_us = 1;
    g_tsc_base = rdtsc();
}

// RSDP fisico, cacheado desde boot_info al inicializar (ver set_rsdp_physical).
uint64_t g_rsdp_physical = 0;

// Un unico SCI: guardamos el handler+ctx de uACPI y registramos un trampolin
// sin-argumentos que es lo que expone arch::x86_64::register_irq_handler.
uacpi_interrupt_handler g_sci_handler = nullptr;
uacpi_handle            g_sci_ctx = nullptr;
uint32_t                g_sci_irq = 0;

void sci_trampoline() {
    if (g_sci_handler) {
        g_sci_handler(g_sci_ctx);
    }
}

// PCI: empaquetamos bus/slot/func en el handle opaco (no hace falta heap).
struct PciAddr { uint8_t bus, slot, func; };
inline uacpi_handle pci_pack(uint8_t bus, uint8_t slot, uint8_t func) {
    return reinterpret_cast<uacpi_handle>(
        static_cast<uintptr_t>(bus) << 16 | static_cast<uintptr_t>(slot) << 8 | func);
}
inline PciAddr pci_unpack(uacpi_handle h) {
    uintptr_t v = reinterpret_cast<uintptr_t>(h);
    return PciAddr{ uint8_t(v >> 16), uint8_t(v >> 8), uint8_t(v) };
}

// I/O port range: empaquetamos la base en el handle (offset se suma al leer).
inline uacpi_handle io_pack(uacpi_io_addr base) { return reinterpret_cast<uacpi_handle>(uintptr_t(base)); }
inline uint16_t     io_base(uacpi_handle h)     { return uint16_t(reinterpret_cast<uintptr_t>(h)); }

// Semaforo minimo para uacpi_kernel_*event (monocore: solo un contador).
struct Event { volatile uint32_t count; };

} // namespace

// Se llama una vez desde el bootstrap ACPI antes de uacpi_initialize().
// boot_info.acpi_rsdp_address viene HHDM-virtual (Limine); lo normalizamos a fisico.
namespace uacpi_glue {
void set_rsdp_physical(uint64_t rsdp_from_bootinfo, uint64_t hhdm_offset) {
    g_rsdp_physical = (rsdp_from_bootinfo >= hhdm_offset)
                        ? rsdp_from_bootinfo - hhdm_offset
                        : rsdp_from_bootinfo;
}

// Bringup de uACPI hasta cargar e inicializar el namespace (interpreta el AML de
// la DSDT/SSDTs). NO habilita GPEs ni instala el SCI todavia (eso es una etapa
// posterior; por ahora el boton de power lo sigue manejando acpi::start_sci).
bool bringup(uint64_t rsdp_from_bootinfo, uint64_t hhdm_offset) {
    // Reloj monotono independiente de IRQ, requerido por stall/sleep/timeouts.
    calibrate_tsc();
    set_rsdp_physical(rsdp_from_bootinfo, hhdm_offset);

    uacpi_status st = uacpi_initialize(0);
    if (uacpi_unlikely_error(st)) {
        console::printf("uacpi: initialize fallo: %s\n", uacpi_status_to_string(st));
        return false;
    }

    st = uacpi_namespace_load();
    if (uacpi_unlikely_error(st)) {
        console::printf("uacpi: namespace_load fallo: %s\n", uacpi_status_to_string(st));
        return false;
    }

    // NOTA: uacpi_namespace_initialize() (corre _INI/_STA) se difiere a la etapa
    // de eventos. Esos metodos acceden a campos con Lock rule y necesitan el
    // handshake del ACPI Global Lock, que a su vez requiere el subsistema de
    // GPE/SCI de uACPI activo y con interrupciones habilitadas. Aca corremos
    // pre-scheduler con IRQ off; el namespace ya cargado alcanza para consultas
    // (enumeracion, _CRS, _PRT). Ver [[uacpi-ioapic-work]].
    console::printf("uacpi: namespace cargado (%s), AML interpretado ok\n",
                    uacpi_status_to_string(st));
    return true;
}
} // namespace uacpi_glue

extern "C" {

// ======================= Log =======================
// Pre-formateado (sin UACPI_FORMATTED_LOGGING). uACPI ya sufija "\n".
void uacpi_kernel_log(uacpi_log_level, const uacpi_char* msg) {
    console::write(msg);
}

// ======================= Tablas / RSDP =======================
uacpi_status uacpi_kernel_get_rsdp(uacpi_phys_addr* out) {
    if (!g_rsdp_physical) return UACPI_STATUS_NOT_FOUND;
    *out = g_rsdp_physical;
    return UACPI_STATUS_OK;
}

// ======================= Mapeo de memoria =======================
void* uacpi_kernel_map(uacpi_phys_addr addr, uacpi_size len) {
    void* virt = nullptr;
    // Flags: presente + escribible + cache-disable (registros ACPI son MMIO).
    if (!vm::map_kernel_mmio(addr, len,
            vm::kPagePresent | vm::kPageWrite | vm::kPageCacheDisable, &virt)) {
        return nullptr;
    }
    return virt;
}
void uacpi_kernel_unmap(void* addr, uacpi_size len) {
    const uint64_t pages = (len + 0xFFF) / 0x1000;
    vm::unmap_kernel_pages(addr, pages);
}

// ======================= MMIO directo =======================
uacpi_u8  uacpi_kernel_mmio_read8 (void* p) { return *reinterpret_cast<volatile uint8_t*>(p); }
uacpi_u16 uacpi_kernel_mmio_read16(void* p) { return *reinterpret_cast<volatile uint16_t*>(p); }
uacpi_u32 uacpi_kernel_mmio_read32(void* p) { return *reinterpret_cast<volatile uint32_t*>(p); }
uacpi_u64 uacpi_kernel_mmio_read64(void* p) { return *reinterpret_cast<volatile uint64_t*>(p); }
void uacpi_kernel_mmio_write8 (void* p, uacpi_u8  v) { *reinterpret_cast<volatile uint8_t*>(p)  = v; }
void uacpi_kernel_mmio_write16(void* p, uacpi_u16 v) { *reinterpret_cast<volatile uint16_t*>(p) = v; }
void uacpi_kernel_mmio_write32(void* p, uacpi_u32 v) { *reinterpret_cast<volatile uint32_t*>(p) = v; }
void uacpi_kernel_mmio_write64(void* p, uacpi_u64 v) { *reinterpret_cast<volatile uint64_t*>(p) = v; }

// ======================= I/O ports =======================
uacpi_status uacpi_kernel_io_map(uacpi_io_addr base, uacpi_size, uacpi_handle* out) {
    *out = io_pack(base);
    return UACPI_STATUS_OK;
}
void uacpi_kernel_io_unmap(uacpi_handle) { /* nada que liberar */ }

uacpi_status uacpi_kernel_io_read8 (uacpi_handle h, uacpi_size off, uacpi_u8*  v) { *v = in8 (io_base(h) + off); return UACPI_STATUS_OK; }
uacpi_status uacpi_kernel_io_read16(uacpi_handle h, uacpi_size off, uacpi_u16* v) { *v = in16(io_base(h) + off); return UACPI_STATUS_OK; }
uacpi_status uacpi_kernel_io_read32(uacpi_handle h, uacpi_size off, uacpi_u32* v) { *v = in32(io_base(h) + off); return UACPI_STATUS_OK; }
uacpi_status uacpi_kernel_io_write8 (uacpi_handle h, uacpi_size off, uacpi_u8  v) { out8 (io_base(h) + off, v); return UACPI_STATUS_OK; }
uacpi_status uacpi_kernel_io_write16(uacpi_handle h, uacpi_size off, uacpi_u16 v) { out16(io_base(h) + off, v); return UACPI_STATUS_OK; }
uacpi_status uacpi_kernel_io_write32(uacpi_handle h, uacpi_size off, uacpi_u32 v) { out32(io_base(h) + off, v); return UACPI_STATUS_OK; }

// ======================= PCI config =======================
// LIMITACION: pci.cpp usa CF8/CFC (256 bytes legacy). offset >= 256 (config
// PCIe extendida via ECAM) devolvera basura. Cubrir cuando exista ECAM/MCFG.
uacpi_status uacpi_kernel_pci_device_open(uacpi_pci_address a, uacpi_handle* out) {
    // a.segment se ignora (un solo segmento en SavanXP hoy).
    *out = pci_pack(a.bus, a.device, a.function);
    return UACPI_STATUS_OK;
}
void uacpi_kernel_pci_device_close(uacpi_handle) {}

uacpi_status uacpi_kernel_pci_read8 (uacpi_handle h, uacpi_size off, uacpi_u8*  v) { auto d=pci_unpack(h); *v=pci::read_config_u8 (d.bus,d.slot,d.func,off); return UACPI_STATUS_OK; }
uacpi_status uacpi_kernel_pci_read16(uacpi_handle h, uacpi_size off, uacpi_u16* v) { auto d=pci_unpack(h); *v=pci::read_config_u16(d.bus,d.slot,d.func,off); return UACPI_STATUS_OK; }
uacpi_status uacpi_kernel_pci_read32(uacpi_handle h, uacpi_size off, uacpi_u32* v) { auto d=pci_unpack(h); *v=pci::read_config_u32(d.bus,d.slot,d.func,off); return UACPI_STATUS_OK; }
uacpi_status uacpi_kernel_pci_write16(uacpi_handle h, uacpi_size off, uacpi_u16 v){ auto d=pci_unpack(h); pci::write_config_u16(d.bus,d.slot,d.func,off,v); return UACPI_STATUS_OK; }
uacpi_status uacpi_kernel_pci_write32(uacpi_handle h, uacpi_size off, uacpi_u32 v){ auto d=pci_unpack(h); pci::write_config_u32(d.bus,d.slot,d.func,off,v); return UACPI_STATUS_OK; }
uacpi_status uacpi_kernel_pci_write8 (uacpi_handle h, uacpi_size off, uacpi_u8  v){ auto d=pci_unpack(h); pci::write_config_u8 (d.bus,d.slot,d.func,off,v); return UACPI_STATUS_OK; }

// ======================= Heap =======================
void* uacpi_kernel_alloc(uacpi_size size) { return heap::allocate(size); }
void* uacpi_kernel_alloc_zeroed(uacpi_size size) {
    void* p = heap::allocate(size);
    if (p) { for (uacpi_size i = 0; i < size; ++i) reinterpret_cast<uint8_t*>(p)[i] = 0; }
    return p;
}
void uacpi_kernel_free(void* mem) { heap::free(mem); }

// ======================= Tiempo (TSC, sin interrupciones) =======================
uacpi_u64 uacpi_kernel_get_nanoseconds_since_boot(void) {
    if (g_tsc_per_us == 0) return 0;
    return ((rdtsc() - g_tsc_base) * 1000ULL) / g_tsc_per_us;
}
void uacpi_kernel_stall(uacpi_u8 usec) {
    const uint64_t target = rdtsc() + uint64_t(usec) * g_tsc_per_us;
    while (rdtsc() < target) { asm volatile("pause"); }
}
void uacpi_kernel_sleep(uacpi_u64 msec) {
    const uint64_t target = rdtsc() + msec * 1000ULL * g_tsc_per_us;
    while (rdtsc() < target) { asm volatile("pause"); }
}

// ======================= Interrupciones (flags) =======================
uacpi_interrupt_state uacpi_kernel_disable_interrupts(void) {
    uint64_t flags;
    asm volatile("pushfq; pop %0; cli" : "=r"(flags) :: "memory");
    return static_cast<uacpi_interrupt_state>(flags);
}
void uacpi_kernel_restore_interrupts(uacpi_interrupt_state state) {
    asm volatile("push %0; popfq" :: "r"(uint64_t(state)) : "memory", "cc");
}

// ======================= Spinlocks (monocore) =======================
// Sin SMP no hay contencion: el "lock" solo salva/restaura IF. Reemplazar por
// un spinlock real cuando exista SMP.
uacpi_handle uacpi_kernel_create_spinlock(void) { return reinterpret_cast<uacpi_handle>(1); }
void uacpi_kernel_free_spinlock(uacpi_handle) {}
uacpi_cpu_flags uacpi_kernel_lock_spinlock(uacpi_handle) {
    return static_cast<uacpi_cpu_flags>(uacpi_kernel_disable_interrupts());
}
void uacpi_kernel_unlock_spinlock(uacpi_handle, uacpi_cpu_flags f) {
    uacpi_kernel_restore_interrupts(static_cast<uacpi_interrupt_state>(f));
}

// ======================= Mutex (monocore, un solo hilo) =======================
// No hay concurrencia real: acquire/release son no-ops que siempre exitan. El
// handle no-nulo basta para que uACPI lo trate como valido.
uacpi_handle uacpi_kernel_create_mutex(void) { return reinterpret_cast<uacpi_handle>(1); }
void uacpi_kernel_free_mutex(uacpi_handle) {}
uacpi_status uacpi_kernel_acquire_mutex(uacpi_handle, uacpi_u16) { return UACPI_STATUS_OK; }
void uacpi_kernel_release_mutex(uacpi_handle) {}

// ======================= Events / semaforos =======================
uacpi_handle uacpi_kernel_create_event(void) {
    Event* e = static_cast<Event*>(heap::allocate(sizeof(Event)));
    if (e) e->count = 0;
    return e;
}
void uacpi_kernel_free_event(uacpi_handle h) { heap::free(h); }
void uacpi_kernel_signal_event(uacpi_handle h) {
    if (h) { Event* e = static_cast<Event*>(h); e->count = e->count + 1; }
}
void uacpi_kernel_reset_event(uacpi_handle h) { if (h) static_cast<Event*>(h)->count = 0; }
uacpi_bool uacpi_kernel_wait_for_event(uacpi_handle h, uacpi_u16 timeout_ms) {
    Event* e = static_cast<Event*>(h);
    // timeout 0xFFFF = infinito. Monocore: si nadie va a signalizar, no colgar.
    const uint64_t deadline = uacpi_kernel_get_nanoseconds_since_boot()
                              + uint64_t(timeout_ms) * 1000000ULL;
    for (;;) {
        if (e->count) { e->count = e->count - 1; return UACPI_TRUE; }
        if (timeout_ms != 0xFFFF &&
            uacpi_kernel_get_nanoseconds_since_boot() >= deadline) return UACPI_FALSE;
        asm volatile("pause");
    }
}

// ======================= Thread id =======================
uacpi_thread_id uacpi_kernel_get_thread_id(void) {
    return reinterpret_cast<uacpi_thread_id>(1); // un solo hilo de kernel
}

// ======================= Firmware request =======================
uacpi_status uacpi_kernel_handle_firmware_request(uacpi_firmware_request* req) {
    switch (req->type) {
        case UACPI_FIRMWARE_REQUEST_TYPE_BREAKPOINT:
            console::printf("uacpi: AML breakpoint\n");
            break;
        case UACPI_FIRMWARE_REQUEST_TYPE_FATAL:
            console::printf("uacpi: AML fatal type=%u code=%u arg=%llu\n",
                            unsigned(req->fatal.type), unsigned(req->fatal.code),
                            (unsigned long long)req->fatal.arg);
            break;
    }
    return UACPI_STATUS_OK;
}

// ======================= Interrupt handler (SCI) =======================
// El SCI es level-triggered / active-low por spec ACPI. Se rutea por el IOAPIC
// (que ya descubre la MADT), con fallback al PIC si la GSI entra en 0..15 y no
// hay IOAPIC. Un solo slot: alcanza para el unico SCI.
uacpi_status uacpi_kernel_install_interrupt_handler(
        uacpi_u32 irq, uacpi_interrupt_handler handler, uacpi_handle ctx,
        uacpi_handle* out_handle) {
    if (g_sci_handler) return UACPI_STATUS_ALREADY_EXISTS;
    g_sci_handler = handler;
    g_sci_ctx = ctx;
    g_sci_irq = irq;

    bool routed = false;
    if (ioapic::ready()) {
        routed = ioapic::route_gsi(irq, ioapic::Polarity::active_low,
                                   ioapic::Trigger::level, sci_trampoline) != 0;
    } else if (irq < 16) {
        routed = arch::x86_64::register_irq_handler(uint8_t(irq), sci_trampoline);
        if (routed) {
            arch::x86_64::enable_irq(uint8_t(irq));
        }
    }
    if (!routed) {
        g_sci_handler = nullptr;
        g_sci_ctx = nullptr;
        return UACPI_STATUS_INTERNAL_ERROR;
    }
    *out_handle = reinterpret_cast<uacpi_handle>(uintptr_t(irq) + 1);
    return UACPI_STATUS_OK;
}
uacpi_status uacpi_kernel_uninstall_interrupt_handler(uacpi_interrupt_handler, uacpi_handle) {
    // TODO: arch no expone unregister_irq_handler todavia.
    g_sci_handler = nullptr;
    g_sci_ctx = nullptr;
    return UACPI_STATUS_OK;
}

// ======================= Trabajo diferido (GPE / Notify) =======================
// PRIMER CORTE: ejecutar sincrono. Correcto para monocore no-preemptivo mientras
// no se llame desde contexto de IRQ con locks tomados. Reemplazar por una cola
// diferida real (kthread / softirq) antes de habilitar GPEs pesados.
uacpi_status uacpi_kernel_schedule_work(uacpi_work_type, uacpi_work_handler handler, uacpi_handle ctx) {
    handler(ctx);
    return UACPI_STATUS_OK;
}
uacpi_status uacpi_kernel_wait_for_work_completion(void) {
    return UACPI_STATUS_OK; // sincrono: nada pendiente
}

} // extern "C"
