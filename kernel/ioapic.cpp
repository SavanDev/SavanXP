// ioapic.cpp — MADT parse + IOAPIC programming + ruteo de GSIs.
//
// ESTADO: ESQUELETO DE EVALUACION. Compila conceptualmente contra lo que ya
// existe (vm, console, cpu), pero NECESITA UN CAMBIO EN arch/x86_64/cpu_init.cpp
// (ver bloque "TOCAR EN ARCH" mas abajo) y no esta cableado al build todavia.
//
// Modelo de entrega:  dispositivo -> IOAPIC (redirection entry) -> Local APIC
//                     (x2APIC) -> vector IDT -> dispatch_external_vector ->
//                     handler -> EOI al Local APIC.

#include "kernel/ioapic.hpp"

#include <stddef.h>
#include <stdint.h>

#include "kernel/acpi.hpp"
#include "kernel/acpi_tables.hpp"
#include "kernel/console.hpp"
#include "kernel/string.hpp"
#include "kernel/vmm.hpp"

namespace {

// ---- MADT (the "APIC" table) ----
// The table is found and its entries are walked by acpi_tables, which bounds
// both; only the two entry types this file understands are declared here.
using acpi_tables::MadtEntryHeader;
using acpi_tables::SdtHeader;
struct [[gnu::packed]] MadtIoApic {
    MadtEntryHeader h; uint8_t io_apic_id, reserved;
    uint32_t io_apic_address; uint32_t gsi_base;   // type 1
};
struct [[gnu::packed]] MadtIso {
    MadtEntryHeader h; uint8_t bus, source; uint32_t gsi; uint16_t flags; // type 2
};

constexpr uint8_t kMadtIoApic = 1;
constexpr uint8_t kMadtIso    = 2;

// ---- Estado descubierto ----
struct IoApic { uint32_t id, gsi_base, gsi_count; volatile uint32_t* regs; };
struct Iso    { uint8_t source; uint32_t gsi; uint16_t flags; };
// Una GSI ya ruteada: se recuerda para compartirla en vez de reprogramarla.
struct Routed { uint32_t gsi; uint8_t vector; bool active_low; bool level; };

constexpr size_t kMaxIoApics = 4;
constexpr size_t kMaxIsos    = 16;
constexpr size_t kMaxRouted  = 16;
IoApic g_ioapics[kMaxIoApics]; size_t g_ioapic_count = 0;
Iso    g_isos[kMaxIsos];       size_t g_iso_count = 0;
Routed g_routed[kMaxRouted];   size_t g_routed_count = 0;
bool   g_ready = false;

// Pool de vectores para GSIs. Arranca despues del MSI-X (49) y del rango PIC.
// TOCAR EN ARCH: hoy cpu_init.cpp solo define trampolines vector_NN hasta 49.
// Para este pool hay que agregar DEFINE_EXTERNAL_ISR(50..63) e incluirlos en
// initialize_idt(). Es mecanico (~14 lineas de macros).
uint8_t g_next_vector = 50;
constexpr uint8_t kVectorPoolEnd = 64;

// ---- Acceso a registros del IOAPIC (IOREGSEL @0x00, IOWIN @0x10) ----
uint32_t ioapic_read(IoApic& io, uint32_t reg) {
    io.regs[0] = reg;              // IOREGSEL
    return io.regs[4];             // IOWIN (offset 0x10 / 4)
}
void ioapic_write(IoApic& io, uint32_t reg, uint32_t value) {
    io.regs[0] = reg;
    io.regs[4] = value;
}

Routed* find_routed(uint32_t gsi) {
    for (size_t i = 0; i < g_routed_count; ++i) {
        if (g_routed[i].gsi == gsi) return &g_routed[i];
    }
    return nullptr;
}

IoApic* owner_of_gsi(uint32_t gsi) {
    for (size_t i = 0; i < g_ioapic_count; ++i) {
        IoApic& io = g_ioapics[i];
        if (gsi >= io.gsi_base && gsi < io.gsi_base + io.gsi_count) return &io;
    }
    return nullptr;
}

// Escribe una entrada de la redirection table (64 bits = 2 regs de 32).
// Layout: [7:0] vector, [10:8] delivery mode (0=fixed), [11] dest mode
// (0=physical), [13] polarity (1=active low), [15] trigger (1=level),
// [16] mask, [63:56] destino (APIC id fisico).
void write_redirection(IoApic& io, uint32_t gsi, uint8_t vector,
                       bool active_low, bool level, bool masked, uint8_t dest_apic) {
    const uint32_t index = 0x10 + (gsi - io.gsi_base) * 2;
    uint32_t low = vector;
    if (active_low) low |= (1u << 13);
    if (level)      low |= (1u << 15);
    if (masked)     low |= (1u << 16);
    const uint32_t high = uint32_t(dest_apic) << 24; // bits [63:56] del qword
    ioapic_write(io, index + 1, high);
    ioapic_write(io, index, low);
}

uint8_t alloc_vector() {
    if (g_next_vector >= kVectorPoolEnd) return 0;
    return g_next_vector++;
}

void parse_madt(const SdtHeader& madt) {
    for (const MadtEntryHeader* h = acpi_tables::madt_next_entry(madt, nullptr); h != nullptr;
         h = acpi_tables::madt_next_entry(madt, h)) {
        // madt_next_entry guarantees h->length bytes; an entry shorter than
        // its type is firmware's mistake and is skipped, not read.
        if (h->type == kMadtIoApic && h->length >= sizeof(MadtIoApic) && g_ioapic_count < kMaxIoApics) {
            const auto* e = reinterpret_cast<const MadtIoApic*>(h);
            void* virt = nullptr;
            if (vm::map_kernel_mmio(e->io_apic_address, 0x20,
                    vm::kPagePresent | vm::kPageWrite | vm::kPageCacheDisable, &virt)) {
                IoApic& io = g_ioapics[g_ioapic_count++];
                io.id = e->io_apic_id; io.gsi_base = e->gsi_base;
                io.regs = reinterpret_cast<volatile uint32_t*>(virt);
                // VER (reg 0x01) bits [23:16] = max redirection entry (count-1).
                io.gsi_count = ((ioapic_read(io, 0x01) >> 16) & 0xff) + 1;
            }
        } else if (h->type == kMadtIso && h->length >= sizeof(MadtIso) && g_iso_count < kMaxIsos) {
            const auto* e = reinterpret_cast<const MadtIso*>(h);
            g_isos[g_iso_count++] = Iso{e->source, e->gsi, e->flags};
        }
    }
}

// Traduce una IRQ ISA a su GSI + polaridad/trigger efectivos aplicando el ISO.
// Sin override: IRQ ISA = edge / active-high, y GSI == IRQ.
void resolve_isa(uint8_t irq, uint32_t& gsi, ioapic::Polarity& pol, ioapic::Trigger& trg) {
    gsi = irq; pol = ioapic::Polarity::active_high; trg = ioapic::Trigger::edge;
    for (size_t i = 0; i < g_iso_count; ++i) {
        if (g_isos[i].source != irq) continue;
        gsi = g_isos[i].gsi;
        const uint16_t f = g_isos[i].flags;
        const uint8_t p = f & 0x3, t = (f >> 2) & 0x3;
        if (p == 0x3) pol = ioapic::Polarity::active_low;   // 1=high, 3=low, 0=bus-default(ISA=high)
        if (t == 0x3) trg = ioapic::Trigger::level;         // 1=edge, 3=level, 0=bus-default(ISA=edge)
        break;
    }
}

} // namespace

