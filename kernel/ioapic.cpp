// Route MADT-described inputs through the I/O APIC to the BSP local APIC.
#include "kernel/ioapic.hpp"

#include <stddef.h>
#include <stdint.h>

#include "../arch/x86/paging.hpp"
#include "kernel/acpi.hpp"
#include "kernel/console.hpp"

namespace {

// SDT header, local APIC address and flags precede the variable entries.
constexpr size_t kMadtHeaderSize = 44;
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

struct IoApic { uint32_t gsi_base, gsi_count; volatile uint32_t* regs; };
struct Iso    { uint8_t source; uint32_t gsi; uint16_t flags; };
// Keep one redirection entry and vector per shared GSI.
struct Routed { uint32_t gsi; uint8_t vector; bool active_low; bool level; };

constexpr size_t kMaxIoApics = 4;
constexpr size_t kMaxIsos    = 16;
// Vectors 50..63 follow the PCI/MSI-X gates and precede the SMP IPIs.
constexpr uint8_t kVectorPoolStart = 50;
constexpr uint8_t kVectorPoolEnd = 64;
IoApic g_ioapics[kMaxIoApics]; size_t g_ioapic_count = 0;
Iso    g_isos[kMaxIsos];       size_t g_iso_count = 0;
Routed g_routed[kVectorPoolEnd - kVectorPoolStart]; size_t g_routed_count = 0;
bool   g_ready = false;

// Indirect registers: IOREGSEL at 0x00, IOWIN at 0x10.
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
        if (gsi >= io.gsi_base && gsi - io.gsi_base < io.gsi_count) return &io;
    }
    return nullptr;
}

// Fixed delivery in physical destination mode. The redirection entry spans two registers.
void write_redirection(IoApic& io, uint32_t gsi, uint8_t vector,
                       bool active_low, bool level, bool masked, uint8_t dest_apic) {
    const uint32_t index = 0x10 + (gsi - io.gsi_base) * 2;
    uint32_t low = vector;
    if (active_low) low |= (1u << 13);
    if (level)      low |= (1u << 15);
    if (masked)     low |= (1u << 16);
    const uint32_t high = uint32_t(dest_apic) << 24; // Bits [63:56] of the entry.
    ioapic_write(io, index + 1, high);
    ioapic_write(io, index, low);
}

void parse_madt(const void* table, size_t length) {
    const auto* bytes = static_cast<const uint8_t*>(table);
    for (size_t offset = kMadtHeaderSize; length - offset >= sizeof(MadtEntryHeader);) {
        const auto* h = reinterpret_cast<const MadtEntryHeader*>(bytes + offset);
        if (h->length < sizeof(*h) || h->length > length - offset) return;
        if (h->type == kMadtIoApic && h->length >= sizeof(MadtIoApic) &&
            g_ioapic_count < kMaxIoApics) {
            const auto* e = reinterpret_cast<const MadtIoApic*>(h);
            void* virt = nullptr;
            if (e->io_apic_address && !(e->io_apic_address & 4095u) &&
                i386::paging::map_kernel_mmio(e->io_apic_address, 0x20, &virt)) {
                IoApic io{e->gsi_base, 0, static_cast<volatile uint32_t*>(virt)};
                // Register 1 encodes the last redirection entry in bits 23:16.
                io.gsi_count = ((ioapic_read(io, 0x01) >> 16) & 0xff) + 1;
                // The selector is eight bits and redirection entries begin at 0x10.
                if (io.gsi_count <= 120) g_ioapics[g_ioapic_count++] = io;
                else i386::paging::unmap_kernel_pages(virt, 1);
            }
        } else if (h->type == kMadtIso && h->length >= sizeof(MadtIso) &&
                   g_iso_count < kMaxIsos) {
            const auto* e = reinterpret_cast<const MadtIso*>(h);
            if (e->bus == 0 && e->source < 16)
                g_isos[g_iso_count++] = Iso{e->source, e->gsi, e->flags};
        }
        offset += h->length;
    }
}

