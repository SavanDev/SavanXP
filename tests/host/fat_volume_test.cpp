/*
 * fat_volume_test.cpp -- the FAT driver (kernel/fat.cpp over FatFs and
 * kernel/fat_diskio.cpp), run on the host against images in memory.
 *
 * Each image is formatted and filled with FatFs itself, through the same disk
 * layer the kernel uses (a drive bound by hand), and then mounted by the
 * driver at /boot, the way the boot volume is.
 *
 * What each case pins:
 *   - FAT12, FAT16 and FAT32 mount and read back, long UTF-8 names included,
 *     and a read at any offset of a file spanning many clusters is exact;
 *   - everything is the system's, read-only, without execute bits, and the
 *     write operations refuse;
 *   - a name the VFS cannot hold is left out, not truncated;
 *   - a partitioned disk is never claimed as a whole, although FatFs alone
 *     would mount its first partition through it;
 *   - a tree larger than the VFS fails attach cleanly, and unmount frees the
 *     slot for the next volume;
 *   - FAT dates convert to seconds and back.
 */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

#include "kernel/block.hpp"
#include "kernel/console.hpp"
#include "kernel/fat.hpp"
#include "kernel/fat_diskio.hpp"
#include "kernel/fs.hpp"
#include "kernel/heap.hpp"
#include "kernel/vfs.hpp"
#include "sxfs_host_stubs.hpp"

#include "ff.h"

extern "C" int memcmp(const void*, const void*, size_t);
extern "C" void* memset(void*, int, size_t);
extern "C" void* memcpy(void*, const void*, size_t);
extern "C" size_t strlen(const char*);

// --- Kernel services the driver uses that the shared stubs do not provide ---

namespace heap {
void* allocate(size_t size, size_t) { return malloc(size); }
void free(void* address) { ::free(address); }
} // namespace heap

namespace console {
void vprintf(const char* format, va_list args) { ::vprintf(format, args); }
void printf(const char* format, ...) {
    va_list args;
    va_start(args, format);
    ::vprintf(format, args);
    va_end(args);
}
} // namespace console

namespace block {
bool sync(size_t) { return true; }
} // namespace block

