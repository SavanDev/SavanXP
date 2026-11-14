#pragma once

#include "kernel/nic.hpp"

namespace rtl8168 {

// Driver for the Realtek RTL8111/8168 PCIe gigabit family and its siblings
// (PCI 10ec:8168, 8161, 8136, 8169, 8167): descriptor rings in C+ mode, one
// frame in flight on TX, RX drained by polling or by an INTx routed through
// _PRT/IOAPIC when the device sits on the root bus. See
// docs/hardware/REAL_HARDWARE.md (HW-1).
const nic::Driver& driver();

} // namespace rtl8168