// ISA defaults to identity routing, active high and edge triggered.
void resolve_isa(uint8_t irq, uint32_t& gsi, ioapic::Polarity& pol, ioapic::Trigger& trg) {
    gsi = irq; pol = ioapic::Polarity::active_high; trg = ioapic::Trigger::edge;
    for (size_t i = 0; i < g_iso_count; ++i) {
        if (g_isos[i].source != irq) continue;
        gsi = g_isos[i].gsi;
        const uint16_t f = g_isos[i].flags;
        const uint8_t p = f & 0x3, t = (f >> 2) & 0x3;
        if (p == 0x3) pol = ioapic::Polarity::active_low;   // 1=high, 3=low, 0=ISA default
        if (t == 0x3) trg = ioapic::Trigger::level;         // 1=edge, 3=level, 0=ISA default
        break;
    }
}

} // namespace

namespace ioapic {

bool initialize() {
    if (g_ready) return true;
    // Every route needs a functioning local APIC to acknowledge delivery.
    if (!arch::x86_64::local_apic_ready()) return false;

    size_t length = 0;
    const void* madt = acpi::table("APIC", length);
    if (!madt || length < kMadtHeaderSize) return false;
    parse_madt(madt, length);
    if (g_ioapic_count == 0) return false;

    g_ready = true;
    console::printf("ioapic: %u controller(s), %u ISA override(s), first GSI base=%u count=%u\n",
                    unsigned(g_ioapic_count), unsigned(g_iso_count),
                    unsigned(g_ioapics[0].gsi_base), unsigned(g_ioapics[0].gsi_count));
    return true;
}

bool ready() { return g_ready; }

uint8_t route_gsi(uint32_t gsi, Polarity pol, Trigger trg, arch::x86_64::IrqHandler handler) {
    if (!g_ready) return 0;
    IoApic* io = owner_of_gsi(gsi);
    if (!io) return 0;

    const bool active_low = pol == Polarity::active_low;
    const bool level = trg == Trigger::level;

    // Reuse the vector and retain the original electrical configuration on shared lines.
    if (Routed* existing = find_routed(gsi)) {
        if (existing->active_low != active_low || existing->level != level) {
            console::printf(
                "ioapic: GSI %u requested with different polarity or trigger; keeping original route\n",
                unsigned(gsi));
        }
        if (!arch::x86_64::register_interrupt_handler(
                existing->vector, handler, arch::x86_64::InterruptEoi::local_apic)) {
            console::printf("ioapic: could not chain handler on GSI %u\n", unsigned(gsi));
            return 0;
        }
        return existing->vector;
    }

    const uint8_t vector = kVectorPoolStart + g_routed_count;
    if (vector >= kVectorPoolEnd) {
        console::write_line("ioapic: vector pool exhausted");
        return 0;
    }

    if (!arch::x86_64::register_interrupt_handler(
            vector, handler, arch::x86_64::InterruptEoi::local_apic)) {
        return 0;
    }

    // Routes are installed by the BSP during boot with interrupts disabled.
    write_redirection(*io, gsi, vector, active_low, level, /*masked=*/false,
                      static_cast<uint8_t>(arch::x86_64::local_apic_id()));
    g_routed[g_routed_count++] = Routed{gsi, vector, active_low, level};
    console::printf("ioapic: GSI %u -> vector %u (%s, active-%s)\n", unsigned(gsi),
                    unsigned(vector), level ? "level" : "edge", active_low ? "low" : "high");
    return vector;
}

uint8_t route_legacy_irq(uint8_t isa_irq, arch::x86_64::IrqHandler handler) {
    if (isa_irq >= 16) return 0;
    uint32_t gsi; Polarity pol; Trigger trg;
    resolve_isa(isa_irq, gsi, pol, trg);
    return route_gsi(gsi, pol, trg, handler);
}

void set_gsi_masked(uint32_t gsi, bool masked) {
    if (!find_routed(gsi)) return;
    IoApic* io = owner_of_gsi(gsi);
    if (!io) return;
    const uint32_t index = 0x10 + (gsi - io->gsi_base) * 2;
    uint32_t low = ioapic_read(*io, index);
    if (masked) low |= (1u << 16); else low &= ~(1u << 16);
    ioapic_write(*io, index, low);
}

} // namespace ioapic
