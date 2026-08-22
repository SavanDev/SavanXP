#pragma once

#include <stdint.h>

#include "kernel/cpu.hpp"

// Capa IOAPIC + MADT (ESQUELETO DE EVALUACION).
//
// Que resuelve: enrutar GSIs (Global System Interrupts) hacia vectores de la
// IDT a traves del IOAPIC, aplicando los Interrupt Source Overrides de la MADT
// (polaridad/trigger/remapeo IRQ->GSI). Es la pieza que falta para:
//   * Recibir el SCI de ACPI (uacpi_kernel_install_interrupt_handler).
//   * Rutear INTx legacy resuelto por _PRT (tu bloqueo documentado en q35+APIC).
//   * Migrar PS/2 (IRQ1/IRQ12) del PIC legacy al IOAPIC.
//
// Depende de: Local APIC operativo (x2APIC o xAPIC), vm::map_kernel_mmio y el
// dispatcher de vectores de cpu_init.cpp. Sin Local APIC no se inicializa: la
// entrega por IOAPIC necesita EOI contra el.

namespace ioapic {

// Parsea la MADT (tabla "APIC"): descubre IOAPICs y guarda los Interrupt Source
// Overrides. Debe llamarse despues de acpi::initialize y del Local APIC.
// Devuelve false si no hay MADT o ningun IOAPIC (caeria de vuelta al PIC).
bool initialize(uint64_t rsdp_physical_or_virtual, uint64_t hhdm_offset);

bool ready();

// Polaridad / disparo de una linea (semantica de la MADT / _PRT).
enum class Polarity : uint8_t { active_high, active_low };
enum class Trigger  : uint8_t { edge, level };

// Rutea una GSI cruda hacia un handler. Elige un vector libre, programa la
// entrada de redireccion del IOAPIC dueño de esa GSI y registra el handler con
// EOI al Local APIC. `polarity`/`trigger` vienen de _PRT o del SCI de la FADT.
// Devuelve el vector asignado, o 0 si fallo.
uint8_t route_gsi(uint32_t gsi, Polarity polarity, Trigger trigger,
                  arch::x86_64::IrqHandler handler);

// Rutea una IRQ ISA legacy (0..15) aplicando el override de la MADT si existe
// (p.ej. IRQ0->GSI2, o polaridad/trigger no-default). Punto de entrada para
// migrar PS/2. Devuelve el vector asignado o 0.
uint8_t route_legacy_irq(uint8_t isa_irq, arch::x86_64::IrqHandler handler);

// Enmascara / desenmascara una GSI ya ruteada (bit 16 de la entrada).
void set_gsi_masked(uint32_t gsi, bool masked);

} // namespace ioapic
