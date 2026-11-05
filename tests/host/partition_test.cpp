/*
 * partition_test.cpp -- host test of the partition driver
 * (kernel/partition.cpp), running the REAL driver against a fake block:: and
 * MBR/GPT tables built in memory.
 *
 * Why on the host and not in a QEMU smoke: the cases that matter -- a valid
 * GPT, a damaged primary header, a table that lies about a size, a partition
 * that runs off the disk -- take two lines each here, and no disk image has to
 * be prepared for each of them.
 *
 * The fake block:: lives in this translation unit (not in
 * sxfs_host_stubs.cpp) on purpose: the SxFS test's has a single device and no
 * register_device, while here the device registry IS what is under test,
 * because partition:: publishes each slice through it.
 */

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "kernel/block.hpp"
#include "kernel/console.hpp"
#include "kernel/partition.hpp"
#include "kernel/string.hpp"
#include "shared/crc32.h"
#include "sxdisk/sxdisk.h"

namespace console {
void printf(const char* format, ...) {
    va_list args;
    va_start(args, format);
    ::vprintf(format, args);
    va_end(args);
}
} // namespace console

namespace {

int g_failures = 0;
int g_checks = 0;

bool check(bool ok, const char* what) {
    ++g_checks;
    if (!ok) {
        ++g_failures;
        printf("  FAIL %s\n", what);
    } else {
        printf("  ok   %s\n", what);
    }
    return ok;
}

// --- Fake block:: -----------------------------------------------------------
// Two kinds of device live together, as in the kernel: "raw" ones backed by
// an in-memory image (the physical disk) and the ones partition:: registers
// with its own DeviceOps (the slices). The kernel's block:: core checks the
// range and the read-only flag, so it is replicated here: without it the test
// could not show that a read past the partition is refused.

constexpr size_t kMaxDevices = 16;

struct Device {
    bool present;
    bool writable;
    const char* name;
    uint32_t sector_count;
    // Raw device: its own bytes. Slice: the driver's ops and context.
    uint8_t* image;
    const block::DeviceOps* ops;
    void* context;
};

Device g_devices[kMaxDevices] = {};
size_t g_device_count = 0;

void reset_devices() {
    for (size_t index = 0; index < g_device_count; ++index) {
        free(g_devices[index].image);
    }
    memset(g_devices, 0, sizeof(g_devices));
    g_device_count = 0;
}

// A blank physical disk. Returns its device index.
size_t add_disk(uint32_t sector_count, const char* name, bool writable) {
    Device& device = g_devices[g_device_count];
    device.present = true;
    device.writable = writable;
    device.name = name;
    device.sector_count = sector_count;
    device.image = static_cast<uint8_t*>(calloc(sector_count, block::kSectorSize));
    return g_device_count++;
}

uint8_t* disk_bytes(size_t index) {
    return g_devices[index].image;
}

bool range_ok(size_t index, uint32_t lba, uint32_t sector_count) {
    if (index >= g_device_count || !g_devices[index].present || sector_count == 0) {
        return false;
    }
    return static_cast<uint64_t>(lba) + sector_count <= g_devices[index].sector_count;
}

size_t byte_offset(uint32_t lba) {
    return static_cast<size_t>(lba) * block::kSectorSize;
}

size_t byte_length(uint32_t sector_count) {
    return static_cast<size_t>(sector_count) * block::kSectorSize;
}

// --- Building tables --------------------------------------------------------

void put32(uint8_t* out, uint32_t value) {
    out[0] = static_cast<uint8_t>(value);
    out[1] = static_cast<uint8_t>(value >> 8);
    out[2] = static_cast<uint8_t>(value >> 16);
    out[3] = static_cast<uint8_t>(value >> 24);
}

void put64(uint8_t* out, uint64_t value) {
    put32(out, static_cast<uint32_t>(value));
    put32(out + 4, static_cast<uint32_t>(value >> 32));
}

constexpr size_t kMbrTableOffset = 446;
constexpr size_t kMbrEntrySize = 16;

// The EFI System Partition type GUID as stored on disk.
const uint8_t kEspTypeGuid[16] = {
    0x28, 0x73, 0x2a, 0xc1, 0x1f, 0xf8, 0xd2, 0x11,
    0xba, 0x4b, 0x00, 0xa0, 0xc9, 0x3e, 0xc9, 0x3b,
};
// Basic data partition, for the second entry.
const uint8_t kDataTypeGuid[16] = {
    0xa2, 0xa0, 0xd0, 0xeb, 0xe5, 0xb9, 0x33, 0x44,
    0x87, 0xc0, 0x68, 0xb6, 0xb7, 0x26, 0x99, 0xc7,
};

void write_boot_signature(uint8_t* image) {
    image[510] = 0x55;
    image[511] = 0xaa;
}

uint8_t* mbr_entry(uint8_t* image, size_t index) {
    return image + kMbrTableOffset + (index * kMbrEntrySize);
}

constexpr uint32_t kGptEntryCount = 128;
constexpr uint32_t kGptEntrySize = 128;
constexpr uint32_t kGptArraySectors = kGptEntryCount * kGptEntrySize / block::kSectorSize;

void put_name(uint8_t* entry, const char* name) {
    for (size_t index = 0; name[index] != 0 && index < 36; ++index) {
        entry[56 + index * 2] = static_cast<uint8_t>(name[index]);
    }
}

// Writes one GPT header at `header_lba` describing the array at `entry_lba`,
// with both CRCs, as any real tool does.
void write_gpt_header(uint8_t* image, uint32_t sectors, uint32_t header_lba, uint32_t alternate_lba,
    uint32_t entry_lba)
{
    uint8_t* header = image + static_cast<size_t>(header_lba) * block::kSectorSize;
    memset(header, 0, block::kSectorSize);
    memcpy(header, "EFI PART", 8);
    put32(header + 8, 0x00010000u);
    put32(header + 12, 92);
    put64(header + 24, header_lba);
    put64(header + 32, alternate_lba);
    put64(header + 40, 2 + kGptArraySectors);
    put64(header + 48, sectors - 2 - kGptArraySectors);
    header[56] = 0xd1; // disk GUID
    header[71] = 0x5c;
    put64(header + 72, entry_lba);
    put32(header + 80, kGptEntryCount);
    put32(header + 84, kGptEntrySize);
    const uint8_t* entries = image + static_cast<size_t>(entry_lba) * block::kSectorSize;
    put32(header + 88, sx_crc32(entries, kGptEntryCount * kGptEntrySize));
    put32(header + 16, sx_crc32(header, 92));
}

// A GPT with two partitions: a 16 MiB ESP at LBA 2048 and data for the rest.
// With `protective`, also the 0xEE MBR any real tool writes; without it, a
// hybrid, which must still be read. The backup copy goes at the end.
void build_gpt(size_t device, bool protective) {
    uint8_t* image = disk_bytes(device);
    const uint32_t sectors = g_devices[device].sector_count;
    write_boot_signature(image);

    if (protective) {
        uint8_t* entry = mbr_entry(image, 0);
        entry[4] = 0xee;
        put32(entry + 8, 1);
        put32(entry + 12, sectors - 1);
    }

    uint8_t* first = image + 2 * block::kSectorSize;
    memcpy(first, kEspTypeGuid, sizeof(kEspTypeGuid));
    first[16] = 0x11; // a unique GUID != 0, or the entry counts as empty
    put64(first + 32, 2048);
    put64(first + 40, 2048 + 32768 - 1);
    put_name(first, "EFI system");

    uint8_t* second = first + kGptEntrySize;
    memcpy(second, kDataTypeGuid, sizeof(kDataTypeGuid));
    second[16] = 0x22;
    put64(second + 32, 2048 + 32768);
    put64(second + 40, sectors - 34); // last usable LBA, before the backup GPT
    put_name(second, "Data");

    const uint32_t backup_entries = sectors - 1 - kGptArraySectors;
    memcpy(image + static_cast<size_t>(backup_entries) * block::kSectorSize, first,
        kGptArraySectors * block::kSectorSize);
    write_gpt_header(image, sectors, 1, sectors - 1, 2);
    write_gpt_header(image, sectors, sectors - 1, 1, backup_entries);
}

// A classic MBR: a FAT32, an extended one (to be skipped), an ESP by type
// 0xEF, and a last one that lies about its size and must be clipped.
void build_mbr(size_t device) {
    uint8_t* image = disk_bytes(device);
    write_boot_signature(image);

    uint8_t* first = mbr_entry(image, 0);
    first[4] = 0x0c; // FAT32 LBA
    put32(first + 8, 2048);
    put32(first + 12, 16384);

    uint8_t* extended = mbr_entry(image, 1);
    extended[4] = 0x05;
    put32(extended + 8, 2048 + 16384);
    put32(extended + 12, 1024);

    uint8_t* esp = mbr_entry(image, 2);
    esp[4] = 0xef;
    put32(esp + 8, 2048 + 17408);
    put32(esp + 12, 4096);

    uint8_t* oversized = mbr_entry(image, 3);
    oversized[4] = 0x83;
    put32(oversized + 8, 2048 + 21504);
    put32(oversized + 12, 0xffffffffu);
}

void enumerate_partitions() {
    partition::driver().enumerate();
}

// --- Cases ------------------------------------------------------------------

// A valid GPT is sliced into devices of their own, with the ESP flagged and the
// name derived from the parent.
void case_gpt_publishes_partitions() {
    printf("case: GPT with a protective MBR\n");
    reset_devices();
    const size_t disk = add_disk(131072, "ata0", /*writable=*/true); // 64 MiB
    build_gpt(disk, /*protective=*/true);
    enumerate_partitions();

    check(g_device_count == 3, "the disk plus its two partitions");

    partition::Info info = {};
    check(!partition::info(disk, info), "the whole disk is not a partition");
    if (check(partition::info(1, info), "the first slice is a partition")) {
        check(info.parent_index == disk, "it points at its parent disk");
        check(info.start_lba == 2048 && info.sector_count == 32768, "ESP offset and size");
        check(info.esp, "recognised as the ESP by its type GUID");
        check(info.mbr_type == 0, "no MBR type: it comes from a GPT");
    }
    if (check(partition::info(2, info), "the second slice is a partition")) {
        check(info.start_lba == 34816, "it starts where the ESP ends");
        check(!info.esp, "the data one is not an ESP");
    }

    block::DeviceInfo device = {};
    check(block::device_info(1, device) && strcmp(device.name, "ata0p1") == 0, "named ata0p1");
    check(block::device_info(2, device) && strcmp(device.name, "ata0p2") == 0, "named ata0p2");
}

// What makes the layer useful: a partition is a device with its own LBA 0.
void case_partition_offsets_reach_the_parent() {
    printf("case: the partition's LBA 0 lands at the parent's offset\n");
    reset_devices();
    const size_t disk = add_disk(131072, "ata0", /*writable=*/true);
    build_gpt(disk, /*protective=*/true);
    enumerate_partitions();

    uint8_t sector[block::kSectorSize];
    memset(sector, 0xa5, sizeof(sector));
    check(block::write(1, 0, 1, sector), "write to the partition's LBA 0");

    uint8_t readback[block::kSectorSize] = {};
    check(block::read(disk, 2048, 1, readback) && readback[0] == 0xa5,
          "it landed on the disk's LBA 2048");

    check(!block::read(1, 32768, 1, readback), "reading a sector past the end is refused");
    check(block::read(1, 32767, 1, readback), "the partition's last sector does read");
}

// Without the 0xEE MBR the GPT must still win: hybrid images look like this.
void case_gpt_without_protective_mbr() {
    printf("case: GPT without a protective MBR\n");
    reset_devices();
    const size_t disk = add_disk(131072, "ata0", /*writable=*/true);
    build_gpt(disk, /*protective=*/false);
    enumerate_partitions();
    check(g_device_count == 3, "the GPT wins without the 0xEE");
}

void case_mbr_table() {
    printf("case: classic MBR\n");
    reset_devices();
    const size_t disk = add_disk(65536, "ata0", /*writable=*/true); // 32 MiB
    build_mbr(disk);
    enumerate_partitions();

    check(g_device_count == 4, "three primaries: the extended one is skipped");

    partition::Info info = {};
    if (check(partition::info(1, info), "first primary registered")) {
        check(info.mbr_type == 0x0c && info.start_lba == 2048, "FAT32 at LBA 2048");
    }
    if (check(partition::info(2, info), "second primary registered")) {
        check(info.mbr_type == 0xef && info.esp, "ESP recognised by type 0xEF");
    }
    if (check(partition::info(3, info), "third primary registered")) {
        // The table declares 0xffffffff sectors from 23552 on a 65536-sector
        // disk: without the clip, block:: would accept reads off the device.
        check(info.sector_count == 65536 - 23552, "clipped to the parent's real size");
    }
}

// A "superfloppy": FAT over the whole disk, no table. Its boot sector ends in
// 0x55AA like an MBR, and the bytes where an MBR keeps its entries are FAT's
// boot code, here made to look like a partition. It is a volume: nothing is
// sliced and the disk reads as having no scheme.
void case_fat_volume_is_not_a_table() {
    printf("case: FAT over the whole disk\n");
    reset_devices();
    const size_t disk = add_disk(65536, "ahci3", /*writable=*/true);
    uint8_t* image = disk_bytes(disk);
    image[0] = 0xeb;
    image[1] = 0x3c;
    image[2] = 0x90;
    memcpy(image + 54, "FAT16   ", 8);
    uint8_t* entry = mbr_entry(image, 0);
    entry[4] = 0x06;
    put32(entry + 8, 63);
    put32(entry + 12, 1000);
    write_boot_signature(image);
    enumerate_partitions();

    check(g_device_count == 1, "nothing sliced from a FAT boot sector");
    check(partition::disk_info(disk).scheme == partition::Scheme::none, "the disk has no scheme");
}

void case_read_only_disk_yields_read_only_partitions() {
    printf("case: read-only disk\n");
    reset_devices();
    const size_t disk = add_disk(131072, "cd0", /*writable=*/false);
    build_gpt(disk, /*protective=*/true);
    enumerate_partitions();

    block::DeviceInfo device = {};
    check(block::device_info(1, device) && !device.writable, "the partition inherits read-only");

    uint8_t sector[block::kSectorSize] = {};
    check(!block::write(1, 0, 1, sector), "writing to it is refused");
}

// SavanXP's disk is a raw SxFS with no table today: it is left alone.
void case_raw_disk_is_left_alone() {
    printf("case: raw disk with no table\n");
    reset_devices();
    add_disk(4096, "ata0", /*writable=*/true);
    enumerate_partitions();
    check(g_device_count == 1, "without the 0xAA55 signature nothing is sliced");
}

void case_bogus_entries_are_discarded() {
    printf("case: invalid MBR entries\n");
    reset_devices();
    const size_t disk = add_disk(4096, "ata0", /*writable=*/true);
    uint8_t* image = disk_bytes(disk);
    write_boot_signature(image);

    uint8_t* out_of_range = mbr_entry(image, 0);
    out_of_range[4] = 0x83;
    put32(out_of_range + 8, 999999); // starts off the disk
    put32(out_of_range + 12, 100);

    uint8_t* empty = mbr_entry(image, 1);
    empty[4] = 0x83;
    put32(empty + 8, 100);
    put32(empty + 12, 0); // zero size

    enumerate_partitions();
    check(g_device_count == 1, "out-of-range or empty entries are discarded");
}

// Several disks at once: each slice must stay tied to ITS parent.
void case_two_disks_do_not_cross() {
    printf("case: two partitioned disks\n");
    reset_devices();
    const size_t first = add_disk(131072, "ata0", /*writable=*/true);
    const size_t second = add_disk(65536, "ata1", /*writable=*/true);
    build_gpt(first, /*protective=*/true);
    build_mbr(second);
    enumerate_partitions();

    check(g_device_count == 2 + 2 + 3, "two disks, two slices and three slices");

    partition::Info info = {};
    size_t from_first = 0;
    size_t from_second = 0;
    for (size_t index = 2; index < g_device_count; ++index) {
        if (!partition::info(index, info)) {
            continue;
        }
        if (info.parent_index == first) {
            ++from_first;
        } else if (info.parent_index == second) {
            ++from_second;
        }
    }
    check(from_first == 2 && from_second == 3, "each slice points at its own disk");

    block::DeviceInfo device = {};
    check(block::device_info(4, device) && strcmp(device.name, "ata1p1") == 0,
          "the second disk's are named ata1pN");
}

// What a partition editor needs: the type and unique GUIDs, the name, the
// entry number, and the disk's scheme.
void case_gpt_metadata() {
    printf("case: GPT metadata\n");
    reset_devices();
    const size_t disk = add_disk(131072, "ata0", /*writable=*/true);
    build_gpt(disk, /*protective=*/true);
    enumerate_partitions();

    const partition::DiskInfo table = partition::disk_info(disk);
    check(table.scheme == partition::Scheme::gpt, "the disk reads as GPT");
    check(!table.gpt_from_backup, "from the primary header");
    check(table.disk_guid[0] == 0xd1 && table.disk_guid[15] == 0x5c, "the disk GUID is kept");

    partition::Info info = {};
    if (check(partition::info(1, info), "the ESP is there")) {
        check(info.scheme == partition::Scheme::gpt && info.number == 1, "entry 1 of a GPT");
        check(memcmp(info.type_guid, kEspTypeGuid, 16) == 0, "its type GUID is kept");
        check(info.unique_guid[0] == 0x11, "its unique GUID is kept");
        check(strcmp(info.name, "EFI system") == 0, "its name is read from UTF-16");
    }
    if (check(partition::info(2, info), "the data partition is there")) {
        check(info.number == 2 && strcmp(info.name, "Data") == 0, "entry 2, named Data");
    }
}

// A primary header that fails its CRC falls back to the backup at the end.
void case_damaged_primary_uses_backup() {
    printf("case: damaged primary GPT\n");
    reset_devices();
    const size_t disk = add_disk(131072, "ata0", /*writable=*/true);
    build_gpt(disk, /*protective=*/true);
    disk_bytes(disk)[block::kSectorSize + 60] ^= 0xff; // breaks the header CRC
    enumerate_partitions();

    check(g_device_count == 3, "both partitions come from the backup");
    const partition::DiskInfo table = partition::disk_info(disk);
    check(table.scheme == partition::Scheme::gpt && table.gpt_from_backup, "flagged as read from the backup");
}

// A damaged entry array breaks the primary too: its CRC is in the header.
void case_damaged_entries_use_backup() {
    printf("case: damaged primary entry array\n");
    reset_devices();
    const size_t disk = add_disk(131072, "ata0", /*writable=*/true);
    build_gpt(disk, /*protective=*/true);
    disk_bytes(disk)[2 * block::kSectorSize + 40] ^= 0x01; // the ESP's end LBA
    enumerate_partitions();

    partition::Info info = {};
    check(partition::info(1, info) && info.sector_count == 32768, "the backup's ESP, unchanged");
    check(partition::disk_info(disk).gpt_from_backup, "flagged as read from the backup");
}

// Both copies broken: nothing is sliced, and the protective MBR is not taken
// for a real one.
void case_both_gpt_copies_damaged() {
    printf("case: both GPT copies damaged\n");
    reset_devices();
    const size_t disk = add_disk(131072, "ata0", /*writable=*/true);
    build_gpt(disk, /*protective=*/true);
    disk_bytes(disk)[block::kSectorSize + 60] ^= 0xff;
    disk_bytes(disk)[static_cast<size_t>(131071) * block::kSectorSize + 60] ^= 0xff;
    enumerate_partitions();

    check(g_device_count == 1, "nothing is sliced");
    check(partition::disk_info(disk).scheme == partition::Scheme::damaged, "the disk reads as damaged");
}

void case_disk_schemes() {
    printf("case: schemes of MBR and raw disks\n");
    reset_devices();
    const size_t mbr = add_disk(65536, "ata0", /*writable=*/true);
    const size_t raw = add_disk(4096, "ata1", /*writable=*/true);
    build_mbr(mbr);
    enumerate_partitions();

    check(partition::disk_info(mbr).scheme == partition::Scheme::mbr, "an MBR disk reads as MBR");
    check(partition::disk_info(raw).scheme == partition::Scheme::none, "a raw disk reads as none");
    partition::Info info = {};
    check(partition::info(2, info) && info.scheme == partition::Scheme::mbr && info.number == 1,
          "an MBR partition knows its scheme and entry");
}

// The contract with libsxdisk: a table it writes is one this reader slices,
// with the same numbers, types and names.
int sxdisk_read_disk(void* cookie, uint64_t lba, uint32_t count, void* buffer) {
    const size_t device = *static_cast<size_t*>(cookie);
    return block::read(device, static_cast<uint32_t>(lba), count, buffer) ? 0 : -1;
}

int sxdisk_write_disk(void* cookie, uint64_t lba, uint32_t count, const void* buffer) {
    const size_t device = *static_cast<size_t*>(cookie);
    return block::write(device, static_cast<uint32_t>(lba), count, buffer) ? 0 : -1;
}

void sxdisk_fill(void*, void* out, size_t size) {
    for (size_t index = 0; index < size; ++index) {
        static_cast<uint8_t*>(out)[index] = static_cast<uint8_t>(0x30 + index);
    }
}

void case_libsxdisk_tables_are_sliced() {
    printf("case: a table written by libsxdisk\n");
    reset_devices();
    size_t disk = add_disk(131072, "ahci1", /*writable=*/true);
    const sxdisk_io io = {&disk, &sxdisk_read_disk, &sxdisk_write_disk, &sxdisk_fill};
    static sxdisk_gpt table;
    check(sxdisk_gpt_init(&table, 131072, &io) == SXDISK_OK, "libsxdisk builds an empty table");
    uint32_t number = 0;
    check(sxdisk_gpt_add(&table, 2048, 8192, sxdisk_type_esp, "EFI system", &io, nullptr) == SXDISK_OK, "an ESP");
    check(sxdisk_gpt_add(&table, 10240, 65536, sxdisk_type_sxfs, "SavanXP", &io, &number) == SXDISK_OK &&
          number == 2, "an SxFS partition as entry 2");
    check(sxdisk_gpt_remove(&table, 1) == SXDISK_OK, "the ESP removed again: entry 1 empty");
    check(sxdisk_gpt_write(&table, &io) == SXDISK_OK, "libsxdisk writes it");
    enumerate_partitions();

    check(g_device_count == 2, "the disk and one partition");
    partition::Info info = {};
    if (check(partition::info(1, info), "the SxFS partition is sliced")) {
        check(info.number == 2 && info.start_lba == 10240 && info.sector_count == 65536, "entry 2, its range");
        check(memcmp(info.type_guid, sxdisk_type_sxfs, 16) == 0, "the SxFS type GUID");
        check(strcmp(info.name, "SavanXP") == 0, "its name");
    }
    block::DeviceInfo device = {};
    check(block::device_info(1, device) && strcmp(device.name, "ahci1p2") == 0, "named after its entry: ahci1p2");
    const partition::DiskInfo found = partition::disk_info(disk);
    check(found.scheme == partition::Scheme::gpt && !found.gpt_from_backup, "a clean GPT");
    check(memcmp(found.disk_guid, table.disk_guid, 16) == 0, "the same disk GUID");
}

// Disk Management's flow: a blank disk is initialized and partitioned while
// the system runs, and rescan makes the partitions appear; deleting one and
// rescanning again frees its slot, which the next partition reuses.
void case_rescan_follows_the_disk() {
    printf("case: rescan after the table changes\n");
    reset_devices();
    size_t other = add_disk(131072, "ata0", /*writable=*/true);
    build_gpt(other, /*protective=*/true);
    size_t disk = add_disk(131072, "ahci1", /*writable=*/true);
    enumerate_partitions();
    check(g_device_count == 4 && partition::disk_info(disk).scheme == partition::Scheme::none,
          "the blank disk has no table and no partitions");

    const sxdisk_io io = {&disk, &sxdisk_read_disk, &sxdisk_write_disk, &sxdisk_fill};
    static sxdisk_gpt table;
    sxdisk_gpt_init(&table, 131072, &io);
    sxdisk_gpt_add(&table, 2048, 4096, sxdisk_type_sxfs, "A", &io, nullptr);
    sxdisk_gpt_add(&table, 8192, 4096, sxdisk_type_sxfs, "B", &io, nullptr);
    sxdisk_gpt_write(&table, &io);
    check(partition::rescan(disk), "rescan the disk");
    check(partition::disk_info(disk).scheme == partition::Scheme::gpt, "it reads as GPT now");

    size_t found = 0;
    size_t b_index = 0;
    partition::Info info = {};
    for (size_t index = 0; index < g_device_count; ++index) {
        if (partition::info(index, info) && info.parent_index == disk) {
            ++found;
            if (strcmp(info.name, "B") == 0) {
                b_index = index;
            }
        }
    }
    check(found == 2, "both new partitions are sliced");
    check(!partition::rescan(b_index), "a partition cannot be rescanned");

    sxdisk_gpt_remove(&table, 1);
    sxdisk_gpt_write(&table, &io);
    check(partition::rescan(disk), "rescan after deleting A");
    found = 0;
    for (size_t index = 0; index < g_device_count; ++index) {
        if (partition::info(index, info) && info.parent_index == disk) {
            ++found;
            check(strcmp(info.name, "B") == 0 && info.number == 2, "B is left, still entry 2");
        }
    }
    check(found == 1, "one partition left");
    partition::Info first = {};
    check(partition::info(2, first) && first.parent_index == other, "the other disk's partitions are untouched");
}

} // namespace

