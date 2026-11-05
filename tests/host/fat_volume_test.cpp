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
 *   - everything is the system's, 0755/0644, never an execute bit;
 *   - writes: create, write, a hole filled with zeros, truncate both ways,
 *     mkdir, rename of a directory with files below it, unlink (an open
 *     vnode then reads nothing), rmdir, and all of it still there after an
 *     unmount and a new mount;
 *   - FAT's read-only bit makes a file read-only, a full volume writes what
 *     fits, and a read-only device mounts read-only with every write refused;
 *   - a volume mounted for someone is theirs, found and created alike;
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
        check(conf->writable, "a file on a writable device is writable");
        check(conf->uid == 0 && conf->gid == 0 && conf->mode == (vfs::kModeFile | 0644),
              "owned by the system, 0644, no execute bit");
        check(conf->mtime == (1700000000u & ~1u), "mtime comes from the FAT date (2-second steps)");
    }

    vfs::Vnode* directory = hoststub::find_node("/boot/a/b");
    check(directory != nullptr && directory->type == vfs::NodeType::directory, "nested directories");
    check(directory != nullptr && directory->mode == (vfs::kModeDirectory | 0755), "a directory is 0755");

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
        check(big->size == kBigFileBytes && big->writable, "refresh keeps size and writability");
    }

    fs::VolumeStats stats = {};
    check(driver.stats(id, stats), "stats");
    check(stats.writable && stats.total_bytes > 0 && stats.used_bytes > 0 &&
              stats.used_bytes + stats.free_bytes == stats.total_bytes,
          "stats add up and say writable");
    check(stats.file_count == 4, "four files published (the long name left out)");
    check(driver.sync(id), "sync");

    driver.unmount(id);
    check(!fat_disk::bound(static_cast<uint8_t>(id)), "unmount unbinds the drive");
    hoststub::detach_device();
    free(image);
}

const vfs::Ownership kNewFile = {0, 0, vfs::kModeFile | 0644};
const vfs::Ownership kNewDirectory = {0, 0, vfs::kModeDirectory | 0755};

bool node_reads(vfs::Vnode* node, const char* expected, size_t length) {
    char buffer[64] = {};
    return node != nullptr && node->size == length && length < sizeof(buffer) &&
        (length == 0 || read_node(node, 0, buffer, length)) && memcmp(buffer, expected, length) == 0;
}

bool node_is_zero(vfs::Vnode* node, size_t from, size_t to) {
    static uint8_t buffer[16384];
    if (node == nullptr || to - from > sizeof(buffer) || !read_node(node, from, buffer, to - from)) {
        return false;
    }
    for (size_t index = 0; index < to - from; ++index) {
        if (buffer[index] != 0) {
            return false;
        }
    }
    return true;
}

bool write_node(vfs::Vnode* node, size_t offset, const char* text, bool truncate) {
    size_t written = 0;
    const size_t length = strlen(text);
    return node != nullptr && fat::driver().write(*node, offset, text, length, truncate, written) && written == length;
}

