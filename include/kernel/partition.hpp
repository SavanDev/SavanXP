#pragma once

#include <stddef.h>
#include <stdint.h>

#include "kernel/block.hpp"

namespace partition {

// A block driver of its own kind: it talks to no hardware, it slices the
// devices the others already registered. That is why its priority is the
// lowest of all and probe_all runs it last, once the physical disks and the
// ramdisk are in the table. Each partition becomes one more block:: device,
// with its own LBA 0, so sxfs:: (and later fat::) mount a partition without
// knowing there is an offset underneath.
const block::Driver& driver();

// How a disk is partitioned.
enum class Scheme : uint8_t {
    none = 0,    // no 0xAA55 signature: a raw volume or a blank disk
    mbr = 1,
    gpt = 2,
    // "EFI PART" is there but neither header (primary or backup) passes its
    // CRC. Nothing is sliced, and the MBR is not trusted either: on a GPT disk
    // it is only a protective entry.
    damaged = 3,
};

constexpr size_t kGuidSize = 16;
// A GPT name is 36 UTF-16 code units; kept as ASCII ('?' for anything else).
constexpr size_t kNameCapacity = 37;

struct Info {
    // Device the partition was cut from.
    size_t parent_index;
    // Offset inside the parent, in sectors.
    uint32_t start_lba;
    uint32_t sector_count;
    Scheme scheme;
    // 1-based position in the table: the GPT entry slot, or the MBR entry.
    // It is the N in the device name "<parent>pN".
    uint32_t number;
    // MBR entry type, or 0 when the partition came from a GPT.
    uint8_t mbr_type;
    // EFI System Partition: type GUID C12A7328-... in GPT, type 0xEF in MBR.
    bool esp;
    // GPT only (zero for MBR), as stored on disk (mixed-endian GUIDs).
    uint8_t type_guid[kGuidSize];
    uint8_t unique_guid[kGuidSize];
    char name[kNameCapacity];
};

// false if the device is not a partition (a whole disk, or the ramdisk).
// Setup uses it so it does not format a whole disk when a partition was
// chosen, and to find the ESP to put the boot loader on.
bool info(size_t device_index, Info& out);

struct DiskInfo {
    Scheme scheme;
    // The primary GPT header failed its CRC and the backup one was used.
    bool gpt_from_backup;
    uint8_t disk_guid[kGuidSize];
};

// Reads a disk's table again: its partitions are removed from block:: and
// registered anew from what is on the disk now. The caller makes sure none of
// them, nor the disk, is mounted. false if `disk_index` is not a disk or a
// partition could not be removed.
bool rescan(size_t disk_index);

// What partition:: found on a whole device. A device it never looked at (a
// partition itself, a disk with fewer than 2 sectors) reads as Scheme::none.
DiskInfo disk_info(size_t device_index);

} // namespace partition