namespace ioapic {

bool initialize(uint64_t rsdp_addr, uint64_t hhdm_offset) {
    if (!rsdp_addr || !hhdm_offset) return false;

    // El IOAPIC entrega a traves del Local APIC y cada handler ruteado por aca
    // hace EOI contra el. Sin Local APIC operativo la entrega quedaria a medias:
    // el handler corre pero el bit del ISR nunca se limpia y el APIC se traba
    // para siempre a partir de la primera interrupcion. Preferimos no programar
    // el IOAPIC y dejar que los consumidores caigan al PIC legacy.
    if (!arch::local_apic_ready()) {
        console::printf("ioapic: sin Local APIC operativo, se mantiene el PIC legacy\n");
        return false;
    }

    // acpi::initialize ran first and validated the RSDP; the walk to the
    // MADT is its bounded one.
    const SdtHeader* madt = acpi::find_table("APIC");
    if (madt == nullptr) { console::printf("ioapic: no MADT\n"); return false; }

    parse_madt(*madt);
    if (g_ioapic_count == 0) { console::printf("ioapic: sin IOAPIC en MADT\n"); return false; }

    g_ready = true;
    console::printf("ioapic: %u IOAPIC(s), %u override(s), GSIs base=%u count=%u\n",
                    unsigned(g_ioapic_count), unsigned(g_iso_count),
                    unsigned(g_ioapics[0].gsi_base), unsigned(g_ioapics[0].gsi_count));
    return true;
}

bool ready() { return g_ready; }

uint8_t route_gsi(uint32_t gsi, Polarity pol, Trigger trg, arch::IrqHandler handler) {
    if (!g_ready) return 0;
    IoApic* io = owner_of_gsi(gsi);
    if (!io) return 0;

    const bool active_low = pol == Polarity::active_low;
    const bool level = trg == Trigger::level;

    // GSI ya ruteada: es una linea compartida, no una linea nueva. El SCI cae
    // siempre aca (lo piden acpi::start_sci y el glue de uACPI) y con INTx pasa
    // lo mismo cuando dos links PCI resuelven a la misma GSI. Reusar el vector y
    // encadenar el handler; reprogramar la entrada de redireccion con un vector
    // nuevo dejaria huerfano al handler anterior.
    if (Routed* existing = find_routed(gsi)) {
        if (existing->active_low != active_low || existing->level != level) {
            console::printf(
                "ioapic: GSI %u compartida con otra polaridad/trigger, se conserva la original\n",
                unsigned(gsi));
        }
        if (!arch::register_interrupt_handler(
                existing->vector, handler, arch::InterruptEoi::local_apic)) {
            console::printf("ioapic: no se pudo encadenar handler en GSI %u\n", unsigned(gsi));
            return 0;
        }
        return existing->vector;
    }

    if (g_routed_count >= kMaxRouted) {
        console::printf("ioapic: tabla de GSIs ruteadas llena\n");
        return 0;
    }

    const uint8_t vector = alloc_vector();
    if (!vector) { console::printf("ioapic: pool de vectores agotado\n"); return 0; }

    // Registrar el handler con EOI al Local APIC (la entrega pasa por el).
    // TOCAR EN ARCH: requiere el trampolin vector_NN para `vector` en la IDT.
    if (!arch::register_interrupt_handler(
            vector, handler, arch::InterruptEoi::local_apic)) {
        return 0;
    }

    // Destination: this core, in physical mode. During boot that is the BSP;
    // spread() moves every GSI to the APs once they schedule.
    write_redirection(*io, gsi, vector, active_low, level,
                      /*masked=*/false,
                      /*dest_apic=*/static_cast<uint8_t>(arch::local_apic_id()));
    g_routed[g_routed_count++] = Routed{gsi, vector, active_low, level};
    return vector;
}

uint8_t route_legacy_irq(uint8_t isa_irq, arch::IrqHandler handler) {
    uint32_t gsi; Polarity pol; Trigger trg;
    resolve_isa(isa_irq, gsi, pol, trg);
    return route_gsi(gsi, pol, trg, handler);
}

uint32_t spread(const uint32_t* lapic_ids, uint32_t count) {
    if (count == 0) return 0;
    uint32_t moved = 0;
    for (size_t i = 0; i < g_routed_count; ++i) {
        IoApic* io = owner_of_gsi(g_routed[i].gsi);
        if (!io) continue;
        const uint32_t lapic = lapic_ids[i % count];
        if (lapic > kMaxDestination) continue;
        // Only the high half: one 32-bit write, so the entry never names half
        // an old destination. The vector, mask and trigger stay as routed.
        const uint32_t index = 0x10 + (g_routed[i].gsi - io->gsi_base) * 2;
        ioapic_write(*io, index + 1, lapic << 24);
        console::printf("ioapic: GSI %u (vector %u) -> lapic %u\n",
                        unsigned(g_routed[i].gsi), unsigned(g_routed[i].vector), unsigned(lapic));
        ++moved;
    }
    return moved;
}

void set_gsi_masked(uint32_t gsi, bool masked) {
    IoApic* io = owner_of_gsi(gsi);
    if (!io) return;
    const uint32_t index = 0x10 + (gsi - io->gsi_base) * 2;
    uint32_t low = ioapic_read(*io, index);
    if (masked) low |= (1u << 16); else low &= ~(1u << 16);
    ioapic_write(*io, index, low);
}

} // namespace ioapic
