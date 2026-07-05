#pragma once

#include <stdint.h>

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

} // namespace uacpi_glue
