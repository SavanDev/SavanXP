#pragma once

#include <stddef.h>
#include <stdint.h>

#include "abi/savanxp_native_abi.h"

/* The storage calls' view of block::, partition:: and fs:: together: what a
 * Disk Management program needs to show and, while elevated, to change
 * (docs/userland/DISK_MANAGEMENT.md). The native dispatcher reaches storage
 * only through here. */
namespace storage {

// Bumps whenever a device, a partition table or a mount changes.
uint32_t generation();

// The index-th present block device, in table order. false past the last.
bool snapshot(size_t index, sxn_storage_device& out);

} // namespace storage