namespace {

int g_failures = 0;

void check(bool condition, const char* what) {
    if (!condition) {
        printf("FAIL: %s\n", what);
        ++g_failures;
    }
}

constexpr uint32_t kSector = 512;
// The drive the test formats and fills images through; the driver takes the
// lowest free one, so they never meet.
constexpr uint8_t kBuildDrive = fat_disk::kDrives - 1;
constexpr const char* kLongUtf8Name = "Ñandú con un nombre largo.txt";
// 70 characters: more than a VFS path component holds.
constexpr const char* kTooLongName =
    "this-name-is-far-too-long-for-a-vfs-path-component-of-sixty-four.txt";
constexpr const char* kLimineText = "timeout: 0\nserial: yes\n";
constexpr size_t kBigFileBytes = 300 * 1024 + 123;

uint8_t pattern_at(size_t offset) {
    return static_cast<uint8_t>((offset * 131u + (offset >> 9)) & 0xff);
}

bool write_file(const char* path, const void* data, size_t size) {
    FIL file = {};
    UINT done = 0;
    if (f_open(&file, path, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK) {
        return false;
    }
    const bool ok = f_write(&file, data, static_cast<UINT>(size), &done) == FR_OK && done == size;
    return f_close(&file) == FR_OK && ok;
}

// Formats `image` with FatFs and fills it. Returns the FAT type FatFs chose.
int build_image(uint8_t* image, uint32_t sectors, BYTE format, size_t extra_root_files) {
    memset(image, 0, static_cast<size_t>(sectors) * kSector);
    hoststub::attach_device(image, sectors, true);
    check(fat_disk::bind(kBuildDrive, 0), "bind the build drive");

    char root[3] = {static_cast<char>('0' + kBuildDrive), ':', 0};
    static uint8_t work[4096];
    const MKFS_PARM options = {static_cast<BYTE>(format | FM_SFD), 0, 0, 0, 0};
    check(f_mkfs(root, &options, work, sizeof(work)) == FR_OK, "f_mkfs");

    FATFS fs = {};
    check(f_mount(&fs, root, 1) == FR_OK, "mount the new image");
    const int type = fs.fs_type;

    char path[300];
    snprintf(path, sizeof(path), "%s/boot", root);
    check(f_mkdir(path) == FR_OK, "mkdir boot");
    snprintf(path, sizeof(path), "%s/boot/limine.conf", root);
    check(write_file(path, kLimineText, strlen(kLimineText)), "write boot/limine.conf");
    snprintf(path, sizeof(path), "%s/%s", root, kLongUtf8Name);
    check(write_file(path, "utf-8\n", 6), "write the long UTF-8 name");
    snprintf(path, sizeof(path), "%s/%s", root, kTooLongName);
    check(write_file(path, "x", 1), "write the name too long for the VFS");
    snprintf(path, sizeof(path), "%s/a", root);
    check(f_mkdir(path) == FR_OK, "mkdir a");
    snprintf(path, sizeof(path), "%s/a/b", root);
    check(f_mkdir(path) == FR_OK, "mkdir a/b");
    snprintf(path, sizeof(path), "%s/a/b/empty", root);
    check(write_file(path, "", 0), "write an empty file");

    uint8_t* big = static_cast<uint8_t*>(malloc(kBigFileBytes));
    for (size_t index = 0; index < kBigFileBytes; ++index) {
        big[index] = pattern_at(index);
    }
    snprintf(path, sizeof(path), "%s/a/b/big.bin", root);
    check(write_file(path, big, kBigFileBytes), "write the big file");
    free(big);

    for (size_t index = 0; index < extra_root_files; ++index) {
        snprintf(path, sizeof(path), "%s/f%zu", root, index);
        check(write_file(path, "", 0), "write a filler file");
    }

    check(f_mount(nullptr, root, 0) == FR_OK, "unmount the build drive");
    fat_disk::unbind(kBuildDrive);
    return type;
}

bool read_node(vfs::Vnode* node, size_t offset, void* buffer, size_t count) {
    return node != nullptr && fat::driver().read(*node, offset, buffer, count);
}

void mounted_volume_reads_back(const char* label, uint32_t sectors, BYTE format, int expected_type) {
    printf("fat-volume-test: %s\n", label);
    uint8_t* image = static_cast<uint8_t*>(malloc(static_cast<size_t>(sectors) * kSector));
    const int type = build_image(image, sectors, format, 0);
    check(type == expected_type, "FatFs chose the expected FAT type");

    const fs::Driver& driver = fat::driver();
    hoststub::reset_vfs();
    const fs::VolumeId id = driver.probe(0, "/boot");
    check(id != fs::kInvalidVolume, "probe claims the volume");
    check(driver.attach(id), "attach publishes the tree");

    vfs::Vnode* conf = hoststub::find_node("/boot/boot/limine.conf");
    check(conf != nullptr && conf->type == vfs::NodeType::file, "boot/limine.conf is published");
    if (conf != nullptr) {
        char text[64] = {};
        check(conf->size == strlen(kLimineText), "its size");
        check(read_node(conf, 0, text, conf->size) && memcmp(text, kLimineText, conf->size) == 0,
              "its contents read back");
        check(!conf->writable, "a file is published read-only");
        check(conf->uid == 0 && conf->gid == 0 && conf->mode == (vfs::kModeFile | 0444),
              "owned by the system, 0444, no execute bit");
        check(conf->mtime == (1700000000u & ~1u), "mtime comes from the FAT date (2-second steps)");
        size_t written = 1;
        check(!driver.write(*conf, 0, "x", 1, false, written) && written == 0, "write refuses");
        check(!driver.truncate(*conf, 0) && !driver.unlink(*conf), "truncate and unlink refuse");
    }
    check(driver.create_file(id, "/boot/new", {0, 0, vfs::kModeFile | 0644}) == nullptr, "create refuses");
    check(driver.create_directory(id, "/boot/newdir", {0, 0, vfs::kModeDirectory | 0755}) == nullptr,
          "mkdir refuses");
    check(!driver.rename(id, "/boot/boot", "/boot/x"), "rename refuses");

    vfs::Vnode* directory = hoststub::find_node("/boot/a/b");
    check(directory != nullptr && directory->type == vfs::NodeType::directory, "nested directories");
    check(directory != nullptr && directory->mode == (vfs::kModeDirectory | 0555), "a directory is 0555");

    char long_path[128];
    snprintf(long_path, sizeof(long_path), "/boot/%s", kLongUtf8Name);
    check(hoststub::find_node(long_path) != nullptr, "a long UTF-8 name is published as is");
    char too_long_path[128];
    snprintf(too_long_path, sizeof(too_long_path), "/boot/%s", kTooLongName);
    check(hoststub::find_node(too_long_path) == nullptr, "a name the VFS cannot hold is left out");

    vfs::Vnode* empty = hoststub::find_node("/boot/a/b/empty");
    check(empty != nullptr && empty->size == 0, "an empty file");

    vfs::Vnode* big = hoststub::find_node("/boot/a/b/big.bin");
    check(big != nullptr && big->size == kBigFileBytes, "the big file and its size");
    if (big != nullptr) {
        const size_t offsets[] = {0, 1, 511, 512, 4095, 123457, kBigFileBytes - 70000, kBigFileBytes - 1};
        static uint8_t buffer[70000];
        for (size_t offset : offsets) {
            const size_t count = offset + sizeof(buffer) <= kBigFileBytes ? sizeof(buffer) : kBigFileBytes - offset;
            bool exact = read_node(big, offset, buffer, count);
            for (size_t index = 0; exact && index < count; ++index) {
                exact = buffer[index] == pattern_at(offset + index);
            }
            check(exact, "a read at an offset is exact");
        }
        driver.refresh(*big);
        check(big->size == kBigFileBytes && !big->writable, "refresh keeps size and read-only");
    }

    fs::VolumeStats stats = {};
    check(driver.stats(id, stats), "stats");
    check(!stats.writable && stats.total_bytes > 0 && stats.used_bytes > 0 &&
              stats.used_bytes + stats.free_bytes == stats.total_bytes,
          "stats add up and say read-only");
    check(stats.file_count == 4, "four files published (the long name left out)");
    check(driver.sync(id), "sync of a clean volume");

    driver.unmount(id);
    check(!fat_disk::bound(static_cast<uint8_t>(id)), "unmount unbinds the drive");
    hoststub::detach_device();
    free(image);
}

void partitioned_disk_is_not_claimed() {
    printf("fat-volume-test: a partitioned disk\n");
    constexpr uint32_t kVolumeSectors = 8192;
    constexpr uint32_t kStart = 2048;
    const uint32_t sectors = kStart + kVolumeSectors;
    uint8_t* volume = static_cast<uint8_t*>(malloc(static_cast<size_t>(kVolumeSectors) * kSector));
    build_image(volume, kVolumeSectors, FM_FAT, 0);

    uint8_t* disk = static_cast<uint8_t*>(calloc(sectors, kSector));
    memcpy(disk + static_cast<size_t>(kStart) * kSector, volume, static_cast<size_t>(kVolumeSectors) * kSector);
    free(volume);
    // An MBR with one FAT16 partition at kStart.
    uint8_t* entry = disk + 446;
    entry[4] = 0x06;
    memcpy(entry + 8, &kStart, 4);
    memcpy(entry + 12, &kVolumeSectors, 4);
    disk[510] = 0x55;
    disk[511] = 0xaa;
    hoststub::attach_device(disk, sectors, true);

    // FatFs on its own would mount the partition through the whole disk.
    check(fat_disk::bind(kBuildDrive, 0), "bind the build drive to the disk");
    char root[3] = {static_cast<char>('0' + kBuildDrive), ':', 0};
    FATFS fs = {};
    check(f_mount(&fs, root, 1) == FR_OK, "FatFs alone finds the partition through the disk");
    (void)f_mount(nullptr, root, 0);
    fat_disk::unbind(kBuildDrive);

    hoststub::reset_vfs();
    check(fat::driver().probe(0, "/boot") == fs::kInvalidVolume, "the driver does not claim the whole disk");
    hoststub::detach_device();
    free(disk);
}

void tree_larger_than_the_vfs() {
    printf("fat-volume-test: a tree larger than the VFS\n");
    constexpr uint32_t kSectors = 16384;
    uint8_t* image = static_cast<uint8_t*>(malloc(static_cast<size_t>(kSectors) * kSector));
    // The host VFS holds 256 nodes.
    build_image(image, kSectors, FM_FAT, 300);

    const fs::Driver& driver = fat::driver();
    hoststub::reset_vfs();
    const fs::VolumeId id = driver.probe(0, "/mnt/big");
    check(id != fs::kInvalidVolume, "probe claims it");
    check(!driver.attach(id), "attach fails when the VFS is full");
    driver.unmount(id);

    hoststub::reset_vfs();
    const fs::VolumeId again = driver.probe(0, "/mnt/big");
    check(again == id, "the slot is free again after unmount");
    driver.unmount(again);
    hoststub::detach_device();
    free(image);
}

void dates_convert() {
    printf("fat-volume-test: dates\n");
    const uint64_t times[] = {315532800u /* 1980-01-01 */, 1700000000u, 951782400u /* 2000-02-29 */,
                              4354819199u /* 2107-12-31 23:59:59 */};
    for (uint64_t time : times) {
        const uint32_t packed = fat_disk::fat_time(time);
        check(fat_disk::epoch_seconds(static_cast<uint16_t>(packed >> 16), static_cast<uint16_t>(packed)) ==
                  (time & ~1ull),
              "a date round-trips (2-second steps)");
    }
    const uint32_t early = fat_disk::fat_time(0);
    check(early == ((1u << 5 | 1u) << 16), "before 1980 clamps to 1980-01-01");
    check(fat_disk::epoch_seconds(0, 0) == 0, "a zero date is no date");
}

} // namespace

int main() {
    mounted_volume_reads_back("FAT12", 4096, FM_FAT, FS_FAT12);
    mounted_volume_reads_back("FAT16", 65536, FM_FAT, FS_FAT16);
    mounted_volume_reads_back("FAT32", 131072, FM_FAT32, FS_FAT32);
    partitioned_disk_is_not_claimed();
    tree_larger_than_the_vfs();
    dates_convert();

    if (g_failures != 0) {
        printf("fat-volume-test: %d failure(s)\n", g_failures);
        return 1;
    }
    printf("fat-volume-test: ok\n");
    return 0;
}
