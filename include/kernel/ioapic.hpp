#pragma once

#include <stdint.h>

#include "kernel/cpu.hpp"

namespace ioapic {

// Discover I/O APICs and ISA overrides from the validated ACPI MADT.
// Initialize on the BSP after ACPI and the local APIC.
bool initialize();
bool ready();

enum class Polarity : uint8_t { active_high, active_low };
enum class Trigger  : uint8_t { edge, level };

// Install routes on the BSP at boot. A shared GSI reuses its existing vector;
// each handler must check its device before acting. Return zero on failure.
uint8_t route_gsi(uint32_t gsi, Polarity polarity, Trigger trigger,
                  arch::x86_64::IrqHandler handler);

// Route an ISA IRQ with the MADT's remapping, polarity and trigger overrides.
uint8_t route_legacy_irq(uint8_t isa_irq, arch::x86_64::IrqHandler handler);

// Change an installed route's mask with interrupt-controller access serialized.
void set_gsi_masked(uint32_t gsi, bool masked);

} // namespace ioapic
