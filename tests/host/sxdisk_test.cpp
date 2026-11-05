/*
 * sxdisk_test.cpp -- host test of libsxdisk (include/sxdisk/sxdisk.h) and of
 * sxfs_mkfs, over an in-memory disk.
 *
 * The other half of the contract, that kernel/partition.cpp slices what this
 * library writes, is in partition_test.cpp, which has the fake block:: the
 * kernel reader needs.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sxdisk/sxdisk.h"
#include "shared/crc32.h"

extern "C" {
#include "sxfs_core.h"
}

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

struct Disk {
    uint8_t* bytes;
    uint64_t sectors;
    uint32_t writes;
    uint32_t max_transfer; // sectors; 0 = unlimited
};

int disk_read(void* cookie, uint64_t lba, uint32_t count, void* buffer) {
    Disk& disk = *static_cast<Disk*>(cookie);
    if (lba + count > disk.sectors) {
        return -1;
    }
    memcpy(buffer, disk.bytes + lba * SXDISK_SECTOR_SIZE, static_cast<size_t>(count) * SXDISK_SECTOR_SIZE);
    return 0;
}

int disk_write(void* cookie, uint64_t lba, uint32_t count, const void* buffer) {
    Disk& disk = *static_cast<Disk*>(cookie);
    if (lba + count > disk.sectors || (disk.max_transfer != 0 && count > disk.max_transfer)) {
        return -1;
    }
    memcpy(disk.bytes + lba * SXDISK_SECTOR_SIZE, buffer, static_cast<size_t>(count) * SXDISK_SECTOR_SIZE);
    ++disk.writes;
    return 0;
}

uint32_t g_seed = 0x12345678u;

void fake_random(void*, void* out, size_t size) {
    uint8_t* bytes = static_cast<uint8_t*>(out);
    for (size_t index = 0; index < size; ++index) {
        g_seed = g_seed * 1103515245u + 12345u;
        bytes[index] = static_cast<uint8_t>(g_seed >> 16);
    }
}

Disk make_disk(uint64_t sectors) {
    Disk disk = {};
    disk.sectors = sectors;
    disk.bytes = static_cast<uint8_t*>(calloc(sectors, SXDISK_SECTOR_SIZE));
    return disk;
}

sxdisk_io io_for(Disk& disk) {
    return sxdisk_io{&disk, &disk_read, &disk_write, &fake_random};
}

constexpr uint64_t kDiskSectors = 262144; // 128 MiB

void case_init_write_read() {
    printf("case: an empty table round-trips\n");
    Disk disk = make_disk(kDiskSectors);
    const sxdisk_io io = io_for(disk);
    static sxdisk_gpt table;
    static sxdisk_gpt loaded;

    check(sxdisk_gpt_init(&table, kDiskSectors, &io) == SXDISK_OK, "init");
    check(table.first_usable == 34 && table.last_usable == kDiskSectors - 34, "usable range of a 128-entry GPT");
    check((table.disk_guid[7] >> 4) == 4 && (table.disk_guid[8] & 0xc0) == 0x80, "the disk GUID is version 4");
    check(sxdisk_gpt_write(&table, &io) == SXDISK_OK, "write");

    check(disk.bytes[510] == 0x55 && disk.bytes[511] == 0xaa, "boot signature");
    check(disk.bytes[446 + 4] == 0xee, "protective MBR entry");
    check(memcmp(disk.bytes + 512, "EFI PART", 8) == 0, "primary header");
    check(memcmp(disk.bytes + (kDiskSectors - 1) * 512, "EFI PART", 8) == 0, "backup header on the last sector");

    check(sxdisk_gpt_read(&loaded, kDiskSectors, &io) == SXDISK_OK, "read back");
    check(!loaded.from_backup, "from the primary");
    check(memcmp(loaded.disk_guid, table.disk_guid, 16) == 0, "same disk GUID");

    struct sxdisk_extent extents[4];
    const size_t count = sxdisk_gpt_free_extents(&loaded, extents, 4);
    check(count == 1 && extents[0].first_lba == 2048 && extents[0].sector_count == kDiskSectors - 34 - 2048 + 1,
          "one free extent from 1 MiB to the last usable sector");
    free(disk.bytes);
}

void case_add_remove_and_free_space() {
    printf("case: adding and removing partitions\n");
    Disk disk = make_disk(kDiskSectors);
    const sxdisk_io io = io_for(disk);
    static sxdisk_gpt table;
    sxdisk_gpt_init(&table, kDiskSectors, &io);

    uint32_t first = 0;
    uint32_t second = 0;
    check(sxdisk_gpt_add(&table, 2048, 65536, sxdisk_type_sxfs, "Data", &io, &first) == SXDISK_OK && first == 1,
          "the first partition takes entry 1");
    check(sxdisk_gpt_add(&table, 2048 + 65536, 32768, sxdisk_type_basic_data, nullptr, &io, &second) == SXDISK_OK &&
          second == 2, "the second takes entry 2");
    check(sxdisk_gpt_add(&table, 4096, 2048, sxdisk_type_sxfs, "x", &io, nullptr) == SXDISK_ERR_NO_SPACE,
          "an overlapping range is refused");
    check(sxdisk_gpt_add(&table, 20, 2048, sxdisk_type_sxfs, "x", &io, nullptr) == SXDISK_ERR_NO_SPACE,
          "a range over the table is refused");
    check(sxdisk_gpt_add(&table, kDiskSectors - 40, 2048, sxdisk_type_sxfs, "x", &io, nullptr) == SXDISK_ERR_NO_SPACE,
          "a range over the backup is refused");

    char name[40];
    sxdisk_entry_get_name(sxdisk_gpt_entry_at(&table, 1), name, sizeof(name));
    check(strcmp(name, "Data") == 0, "the name reads back");
    check(memcmp(table.entries[0].unique_guid, table.entries[1].unique_guid, 16) != 0, "unique GUIDs differ");

    struct sxdisk_extent extents[4];
    size_t count = sxdisk_gpt_free_extents(&table, extents, 4);
    check(count == 1 && extents[0].first_lba == 2048 + 65536 + 32768, "the free space follows the second");

    check(sxdisk_gpt_remove(&table, 1) == SXDISK_OK, "remove entry 1");
    check(sxdisk_gpt_remove(&table, 1) == SXDISK_ERR_NOT_FOUND, "removing it twice fails");
    count = sxdisk_gpt_free_extents(&table, extents, 4);
    check(count == 2 && extents[0].first_lba == 2048 && extents[0].sector_count == 65536,
          "its range is free again, before the second");

    uint32_t again = 0;
    check(sxdisk_gpt_add(&table, 2048, 4096, sxdisk_type_linux, "L", &io, &again) == SXDISK_OK && again == 1,
          "a new partition reuses entry 1");
    count = sxdisk_gpt_free_extents(&table, extents, 4);
    check(count == 2 && extents[0].first_lba == 6144, "a gap starts aligned after the small one");
    free(disk.bytes);
}

void case_backup_repairs_primary() {
    printf("case: a damaged primary is read from the backup and repaired\n");
    Disk disk = make_disk(kDiskSectors);
    const sxdisk_io io = io_for(disk);
    static sxdisk_gpt table;
    static sxdisk_gpt loaded;
    sxdisk_gpt_init(&table, kDiskSectors, &io);
    sxdisk_gpt_add(&table, 2048, 4096, sxdisk_type_sxfs, "A", &io, nullptr);
    sxdisk_gpt_write(&table, &io);

    disk.bytes[2 * 512 + 3] ^= 0xff; // the primary entry array
    check(sxdisk_gpt_read(&loaded, kDiskSectors, &io) == SXDISK_OK && loaded.from_backup, "read from the backup");
    check(sxdisk_gpt_entry_at(&loaded, 1) != nullptr && loaded.entries[0].first_lba == 2048, "with the partition");

    check(sxdisk_gpt_write(&loaded, &io) == SXDISK_OK, "write it again");
    check(sxdisk_gpt_read(&loaded, kDiskSectors, &io) == SXDISK_OK && !loaded.from_backup, "the primary is valid again");

    disk.bytes[512 + 20] ^= 0xff;
    disk.bytes[(kDiskSectors - 1) * 512 + 20] ^= 0xff;
    check(sxdisk_gpt_read(&loaded, kDiskSectors, &io) == SXDISK_ERR_INVALID, "both copies damaged is invalid");

    memset(disk.bytes, 0, 4096);
    check(sxdisk_gpt_read(&loaded, kDiskSectors, &io) == SXDISK_ERR_INVALID, "a blank disk has no table");
    free(disk.bytes);
}

void case_keeps_boot_code() {
    printf("case: the protective MBR keeps boot code\n");
    Disk disk = make_disk(kDiskSectors);
    const sxdisk_io io = io_for(disk);
    static sxdisk_gpt table;
    memset(disk.bytes, 0x90, 440);
    disk.bytes[446 + 4] = 0x83; // an old MBR entry, to be replaced
    sxdisk_gpt_init(&table, kDiskSectors, &io);
    sxdisk_gpt_write(&table, &io);
    check(disk.bytes[0] == 0x90 && disk.bytes[439] == 0x90, "the first 440 bytes survive");
    check(disk.bytes[446 + 4] == 0xee && disk.bytes[446 + 16 + 4] == 0, "the old table does not");
    free(disk.bytes);
}

void case_names_and_types() {
    printf("case: type names and GUID text\n");
    char text[40];
    sxdisk_guid_format(sxdisk_type_esp, text);
    check(strcmp(text, "C12A7328-F81F-11D2-BA4B-00A0C93EC93B") == 0, "ESP GUID as text");
    sxdisk_guid_format(sxdisk_type_sxfs, text);
    check(strcmp(text, "277C8079-900E-477E-99D8-4FEE1496D370") == 0, "SxFS GUID as text");
    check(strcmp(sxdisk_type_name(sxdisk_type_sxfs), "SavanXP SxFS") == 0, "SxFS type name");
    check(strcmp(sxdisk_mbr_type_name(0x0c), "FAT32") == 0, "MBR type name");

    sxdisk_gpt_entry entry = {};
    sxdisk_entry_set_name(&entry, "A name longer than thirty-six characters, cut");
    sxdisk_entry_get_name(&entry, text, sizeof(text));
    check(strlen(text) == 36, "names are cut at 36 units");
}

// sxfs_mkfs over a partition-sized device whose writes are capped at 128
// sectors, as the storage calls are: the caller splits, the core does not
// care.
struct Split {
    Disk* disk;
    uint64_t offset;
};

int split_read(void* cookie, uint32_t lba, uint32_t count, void* buffer) {
    Split& split = *static_cast<Split*>(cookie);
    return disk_read(split.disk, split.offset + lba, count, buffer);
}

int split_write(void* cookie, uint32_t lba, uint32_t count, const void* buffer) {
    Split& split = *static_cast<Split*>(cookie);
    const uint8_t* bytes = static_cast<const uint8_t*>(buffer);
    while (count > 0) {
        const uint32_t chunk = count > 128 ? 128 : count;
        if (disk_write(split.disk, split.offset + lba, chunk, bytes) != 0) {
            return -1;
        }
        lba += chunk;
        count -= chunk;
        bytes += static_cast<size_t>(chunk) * SXDISK_SECTOR_SIZE;
    }
    return 0;
}

void case_mkfs() {
    printf("case: sxfs_mkfs\n");
    Disk disk = make_disk(kDiskSectors);
    disk.max_transfer = 128;
    Split split = {&disk, 2048};
    static sxfs_ctx ctx;

    // A stale journal from an earlier volume, with a transaction pending.
    memset(disk.bytes + (2048 + SXFS_JOURNAL_LBA) * 512, 0x5a, 512);

    sxfs_ctx_init(&ctx, &split, &split_read, &split_write);
    check(sxfs_mkfs(&ctx, 65536, 1000, 1000, 040755) == SXFS_OK, "mkfs of 32 MiB, root owned by uid 1000");

    const uint8_t* journal = disk.bytes + (2048 + SXFS_JOURNAL_LBA) * 512;
    bool zero = true;
    for (size_t index = 0; index < 512; ++index) {
        zero = zero && journal[index] == 0;
    }
    check(zero, "the journal header is zeroed");

    static sxfs_ctx reopened;
    sxfs_ctx_init(&reopened, &split, &split_read, &split_write);
    check(sxfs_open(&reopened) == SXFS_OK, "the volume opens");
    uint32_t uid = 0;
    uint32_t gid = 0;
    uint16_t mode = 0;
    check(sxfs_get_ownership(&reopened, SXFS_ROOT_INODE, &uid, &gid, &mode) == SXFS_OK &&
          uid == 1000 && gid == 1000 && mode == 040755, "the root keeps its owner and mode");
    check(sxfs_write_file(&reopened, "hello.txt", "hi", 2) == SXFS_OK && sxfs_flush(&reopened) == SXFS_OK,
          "a file can be written on it");

    static sxfs_ctx bad;
    sxfs_ctx_init(&bad, &split, &split_read, &split_write);
    check(sxfs_mkfs(&bad, 65536, 0, 0, 0100644) == SXFS_ERR_INVALID, "a file mode for the root is refused");
    check(sxfs_mkfs(&bad, SXFS_MAX_TOTAL_SECTORS + 1, 0, 0, 0) == SXFS_ERR_INVALID, "past the format's size is refused");
    free(disk.bytes);
}

} // namespace

int main() {
    printf("SXDISK TEST START\n");
    case_init_write_read();
    case_add_remove_and_free_space();
    case_backup_repairs_primary();
    case_keeps_boot_code();
    case_names_and_types();
    case_mkfs();
    printf("%s (%d checks, %d failures)\n", g_failures == 0 ? "SXDISK TEST PASS" : "SXDISK TEST FAIL",
           g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
