#pragma once

#include <stddef.h>
#include <stdint.h>

/* FatFs's disk layer (disk_*, get_fattime, ff_memalloc/ff_memfree) over
 * block::, rtc:: and heap:: (docs/fs/FAT_ROADMAP.md#the-disk-layer-diskio).
 *
 * FatFs names a disk by a drive number, one per FatFs volume. The FAT driver
 * binds a drive to a block device before f_mount and unbinds it after
 * f_unmount. It is a layer of its own so the host test can also bind a drive
 * to build its images with FatFs before the driver mounts them. */
namespace fat_disk {

// FF_VOLUMES in kernel/fatfs/ffconf.h.
constexpr uint8_t kDrives = 4;

// The drive reads and writes block device `device_index` from now on. False
// if the drive is out of range or already bound.
bool bind(uint8_t drive, size_t device_index);
void unbind(uint8_t drive);
bool bound(uint8_t drive);

// FAT keeps local time packed in 16-bit date and time words (the format
// get_fattime returns: date in the high half). The kernel's clock is
// rtc::epoch_seconds(), read from a chip that keeps local time, so no zone
// is applied either way. Dates before 1980 or after 2107 are clamped.
uint32_t fat_time(uint64_t epoch_seconds);
uint64_t epoch_seconds(uint16_t date, uint16_t time);

} // namespace fat_disk
