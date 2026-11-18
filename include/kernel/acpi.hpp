#pragma once

#include <stdint.h>

#include "boot/boot_info.hpp"
#include "kernel/acpi_tables.hpp"

namespace acpi {

// Parsea las tablas ACPI (RSDP -> RSDT/XSDT -> FADT, y el _S5_ de la DSDT)
// usando el RSDP que entrega Limine y el HHDM para traducir phys->virt.
void initialize(const boot::BootInfo& boot_info);

// True when a valid RSDP and FADT were found.
bool ready();

// The first table with `signature`, readable for its whole length, or nullptr.
// Works once initialize found a valid RSDP, with or without a FADT. The walk
// is bounded (acpi_tables.hpp); the caller still checks the table is long
// enough for the fields it reads.
const acpi_tables::SdtHeader* find_table(const char (&signature)[5]);

// PM timer del chipset (ACPI): contador libre a 3.579545 MHz declarado por la
// FADT. Puerto 0 = la maquina no lo expone. Es de 24 bits salvo que la FADT
// diga TMR_VAL_EXT, y en ese caso de 32. Lo lee timer::adopt_pm_timer para
// tener un reloj de pared que siga al tiempo virtual de la maquina.
uint16_t pm_timer_port();
bool pm_timer_is_32bit();
uint32_t pm_timer_read();
constexpr uint32_t kPmTimerHz = 3579545u;

// Habilita el modo ACPI y rutea la SCI (via IOAPIC, o PIC como fallback) para
// atender eventos fijos: hoy el boton de encendido (PWRBTN -> shutdown S5). Las
// GPE quedan enmascaradas (sin interprete AML). Debe llamarse despues de
// ioapic::initialize. Devuelve true si la SCI quedo ruteada.
bool start_sci();

// Habilita PWRBTN_EN (y limpia un PWRBTN_STS pendiente) sin tocar el resto de
// los eventos fijos. Idempotente. La usa start_sci y hay que volver a llamarla
// despues del bringup de uACPI, que apaga TODOS los eventos fijos en su init.
bool enable_power_button();

// Powers the machine off: S5 through PM1a/PM1b_CNT, then the known hypervisor
// power-off ports. Returns, with interrupts off, only if the machine is still
// on about a second later: power::shutdown() then says it is safe to turn off.
void shutdown();

// Resets the machine with a cascade of methods: the FADT reset register, port
// 0xCF9 and the 8042 keyboard controller. Returns, with interrupts off, only if
// none of them did.
void reboot();

} // namespace acpi
