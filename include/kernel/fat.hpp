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

// Sector 0 of a FAT volume: an x86 jump (0xEB or 0xE9), the boot signature,
// and the "FAT" or "FAT32" file-system type string where FAT12/16 and FAT32
// keep it. What probe checks before FatFs sees the device, what storage::
// reports as "fat", and what tells partition:: that a disk holding it whole
// (a "superfloppy" stick) has no partition table, although its sector 0 ends
// in 0x55AA like an MBR. Inline, so partition:: does not link the driver.
inline bool is_boot_sector(const uint8_t* sector) {
    const bool jump = sector[0] == 0xeb || sector[0] == 0xe9;
    const bool signature = sector[510] == 0x55 && sector[511] == 0xaa;
    const bool fat16 = sector[54] == 'F' && sector[55] == 'A' && sector[56] == 'T';
    const bool fat32 = sector[82] == 'F' && sector[83] == 'A' && sector[84] == 'T' && sector[85] == '3' &&
        sector[86] == '2';
    return jump && signature && (fat16 || fat32);
}

} // namespace fat
