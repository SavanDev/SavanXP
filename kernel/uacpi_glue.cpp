// The reference AML interpreter runs on the BSP during boot. APs are parked,
// and the existing ACPI driver owns the fixed power-button event.
extern "C" {
#include "uacpi/kernel_api.h"
#include "uacpi/namespace.h"
#include "uacpi/resources.h"
#include "uacpi/status.h"
#include "uacpi/uacpi.h"
#include "uacpi/utilities.h"
#include "uacpi/platform/arch_helpers.h"
}

#include <stddef.h>
#include <stdint.h>

#include "kernel/acpi.hpp"
#include "kernel/console.hpp"
#include "kernel/cpu.hpp"
#include "kernel/heap.hpp"
#include "kernel/ioapic.hpp"
#include "kernel/pci.hpp"
#include "kernel/timer.hpp"
#include "../arch/x86/paging.hpp"
#include "kernel/uacpi_glue.hpp"

namespace {

// --- Port I/O (mismo patron que acpi.cpp; falta out32/in32 alla) ---
inline void out8(uint16_t port, uint8_t v)  { asm volatile("outb %0,%1"::"a"(v),"Nd"(port)); }
inline void out16(uint16_t port, uint16_t v){ asm volatile("outw %0,%1"::"a"(v),"Nd"(port)); }
inline void out32(uint16_t port, uint32_t v){ asm volatile("outl %0,%1"::"a"(v),"Nd"(port)); }
inline uint8_t  in8(uint16_t port)  { uint8_t  v; asm volatile("inb %1,%0":"=a"(v):"Nd"(port)); return v; }
inline uint16_t in16(uint16_t port) { uint16_t v; asm volatile("inw %1,%0":"=a"(v):"Nd"(port)); return v; }
inline uint32_t in32(uint16_t port) { uint32_t v; asm volatile("inl %1,%0":"=a"(v):"Nd"(port)); return v; }

uint64_t g_rsdp_physical;
bool g_namespace_ready;

// uACPI shares the existing SCI vector through this argument-free trampoline.
uacpi_interrupt_handler g_sci_handler = nullptr;
uacpi_handle            g_sci_ctx = nullptr;

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

namespace uacpi_glue {
bool bringup(uint64_t rsdp_physical) {
    if (!rsdp_physical || !ioapic::ready()) return false;
    g_rsdp_physical = rsdp_physical;

    uacpi_status status = uacpi_initialize(0);
    if (status == UACPI_STATUS_OK) status = uacpi_namespace_load();
    if (status == UACPI_STATUS_OK) {
        // Firmware can return a different _PRT for the PIC and I/O APIC.
        status = uacpi_set_interrupt_model(UACPI_INTERRUPT_MODEL_IOAPIC);
    }
    // Initialization clears fixed-event enables even if a later stage fails.
    acpi::enable_power_button();
    if (uacpi_unlikely_error(status)) {
        console::printf("uacpi: AML boot setup failed: %s\n", uacpi_status_to_string(status));
        return false;
    }
    // Namespace loading chains uACPI on the SCI. As in v0.4.0, defer
    // _INI/_STA and GPE activation while boot runs with interrupts disabled.
    g_namespace_ready = true;
    console::write_line("uacpi: AML namespace loaded for IOAPIC routing");
    return true;
}

// IRQ resuelta desde el _CRS de un PCI link device.
struct LinkIrq {
    uint32_t gsi;
    uint8_t triggering; // UACPI_TRIGGERING_*
    uint8_t polarity;   // UACPI_POLARITY_*
    bool found;
};

// Callback de _CRS: captura la primera IRQ (descriptor IRQ o Extended IRQ).
uacpi_iteration_decision link_crs_cb(void* user, uacpi_resource* res) {
    LinkIrq* out = static_cast<LinkIrq*>(user);
    if (res->type == UACPI_RESOURCE_TYPE_IRQ && res->irq.num_irqs > 0) {
        out->gsi = res->irq.irqs[0];
        out->triggering = res->irq.triggering;
        out->polarity = res->irq.polarity;
        out->found = true;
        return UACPI_ITERATION_DECISION_BREAK;
    }
    if (res->type == UACPI_RESOURCE_TYPE_EXTENDED_IRQ && res->extended_irq.num_irqs > 0) {
        out->gsi = res->extended_irq.irqs[0];
        out->triggering = res->extended_irq.triggering;
        out->polarity = res->extended_irq.polarity;
        out->found = true;
        return UACPI_ITERATION_DECISION_BREAK;
    }
    return UACPI_ITERATION_DECISION_CONTINUE;
}

// Evalua el _CRS de un link device y extrae la GSI a la que esta ruteado ahora.
bool resolve_link_gsi(uacpi_namespace_node* link, LinkIrq& out) {
    uacpi_resources* res = nullptr;
    if (uacpi_unlikely_error(uacpi_get_current_resources(link, &res))) {
        return false;
    }
    out.found = false;
    uacpi_for_each_resource(res, link_crs_cb, &out);
    uacpi_free_resources(res);
    return out.found;
}

struct PrtDumpCtx {
    uacpi_namespace_node* seen[16];
    int seen_count;
};

// Callback por cada PCI root bridge: recorre su _PRT y resuelve cada link device
// unico a su GSI real (via _CRS). En q35 todas las entradas usan links (LNKx);
// las GSI directas (source==NULL) se saltean aca.
uacpi_iteration_decision prt_dump_cb(void* user, uacpi_namespace_node* node, uacpi_u32) {
    PrtDumpCtx* ctx = static_cast<PrtDumpCtx*>(user);
    uacpi_pci_routing_table* prt = nullptr;
    if (uacpi_unlikely_error(uacpi_get_pci_routing_table(node, &prt))) {
        return UACPI_ITERATION_DECISION_CONTINUE;
    }

    console::printf("uacpi: _PRT con %u entradas, links:\n",
                    static_cast<unsigned>(prt->num_entries));
    for (uacpi_size i = 0; i < prt->num_entries; ++i) {
        const uacpi_pci_routing_table_entry& e = prt->entries[i];
        if (e.source == nullptr) {
            continue; // GSI directa (no ocurre en q35)
        }
        bool seen = false;
        for (int k = 0; k < ctx->seen_count; ++k) {
            if (ctx->seen[k] == e.source) { seen = true; break; }
        }
        if (seen) {
            continue;
        }
        if (ctx->seen_count < 16) {
            ctx->seen[ctx->seen_count++] = e.source;
        }

        char name[5] = {};
        const uacpi_object_name n = uacpi_namespace_node_name(e.source);
        for (int k = 0; k < 4; ++k) name[k] = n.text[k];

        LinkIrq irq = {};
        if (resolve_link_gsi(e.source, irq)) {
            console::printf("  %s -> GSI %u (%s, active-%s)\n", name,
                            static_cast<unsigned>(irq.gsi),
                            irq.triggering == UACPI_TRIGGERING_LEVEL ? "level" : "edge",
                            irq.polarity == UACPI_POLARITY_ACTIVE_LOW ? "low" : "high");
        } else {
            console::printf("  %s -> (sin IRQ en _CRS)\n", name);
        }
    }

    uacpi_free_pci_routing_table(prt);
    return UACPI_ITERATION_DECISION_CONTINUE;
}

// Recorre los root bridges PCI/PCIe y resuelve los link devices del _PRT a GSIs.
void dump_pci_routing() {
    if (!g_namespace_ready) return;
    PrtDumpCtx ctx = {};
    uacpi_find_devices("PNP0A03", prt_dump_cb, &ctx); // PCI root bridge
    uacpi_find_devices("PNP0A08", prt_dump_cb, &ctx); // PCIe root bridge
}

// Resolve the INTx entry on the device's root bridge.
struct RouteCtx {
    uint8_t bus;
    uint8_t device;
    uint8_t function;
    uint8_t target_pin; // Zero-based (0 = INTA).
    LinkIrq result;
    const char* source;
};

uacpi_iteration_decision route_find_cb(void* user, uacpi_namespace_node* node, uacpi_u32) {
    RouteCtx* ctx = static_cast<RouteCtx*>(user);
    uacpi_u64 segment = 0, bus = 0;
    auto status = uacpi_eval_integer(node, "_SEG", nullptr, &segment);
    if (status != UACPI_STATUS_NOT_FOUND && (uacpi_unlikely_error(status) || segment != 0))
        return UACPI_ITERATION_DECISION_CONTINUE;
    status = uacpi_eval_integer(node, "_BBN", nullptr, &bus);
    if (status != UACPI_STATUS_NOT_FOUND && (uacpi_unlikely_error(status) || bus != ctx->bus))
        return UACPI_ITERATION_DECISION_CONTINUE;
    if (status == UACPI_STATUS_NOT_FOUND && ctx->bus != 0)
        return UACPI_ITERATION_DECISION_CONTINUE;

    uacpi_pci_routing_table* prt = nullptr;
    if (uacpi_unlikely_error(uacpi_get_pci_routing_table(node, &prt))) {
        return UACPI_ITERATION_DECISION_CONTINUE;
    }
    for (uacpi_size i = 0; i < prt->num_entries; ++i) {
        const uacpi_pci_routing_table_entry& e = prt->entries[i];
        const uint16_t function = uint16_t(e.address);
        if ((e.address >> 16) != ctx->device || e.pin != ctx->target_pin ||
            (function != 0xffff && function != ctx->function)) {
            continue;
        }
        if (e.source == nullptr) {
            // A direct PCI GSI uses the standard level/active-low mode.
            ctx->result.gsi = e.index;
            ctx->result.triggering = UACPI_TRIGGERING_LEVEL;
            ctx->result.polarity = UACPI_POLARITY_ACTIVE_LOW;
            ctx->source = "direct GSI";
        } else if (e.index == 0 && resolve_link_gsi(e.source, ctx->result)) {
            ctx->source = "link _CRS";
        }
        if (ctx->source) break;
    }
    uacpi_free_pci_routing_table(prt);
    return ctx->source ? UACPI_ITERATION_DECISION_BREAK : UACPI_ITERATION_DECISION_CONTINUE;
}

static uint8_t route_pci_intx_ioapic(uint8_t bus, uint8_t dev, uint8_t func, uint8_t pin,
                                     arch::x86_64::IrqHandler handler) {
    if (!g_namespace_ready) return 0;
    RouteCtx ctx = {};
    ctx.bus = bus;
    ctx.device = dev;
    ctx.function = func;
    ctx.target_pin = static_cast<uint8_t>(pin - 1);
    uacpi_find_devices("PNP0A03", route_find_cb, &ctx);
    if (!ctx.source) {
        uacpi_find_devices("PNP0A08", route_find_cb, &ctx);
    }
    if (!ctx.source) {
        console::printf("uacpi: INTx dev %u has no matching _PRT entry\n", static_cast<unsigned>(dev));
        return 0;
    }
    // Apply the firmware-provided polarity and trigger mode.
    const ioapic::Polarity pol = ctx.result.polarity == UACPI_POLARITY_ACTIVE_LOW
                                     ? ioapic::Polarity::active_low
                                     : ioapic::Polarity::active_high;
    const ioapic::Trigger trg = ctx.result.triggering == UACPI_TRIGGERING_LEVEL
                                    ? ioapic::Trigger::level
                                    : ioapic::Trigger::edge;
    const uint8_t vec = ioapic::route_gsi(ctx.result.gsi, pol, trg, handler);
    console::printf("uacpi: INTx dev %u INT%c -> GSI %u %s (vector %u, %s)\n",
                    static_cast<unsigned>(dev), static_cast<char>('A' + ctx.target_pin),
                    static_cast<unsigned>(ctx.result.gsi), vec ? "routed" : "unavailable",
                    static_cast<unsigned>(vec), ctx.source);
    return vec;
}

uint8_t route_pci_intx(uint8_t bus, uint8_t dev, uint8_t func,
                       arch::x86_64::IrqHandler handler) {
    const uint8_t pin = pci::read_config_u8(bus, dev, func, 0x3d);
    if (pin < 1 || pin > 4) return 0;
    if (const uint8_t vector = route_pci_intx_ioapic(bus, dev, func, pin, handler))
        return vector;

    const uint8_t irq = pci::read_config_u8(bus, dev, func, 0x3c);
    if (irq < 3 || irq >= 16 || irq == 8 || irq == 13) return 0;
    if (!arch::x86_64::register_irq_handler(irq, handler)) return 0;
    arch::x86_64::enable_irq(irq);
    return 0x20 + irq;
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
    if (!i386::paging::map_kernel_mmio(addr, len, &virt)) return nullptr;
    return virt;
}
void uacpi_kernel_unmap(void* addr, uacpi_size len) {
    // The mapper returns the original offset within the first mapped page.
    const uintptr_t address = reinterpret_cast<uintptr_t>(addr);
    const uint32_t offset = address & (i386::paging::kPageBytes - 1);
    const uint32_t pages = (uint64_t(offset) + len + i386::paging::kPageBytes - 1) /
                           i386::paging::kPageBytes;
    i386::paging::unmap_kernel_pages(reinterpret_cast<void*>(address - offset), pages);
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
uacpi_status uacpi_kernel_io_map(uacpi_io_addr base, uacpi_size length, uacpi_handle* out) {
    if (base >= 0x10000 || length > 0x10000 - base) return UACPI_STATUS_INVALID_ARGUMENT;
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

// CF8/CFC exposes only the aligned registers of segment zero's first 256 bytes.
uacpi_status uacpi_kernel_pci_device_open(uacpi_pci_address a, uacpi_handle* out) {
    if (a.segment != 0 || a.device > 31 || a.function > 7)
        return UACPI_STATUS_INVALID_ARGUMENT;
    *out = pci_pack(a.bus, a.device, a.function);
    return UACPI_STATUS_OK;
}
void uacpi_kernel_pci_device_close(uacpi_handle) {}

uacpi_status uacpi_kernel_pci_read8(uacpi_handle handle, uacpi_size offset, uacpi_u8* value) {
    if (offset >= 256) return UACPI_STATUS_INVALID_ARGUMENT;
    const auto addr = pci_unpack(handle);
    *value = pci::read_config_u8(addr.bus, addr.slot, addr.func, uint8_t(offset));
    return UACPI_STATUS_OK;
}
uacpi_status uacpi_kernel_pci_read16(uacpi_handle handle, uacpi_size offset, uacpi_u16* value) {
    if (offset >= 256 || offset % sizeof(uacpi_u16)) return UACPI_STATUS_INVALID_ARGUMENT;
    const auto addr = pci_unpack(handle);
    *value = pci::read_config_u16(addr.bus, addr.slot, addr.func, uint8_t(offset));
    return UACPI_STATUS_OK;
}
uacpi_status uacpi_kernel_pci_read32(uacpi_handle handle, uacpi_size offset, uacpi_u32* value) {
    if (offset >= 256 || offset % sizeof(uacpi_u32)) return UACPI_STATUS_INVALID_ARGUMENT;
    const auto addr = pci_unpack(handle);
    *value = pci::read_config_u32(addr.bus, addr.slot, addr.func, uint8_t(offset));
    return UACPI_STATUS_OK;
}

uacpi_status uacpi_kernel_pci_write8(uacpi_handle handle, uacpi_size offset, uacpi_u8 value) {
    if (offset >= 256) return UACPI_STATUS_INVALID_ARGUMENT;
    const auto addr = pci_unpack(handle);
    pci::write_config_u8(addr.bus, addr.slot, addr.func, uint8_t(offset), value);
    return UACPI_STATUS_OK;
}
uacpi_status uacpi_kernel_pci_write16(uacpi_handle handle, uacpi_size offset, uacpi_u16 value) {
    if (offset >= 256 || offset % sizeof(uacpi_u16)) return UACPI_STATUS_INVALID_ARGUMENT;
    const auto addr = pci_unpack(handle);
    pci::write_config_u16(addr.bus, addr.slot, addr.func, uint8_t(offset), value);
    return UACPI_STATUS_OK;
}
uacpi_status uacpi_kernel_pci_write32(uacpi_handle handle, uacpi_size offset, uacpi_u32 value) {
    if (offset >= 256 || offset % sizeof(uacpi_u32)) return UACPI_STATUS_INVALID_ARGUMENT;
    const auto addr = pci_unpack(handle);
    pci::write_config_u32(addr.bus, addr.slot, addr.func, uint8_t(offset), value);
    return UACPI_STATUS_OK;
}

// ======================= Heap =======================
void* uacpi_kernel_alloc(uacpi_size size) { return heap::allocate(size); }
void* uacpi_kernel_alloc_zeroed(uacpi_size size) {
    void* p = heap::allocate(size);
    if (p) { for (uacpi_size i = 0; i < size; ++i) reinterpret_cast<uint8_t*>(p)[i] = 0; }
    return p;
}
void uacpi_kernel_free(void* mem) { heap::free(mem); }

// Native monotonic time advances with IF clear during boot.
uacpi_u64 uacpi_kernel_get_nanoseconds_since_boot(void) { return timer::monotonic_ns(); }
void uacpi_kernel_stall(uacpi_u8 usec) {
    const uint64_t target = timer::monotonic_ns() + uint64_t(usec) * 1000;
    while (timer::monotonic_ns() < target) asm volatile("pause");
}
void uacpi_kernel_sleep(uacpi_u64 msec) {
    const uint64_t target = timer::monotonic_ns() + msec * 1000000;
    while (timer::monotonic_ns() < target) asm volatile("pause");
}

// ======================= Interrupciones (flags) =======================
uacpi_interrupt_state uacpi_kernel_disable_interrupts(void) {
    uintptr_t flags;
    asm volatile("pushfl; pop %0; cli" : "=r"(flags) :: "memory");
    return static_cast<uacpi_interrupt_state>(flags);
}
void uacpi_kernel_restore_interrupts(uacpi_interrupt_state state) {
    asm volatile("push %0; popfl" :: "r"(uintptr_t(state)) : "memory", "cc");
}

// All interpreter calls run on the BSP before AP scheduling starts.
uacpi_handle uacpi_kernel_create_spinlock(void) { return reinterpret_cast<uacpi_handle>(1); }
void uacpi_kernel_free_spinlock(uacpi_handle) {}
uacpi_cpu_flags uacpi_kernel_lock_spinlock(uacpi_handle) {
    return static_cast<uacpi_cpu_flags>(uacpi_kernel_disable_interrupts());
}
void uacpi_kernel_unlock_spinlock(uacpi_handle, uacpi_cpu_flags f) {
    uacpi_kernel_restore_interrupts(static_cast<uacpi_interrupt_state>(f));
}

// Boot AML queries have a single caller.
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
