#pragma once

#include <stddef.h>
#include <stdint.h>

namespace fs {
struct Driver;
}

/* FAT12, FAT16 and FAT32 with long names, on FatFs (vendor/fatfs), as an
 * fs::Driver (docs/fs/FAT_ROADMAP.md). Read-only for now: every vnode is
 * published without write permission and the write operations refuse.
 *
 * Never the root (can_be_root is false), and a whole disk is never claimed:
 * only a device whose sector 0 is a FAT boot sector, so a partitioned disk
 * and its first partition are never the same volume twice. */
namespace fat {

const fs::Driver& driver();

// Sector 0 of a FAT volume: the boot signature and the "FAT" or "FAT32"
// file-system type string where FAT12/16 and FAT32 keep it. What probe
// checks before FatFs sees the device, and what storage:: reports as "fat".
bool is_boot_sector(const uint8_t* sector);

} // namespace fat
