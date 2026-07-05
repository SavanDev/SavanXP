#pragma once

#include <stdint.h>

#include "kernel/cpu.hpp"

// Bridge C++ hacia la capa de host de uACPI (uacpi_glue.cpp). El resto de la
// kernel-api de uACPI son funciones extern "C" que uACPI llama directo.
namespace uacpi_glue {

// Inicializa uACPI hasta cargar e inicializar el namespace (interpreta el AML).
// rsdp/hhdm vienen de boot_info. No habilita GPEs ni el SCI todavia. Devuelve
// true si todo el bringup fue exitoso.
bool bringup(uint64_t rsdp_from_bootinfo, uint64_t hhdm_offset);

// Vuelca el _PRT (ruteo de INTx) de los root bridges PCI/PCIe al log. Requiere el
// namespace cargado (bringup). Investigacion previa al routing real de INTx.
void dump_pci_routing();

// Resuelve la INTx de un dispositivo PCI (bus/dev/func) via _PRT + _CRS del link
// y la rutea al IOAPIC con el handler dado. Devuelve el vector asignado, o 0 si el
// dispositivo no usa INTx / no se pudo resolver o rutear. Asume que el dispositivo
// cuelga del root bus (el _PRT es del root, keyed por device).
uint8_t route_pci_intx(uint8_t bus, uint8_t dev, uint8_t func,
                       arch::x86_64::IrqHandler handler);

} // namespace uacpi_glue
