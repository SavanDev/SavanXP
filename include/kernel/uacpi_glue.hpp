#pragma once

#include <stdint.h>

#include "kernel/cpu.hpp"

namespace uacpi_glue {

// Load AML for BSP-only PCI routing queries before the scheduler starts.
// The caller passes the firmware RSDP's physical address.
bool bringup(uint64_t rsdp_physical);

// Vuelca el _PRT (ruteo de INTx) de los root bridges PCI/PCIe al log. Requiere el
// namespace cargado (bringup). Investigacion previa al routing real de INTx.
void dump_pci_routing();

// Resolve a root-bus PCI function using _PRT and the link device's _CRS.
// Return the assigned vector or zero if the route is unavailable.
uint8_t route_pci_intx(uint8_t bus, uint8_t dev, uint8_t func,
                       arch::x86_64::IrqHandler handler);

} // namespace uacpi_glue
