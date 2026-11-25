#pragma once

#include <stdint.h>

#include "arch/cpu.hpp"

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

// Resolves a PCI function's INTx (bus/dev/func) through _PRT and the link's
// _CRS and routes it to the IOAPIC with the given handler. Returns the vector,
// or 0 when the function uses no INTx or it could not be resolved or routed;
// on success *gsi_out, when given, is the GSI it reached. Assumes the function
// sits on the root bus (the _PRT is the root's, keyed by device).
uint8_t route_pci_intx(uint8_t bus, uint8_t dev, uint8_t func,
                       arch::IrqHandler handler, uint32_t* gsi_out = nullptr);

} // namespace uacpi_glue
