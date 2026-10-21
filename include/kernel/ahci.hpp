#pragma once

#include "kernel/block.hpp"

// AHCI (SATA) disks through the HBA's DMA engine. Its enumerate() registers
// every port with a SATA disk behind it as "ahciN"; ATAPI drives and empty
// ports are skipped. Like the other storage drivers it completes requests by
// polling, but a request moves up to 256 sectors in one command instead of
// one PIO data phase per sector. See docs/kernel/AHCI.md.
namespace ahci {

const block::Driver& driver();

} // namespace ahci