// --- block:: ----------------------------------------------------------------

namespace block {

size_t register_device(const DeviceOps& ops, void* context, uint32_t sector_count, bool writable, const char* name,
    const DeviceIdentity*) {
    if (ops.read == nullptr || sector_count == 0) {
        return kInvalidDevice;
    }
    // The first hole, as the kernel's core does.
    size_t slot = 0;
    while (slot < kMaxDevices && g_devices[slot].present) {
        ++slot;
    }
    if (slot >= kMaxDevices) {
        return kInvalidDevice;
    }
    Device& device = g_devices[slot];
    device = {};
    device.present = true;
    device.ops = &ops;
    device.context = context;
    device.sector_count = sector_count;
    device.writable = writable && ops.write != nullptr;
    device.name = name;
    if (slot + 1 > g_device_count) {
        g_device_count = slot + 1;
    }
    return slot;
}

bool unregister_device(size_t index) {
    if (index >= g_device_count || !g_devices[index].present || g_devices[index].ops == nullptr) {
        return false;
    }
    g_devices[index] = {};
    return true;
}

size_t device_count() {
    return g_device_count;
}

bool device_info(size_t index, DeviceInfo& info) {
    if (index >= g_device_count || !g_devices[index].present) {
        return false;
    }
    info.present = true;
    info.id = static_cast<uint32_t>(index + 1);
    info.sector_count = g_devices[index].sector_count;
    info.writable = g_devices[index].writable;
    info.name = g_devices[index].name;
    info.identity = nullptr;
    return true;
}

bool read(size_t index, uint32_t lba, uint32_t sector_count, void* buffer) {
    if (buffer == nullptr || !range_ok(index, lba, sector_count)) {
        return false;
    }
    Device& device = g_devices[index];
    if (device.ops != nullptr) {
        return device.ops->read(device.context, lba, sector_count, buffer);
    }
    memcpy(buffer, device.image + byte_offset(lba), byte_length(sector_count));
    return true;
}

bool write(size_t index, uint32_t lba, uint32_t sector_count, const void* buffer) {
    if (buffer == nullptr || !range_ok(index, lba, sector_count)) {
        return false;
    }
    Device& device = g_devices[index];
    if (!device.writable) {
        return false;
    }
    if (device.ops != nullptr) {
        return device.ops->write(device.context, lba, sector_count, buffer);
    }
    memcpy(device.image + byte_offset(lba), buffer, byte_length(sector_count));
    return true;
}

// The in-memory disks have no cache: a cached write is a write, and flushing
// a partition only has to reach its parent.
bool write_cached(size_t index, uint32_t lba, uint32_t sector_count, const void* buffer) {
    return write(index, lba, sector_count, buffer);
}

bool flush(size_t index) {
    if (index >= g_device_count || !g_devices[index].writable) {
        return false;
    }
    Device& device = g_devices[index];
    return device.ops == nullptr || device.ops->flush == nullptr || device.ops->flush(device.context);
}

bool ready() {
    return g_device_count != 0;
}

bool register_driver(const Driver&) {
    return true;
}

size_t probe_all() {
    return g_device_count;
}

} // namespace block

int main() {
    printf("PARTITION TEST START\n");

    case_gpt_publishes_partitions();
    case_partition_offsets_reach_the_parent();
    case_gpt_without_protective_mbr();
    case_mbr_table();
    case_fat_volume_is_not_a_table();
    case_read_only_disk_yields_read_only_partitions();
    case_raw_disk_is_left_alone();
    case_bogus_entries_are_discarded();
    case_two_disks_do_not_cross();
    case_gpt_metadata();
    case_damaged_primary_uses_backup();
    case_damaged_entries_use_backup();
    case_both_gpt_copies_damaged();
    case_disk_schemes();
    case_libsxdisk_tables_are_sliced();
    case_rescan_follows_the_disk();

    reset_devices();

    printf("%s (%d checks, %d failures)\n",
           g_failures == 0 ? "PARTITION TEST PASS" : "PARTITION TEST FAIL",
           g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
