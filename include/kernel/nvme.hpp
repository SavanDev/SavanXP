#pragma once

#include "kernel/block.hpp"

// NVMe disks: every active namespace with 512-byte blocks on up to two
// controllers, registered as "nvmeN". One I/O queue, completed by polling,
// like the other storage drivers. See docs/kernel/NVME.md.
namespace nvme {

// Whether the disks are registered writable. Off unless the boot entry
// carries `nvme-rw`: until the driver has written to a real drive, an NVMe
// disk on metal is read-only. Called before block::probe_all().
void set_writable(bool writable);

const block::Driver& driver();

} // namespace nvme
