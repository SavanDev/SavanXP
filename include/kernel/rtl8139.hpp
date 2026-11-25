#pragma once

#include "kernel/nic.hpp"

namespace lifecycle {
struct Subject;
}

namespace rtl8139 {

// Driver del Realtek RTL8139 (PCI 10ec:8139), el NIC que emulan QEMU y
// VirtualBox por defecto. TX por bus-master DMA con 4 slots y RX por ring
// circular; la INTx se rutea por _PRT/IOAPIC y, si eso falla, el camino cae
// solo a polling.
const nic::Driver& driver();

/* Boot option `rtl8139-unmapped-tx`: with the IOMMU translating, transmit
 * buffer 0 is left out of the card's domain, so the first frame it sends
 * faults and the card ends blocked (docs/kernel/IOMMU.md#testing). Call
 * before the card is brought up. */
void leave_transmit_buffer_unmapped(bool leave);

/* Its start and stop, for `lifecycle-selftest=rtl8139`
 * (docs/kernel/DRIVER_LIFECYCLE.md). */
const lifecycle::Subject& lifecycle_subject();

} // namespace rtl8139
