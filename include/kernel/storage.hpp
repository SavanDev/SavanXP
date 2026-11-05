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

// The most sectors one read or write moves.
constexpr uint32_t kMaxTransferSectors = 128;

// Raw sector access on the device with this id; the caller checks the caller
// acts as the system. 0 or a negative status. A write is refused with -EBUSY
// when it overlaps a mounted volume on the same disk (the device itself, the
// disk it belongs to, or a partition inside the range), and is on the medium
// when it returns.
int read(uint32_t id, uint64_t lba, uint32_t count, void* buffer);
int write(uint32_t id, uint64_t lba, uint32_t count, const void* buffer);
// Reads a disk's partition table again. -EBUSY if the disk or any of its
// partitions is mounted, -EINVAL for a partition.
int rescan(uint32_t id);

} // namespace storage