void writes_round_trip() {
    printf("fat-volume-test: writes\n");
    constexpr uint32_t kSectors = 65536;
    uint8_t* image = static_cast<uint8_t*>(malloc(static_cast<size_t>(kSectors) * kSector));
    build_image(image, kSectors, FM_FAT, 0);
    const fs::Driver& driver = fat::driver();
    hoststub::reset_vfs();
    fs::VolumeId id = driver.probe(0, "/mnt/w");
    check(id != fs::kInvalidVolume && driver.attach(id), "mount a writable volume");

    vfs::Vnode* file = driver.create_file(id, "/mnt/w/new.txt", kNewFile);
    check(file != nullptr && file->writable && file->size == 0 && file->mode == (vfs::kModeFile | 0644),
          "create a file: empty, writable, 0644");
    check(driver.create_file(id, "/mnt/w/new.txt", kNewFile) == nullptr, "creating it again refuses");
    check(write_node(file, 0, "hello", false) && node_reads(file, "hello", 5), "write and read back");
    check(write_node(file, 10000, "X", false) && file != nullptr && file->size == 10001, "a write past the end");
    check(node_is_zero(file, 5, 10000), "the hole reads as zeros");
    check(write_node(file, 0, "abc", true) && node_reads(file, "abc", 3), "a truncating write starts over");
    check(file != nullptr && driver.truncate(*file, 1) && node_reads(file, "a", 1), "truncate shrinks");
    check(file != nullptr && driver.truncate(*file, 3000) && file->size == 3000 && node_is_zero(file, 1, 3000),
          "truncate grows with zeros");

    check(driver.create_directory(id, "/mnt/w/dir", kNewDirectory) != nullptr, "mkdir");
    vfs::Vnode* sub = driver.create_directory(id, "/mnt/w/dir/sub", kNewDirectory);
    check(sub != nullptr && sub->mode == (vfs::kModeDirectory | 0755), "a nested mkdir, 0755");
    vfs::Vnode* inner = driver.create_file(id, "/mnt/w/dir/inner.txt", kNewFile);
    vfs::Vnode* deep = driver.create_file(id, "/mnt/w/dir/sub/deep.txt", kNewFile);
    check(write_node(inner, 0, "inner", false) && write_node(deep, 0, "deep", false), "files below the directory");

    check(driver.rename(id, "/mnt/w/dir", "/mnt/w/moved"), "rename the directory");
    check(node_reads(inner, "inner", 5) && node_reads(deep, "deep", 4), "its files read through their new path");
    check(write_node(deep, 4, "er", false) && node_reads(deep, "deeper", 6), "and write through it");
    check(!driver.rename(id, "/mnt/w/new.txt", "/mnt/w/moved/inner.txt"), "a rename onto a file refuses");
    check(!driver.rename(id, "/mnt/w/moved", "/mnt/w/moved/sub/x"), "a directory into itself refuses");

    check(inner != nullptr && driver.unlink(*inner), "unlink");
    char buffer[8] = {};
    check(inner != nullptr && inner->data == nullptr && !read_node(inner, 0, buffer, 1),
          "an unlinked vnode still open reads nothing");
    check(deep != nullptr && driver.unlink(*deep), "unlink the deep file");
    check(sub != nullptr && driver.remove_directory(*sub), "rmdir the emptied directory");

    vfs::Vnode* locked = driver.create_file(id, "/mnt/w/locked.txt", kNewFile);
    check(write_node(locked, 0, "keep", false), "write a file to lock");
    char locked_path[16] = {};
    snprintf(locked_path, sizeof(locked_path), "%d:/locked.txt", static_cast<int>(id));
    check(f_chmod(locked_path, AM_RDO, AM_RDO) == FR_OK, "set FAT's read-only bit");
    if (locked != nullptr) {
        driver.refresh(*locked);
        check(!locked->writable && locked->mode == (vfs::kModeFile | 0444), "it is published read-only");
        check(!write_node(locked, 0, "x", false), "and writing it refuses");
    }

    fs::VolumeStats stats = {};
    check(driver.stats(id, stats) && stats.file_count == 6, "the file count follows creates and unlinks");
    check(driver.sync(id), "sync");
    driver.unmount(id);

    // A new mount reads what the first one wrote.
    hoststub::reset_vfs();
    id = driver.probe(0, "/mnt/w");
    check(id != fs::kInvalidVolume && driver.attach(id), "mount it again");
    vfs::Vnode* again = hoststub::find_node("/mnt/w/new.txt");
    check(again != nullptr && again->size == 3000 && node_is_zero(again, 1, 3000), "new.txt is still there");
    check(hoststub::find_node("/mnt/w/moved") != nullptr && hoststub::find_node("/mnt/w/dir") == nullptr,
          "the rename is on the disk");
    check(hoststub::find_node("/mnt/w/moved/inner.txt") == nullptr, "the unlink is on the disk");
    vfs::Vnode* locked_again = hoststub::find_node("/mnt/w/locked.txt");
    check(locked_again != nullptr && !locked_again->writable, "the read-only bit is on the disk");
    driver.unmount(id);
    hoststub::detach_device();
    free(image);
}

void full_volume_writes_what_fits() {
    printf("fat-volume-test: a full volume\n");
    constexpr uint32_t kSectors = 1024;
    uint8_t* image = static_cast<uint8_t*>(malloc(static_cast<size_t>(kSectors) * kSector));
    build_image(image, kSectors, FM_FAT, 0);
    const fs::Driver& driver = fat::driver();
    hoststub::reset_vfs();
    const fs::VolumeId id = driver.probe(0, "/mnt/full");
    check(id != fs::kInvalidVolume && driver.attach(id), "mount a small volume");
    vfs::Vnode* file = driver.create_file(id, "/mnt/full/fill", kNewFile);
    static uint8_t data[1u << 20];
    size_t written = 0;
    check(file != nullptr && driver.write(*file, 0, data, sizeof(data), false, written), "a write larger than the volume");
    check(written > 0 && written < sizeof(data) && file != nullptr && file->size == written, "writes what fits");
    check(file != nullptr && !driver.write(*file, written, data, 4096, false, written) && written == 0,
          "then nothing more fits");
    driver.unmount(id);
    hoststub::detach_device();
    free(image);
}

