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

#include "kernel/console.hpp"
#include "kernel/string.hpp"
#include "kernel/vmm.hpp"

namespace {

// ---- Igual que acpi.cpp: normaliza phys/virt con el HHDM ----
uint64_t g_hhdm = 0;
template <typename T> T* map(uint64_t addr) {
    return reinterpret_cast<T*>(addr >= g_hhdm ? addr : addr + g_hhdm);
}
bool checksum_ok(const void* d, size_t n) {
    const uint8_t* b = static_cast<const uint8_t*>(d); uint8_t s = 0;
    for (size_t i = 0; i < n; ++i) s = uint8_t(s + b[i]);
    return s == 0;
}

struct [[gnu::packed]] SdtHeader {
    char signature[4]; uint32_t length; uint8_t revision, checksum;
    char oem_id[6], oem_table_id[8]; uint32_t oem_revision, creator_id, creator_revision;
};
struct [[gnu::packed]] Rsdp {
    char signature[8]; uint8_t checksum; char oem_id[6];
    uint8_t revision; uint32_t rsdt_address;
    uint32_t length; uint64_t xsdt_address; uint8_t extended_checksum, reserved[3];
};

// ---- MADT (tabla "APIC") ----
struct [[gnu::packed]] Madt {
    SdtHeader header; uint32_t local_apic_address; uint32_t flags;
    // ...seguido de entradas variables (type,len,...)
};
struct [[gnu::packed]] MadtEntryHeader { uint8_t type, length; };
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

constexpr size_t kMaxIoApics = 4;
constexpr size_t kMaxIsos    = 16;
IoApic g_ioapics[kMaxIoApics]; size_t g_ioapic_count = 0;
Iso    g_isos[kMaxIsos];       size_t g_iso_count = 0;
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

// ---- Camino de tablas: RSDP -> XSDT/RSDT -> "APIC" ----
const SdtHeader* find_madt(const Rsdp* rsdp) {
    const bool use_xsdt = rsdp->revision >= 2 && rsdp->xsdt_address != 0;
    const SdtHeader* root = use_xsdt ? map<SdtHeader>(rsdp->xsdt_address)
                                     : map<SdtHeader>(rsdp->rsdt_address);
    const size_t esz = use_xsdt ? 8 : 4;
    const size_t n = (root->length - sizeof(SdtHeader)) / esz;
    const uint8_t* e = reinterpret_cast<const uint8_t*>(root) + sizeof(SdtHeader);
    for (size_t i = 0; i < n; ++i) {
        uint64_t phys = 0; memcpy(&phys, e + i * esz, esz);
        const SdtHeader* t = map<SdtHeader>(phys);
        if (memcmp(t->signature, "APIC", 4) == 0) return t;
    }
    return nullptr;
}

void parse_madt(const Madt* madt) {
    const uint8_t* p   = reinterpret_cast<const uint8_t*>(madt) + sizeof(Madt);
    const uint8_t* end = reinterpret_cast<const uint8_t*>(madt) + madt->header.length;
    while (p + sizeof(MadtEntryHeader) <= end) {
        const auto* h = reinterpret_cast<const MadtEntryHeader*>(p);
        if (h->length == 0) break; // proteccion anti-loop
        if (h->type == kMadtIoApic && g_ioapic_count < kMaxIoApics) {
            const auto* e = reinterpret_cast<const MadtIoApic*>(p);
            void* virt = nullptr;
            if (vm::map_kernel_mmio(e->io_apic_address, 0x20,
                    vm::kPagePresent | vm::kPageWrite | vm::kPageCacheDisable, &virt)) {
                IoApic& io = g_ioapics[g_ioapic_count++];
                io.id = e->io_apic_id; io.gsi_base = e->gsi_base;
                io.regs = reinterpret_cast<volatile uint32_t*>(virt);
                // VER (reg 0x01) bits [23:16] = max redirection entry (count-1).
                io.gsi_count = ((ioapic_read(io, 0x01) >> 16) & 0xff) + 1;
            }
        } else if (h->type == kMadtIso && g_iso_count < kMaxIsos) {
            const auto* e = reinterpret_cast<const MadtIso*>(p);
            g_isos[g_iso_count++] = Iso{e->source, e->gsi, e->flags};
        }
        p += h->length;
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
    g_hhdm = hhdm_offset;
    if (!rsdp_addr || !hhdm_offset) return false;

    // El IOAPIC entrega a traves del Local APIC y cada handler ruteado por aca
    // hace EOI contra el. Sin Local APIC operativo la entrega quedaria a medias:
    // el handler corre pero el bit del ISR nunca se limpia y el APIC se traba
    // para siempre a partir de la primera interrupcion. Preferimos no programar
    // el IOAPIC y dejar que los consumidores caigan al PIC legacy.
    if (!arch::x86_64::local_apic_ready()) {
        console::printf("ioapic: sin Local APIC operativo, se mantiene el PIC legacy\n");
        return false;
    }

    const Rsdp* rsdp = map<Rsdp>(rsdp_addr);
    if (memcmp(rsdp->signature, "RSD PTR ", 8) != 0 || !checksum_ok(rsdp, 20)) {
        console::printf("ioapic: RSDP invalido\n"); return false;
    }
    const SdtHeader* madt_hdr = find_madt(rsdp);
    if (!madt_hdr) { console::printf("ioapic: MADT ausente\n"); return false; }

    parse_madt(reinterpret_cast<const Madt*>(madt_hdr));
    if (g_ioapic_count == 0) { console::printf("ioapic: sin IOAPIC en MADT\n"); return false; }

    g_ready = true;
    console::printf("ioapic: %u IOAPIC(s), %u override(s), GSIs base=%u count=%u\n",
                    unsigned(g_ioapic_count), unsigned(g_iso_count),
                    unsigned(g_ioapics[0].gsi_base), unsigned(g_ioapics[0].gsi_count));
    return true;
}

bool ready() { return g_ready; }

uint8_t route_gsi(uint32_t gsi, Polarity pol, Trigger trg, arch::x86_64::IrqHandler handler) {
    if (!g_ready) return 0;
    IoApic* io = owner_of_gsi(gsi);
    if (!io) return 0;

    const uint8_t vector = alloc_vector();
    if (!vector) { console::printf("ioapic: pool de vectores agotado\n"); return 0; }

    // Registrar el handler con EOI al Local APIC (la entrega pasa por el).
    // TOCAR EN ARCH: requiere el trampolin vector_NN para `vector` en la IDT.
    if (!arch::x86_64::register_interrupt_handler(
            vector, handler, arch::x86_64::InterruptEoi::local_apic)) {
        return 0;
    }

    // Destino: el BSP, en modo fisico. Con SMP habria que elegir CPU y considerar
    // destino logico cuando el id de x2APIC no entra en 8 bits.
    write_redirection(*io, gsi, vector,
                      pol == Polarity::active_low, trg == Trigger::level,
                      /*masked=*/false,
                      /*dest_apic=*/static_cast<uint8_t>(arch::x86_64::local_apic_id()));
    return vector;
}

uint8_t route_legacy_irq(uint8_t isa_irq, arch::x86_64::IrqHandler handler) {
    uint32_t gsi; Polarity pol; Trigger trg;
    resolve_isa(isa_irq, gsi, pol, trg);
    return route_gsi(gsi, pol, trg, handler);
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
