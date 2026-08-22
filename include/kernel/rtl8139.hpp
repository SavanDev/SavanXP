#pragma once

#include "kernel/nic.hpp"

namespace rtl8139 {

// Driver del Realtek RTL8139 (PCI 10ec:8139), el NIC que emulan QEMU y
// VirtualBox por defecto. TX por bus-master DMA con 4 slots y RX por ring
// circular; la INTx se rutea por _PRT/IOAPIC y, si eso falla, el camino cae
// solo a polling.
const nic::Driver& driver();

} // namespace rtl8139