void read_only_device() {
    printf("fat-volume-test: a read-only device\n");
    constexpr uint32_t kSectors = 4096;
    uint8_t* image = static_cast<uint8_t*>(malloc(static_cast<size_t>(kSectors) * kSector));
    build_image(image, kSectors, FM_FAT, 0);
    hoststub::attach_device(image, kSectors, false);
    const fs::Driver& driver = fat::driver();
    hoststub::reset_vfs();
    const fs::VolumeId id = driver.probe(0, "/boot");
    check(id != fs::kInvalidVolume && driver.attach(id), "mount it");
    vfs::Vnode* conf = hoststub::find_node("/boot/boot/limine.conf");
    check(conf != nullptr && !conf->writable && conf->mode == (vfs::kModeFile | 0444), "files are 0444, read-only");
    vfs::Vnode* directory = hoststub::find_node("/boot/a");
    check(directory != nullptr && directory->mode == (vfs::kModeDirectory | 0555), "directories are 0555");
    check(!write_node(conf, 0, "x", false), "write refuses");
    check(conf != nullptr && !driver.truncate(*conf, 0) && !driver.unlink(*conf), "truncate and unlink refuse");
    check(driver.create_file(id, "/boot/new", kNewFile) == nullptr, "create refuses");
    check(driver.create_directory(id, "/boot/newdir", kNewDirectory) == nullptr, "mkdir refuses");
    check(!driver.rename(id, "/boot/boot", "/boot/x"), "rename refuses");
    fs::VolumeStats stats = {};
    check(driver.stats(id, stats) && !stats.writable, "stats say read-only");
    check(hoststub::rejected_writes() == 0, "nothing even tried to write the device");
    driver.unmount(id);
    hoststub::detach_device();
    free(image);
}

// A volume mounted for someone (a stick mounted from Disk Management) is
// published as theirs: what attach finds, and what is created later.
void owner_is_adopted() {
    printf("fat-volume-test: an adopted owner\n");
    constexpr uint32_t kSectors = 4096;
    uint8_t* image = static_cast<uint8_t*>(malloc(static_cast<size_t>(kSectors) * kSector));
    build_image(image, kSectors, FM_FAT, 0);
    const fs::Driver& driver = fat::driver();
    hoststub::reset_vfs();
    const fs::VolumeId id = driver.probe(0, "/mnt/stick");
    check(id != fs::kInvalidVolume, "probe");
    check(driver.adopt_owner != nullptr, "FAT adopts an owner");
    driver.adopt_owner(id, {1000, 100});
    check(driver.attach(id), "attach");
    vfs::Vnode* root = hoststub::find_node("/mnt/stick");
    vfs::Vnode* conf = hoststub::find_node("/mnt/stick/boot/limine.conf");
    check(root != nullptr && root->uid == 1000 && root->gid == 100, "the mount point is the owner's");
    check(conf != nullptr && conf->uid == 1000 && conf->gid == 100 && conf->mode == (vfs::kModeFile | 0644),
          "a file found at attach is the owner's, 0644");
    vfs::Vnode* created = driver.create_file(id, "/mnt/stick/new.txt", kNewFile);
    check(created != nullptr && created->uid == 1000 && created->gid == 100, "a created file is the owner's");
    if (conf != nullptr) {
        driver.refresh(*conf);
        check(conf->uid == 1000, "refresh keeps the owner");
    }
    driver.unmount(id);

    // The next volume in that slot starts as the system's again.
    hoststub::reset_vfs();
    const fs::VolumeId again = driver.probe(0, "/mnt/stick");
    check(again != fs::kInvalidVolume && driver.attach(again), "mount it again without an owner");
    vfs::Vnode* plain = hoststub::find_node("/mnt/stick/boot/limine.conf");
    check(plain != nullptr && plain->uid == 0 && plain->gid == 0, "without an owner, the system's");
    driver.unmount(again);
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
    writes_round_trip();
    full_volume_writes_what_fits();
    read_only_device();
    owner_is_adopted();
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
