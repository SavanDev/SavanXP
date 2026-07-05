#pragma once

#include "boot/boot_info.hpp"

namespace acpi {

// Parsea las tablas ACPI (RSDP -> RSDT/XSDT -> FADT, y el _S5_ de la DSDT)
// usando el RSDP que entrega Limine y el HHDM para traducir phys->virt.
void initialize(const boot::BootInfo& boot_info);

// true si se encontraron RSDP y FADT validos.
bool ready();

// Habilita el modo ACPI y rutea la SCI (via IOAPIC, o PIC como fallback) para
// atender eventos fijos: hoy el boton de encendido (PWRBTN -> shutdown S5). Las
// GPE quedan enmascaradas (sin interprete AML). Debe llamarse despues de
// ioapic::initialize. Devuelve true si la SCI quedo ruteada.
bool start_sci();

// Apaga la maquina (estado S5 via PM1a/PM1b_CNT). Si no hay _S5_ o no apaga,
// intenta los puertos de apagado conocidos de los hipervisores y, como ultimo
// recurso, detiene la CPU para siempre. No retorna.
[[noreturn]] void shutdown();

// Reinicia la maquina con una cascada de metodos: reset register de la FADT,
// puerto 0xCF9 y el controlador de teclado 8042. No retorna.
[[noreturn]] void reboot();

} // namespace acpi
