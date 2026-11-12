/*
 * fat_vfs_test.cpp -- the FAT driver under the real VFS and mount registry.
 *
 * fat_volume_test drives kernel/fat.cpp through a stand-in tree; this one
 * mounts a FatFs image at /boot through fs:: and reaches it only by paths, so
 * what the VFS does with the driver's answers is part of what is checked.
 *
 * What each case pins (VFS-C slice 4, docs/roadmaps/open/VFS_ROADMAP.md):
 *   - names inside the volume compare as FatFs compares them, case folded,
 *     ASCII and beyond; the tree above the mount point stays case-sensitive;
 *   - creating a name that exists in another case opens the existing file;
 *   - a new name keeps the directories' spelling on the disk, whatever the
 *     caller typed, and the caller's spelling of the new leaf;
 *   - a rename that only changes case is a rename, of a file and of a
 *     directory with files below it, and it is on the disk after a remount.
 */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

#include "abi/savanxp_abi.h"
#include "kernel/console.hpp"
#include "kernel/credentials.hpp"
#include "kernel/fat.hpp"
#include "kernel/fat_diskio.hpp"
#include "kernel/fs.hpp"
#include "kernel/heap.hpp"
#include "kernel/vfs.hpp"
#include "sxfs_host_stubs.hpp"

#include "ff.h"

extern "C" int memcmp(const void*, const void*, size_t);
extern "C" void* memset(void*, int, size_t);
extern "C" int strcmp(const char*, const char*);
extern "C" size_t strlen(const char*);
extern "C" char* strcat(char*, const char*);

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
int g_checks = 0;

bool check(bool condition, const char* what) {
    ++g_checks;
    printf("  %s %s\n", condition ? "ok  " : "FAIL", what);
    if (!condition) {
        ++g_failures;
    }
    return condition;
}

constexpr uint32_t kSector = 512;
constexpr uint32_t kSectors = 8192;
constexpr uint8_t kBuildDrive = fat_disk::kDrives - 1;
constexpr const char* kReadme = "read me\n";
const cred::Credentials kSystem = cred::system();

bool write_file(const char* path, const char* text) {
    FIL file = {};
    UINT done = 0;
    if (f_open(&file, path, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK) {
        return false;
    }
    const UINT size = static_cast<UINT>(strlen(text));
    const bool ok = f_write(&file, text, size, &done) == FR_OK && done == size;
    return f_close(&file) == FR_OK && ok;
}

// Docs/ReadMe.txt, Docs/Sub/x and Ñandú.txt, formatted and written by FatFs.
bool build_image(uint8_t* image) {
    memset(image, 0, static_cast<size_t>(kSectors) * kSector);
    hoststub::attach_device(image, kSectors, true);
    if (!fat_disk::bind(kBuildDrive, 0)) {
        return false;
    }
    const char root[3] = {static_cast<char>('0' + kBuildDrive), ':', 0};
    static uint8_t work[4096];
    const MKFS_PARM options = {static_cast<BYTE>(FM_FAT | FM_SFD), 0, 0, 0, 0};
    FATFS fs = {};
    char path[64];
    bool ok = f_mkfs(root, &options, work, sizeof(work)) == FR_OK && f_mount(&fs, root, 1) == FR_OK;
    snprintf(path, sizeof(path), "%s/Docs", root);
    ok = ok && f_mkdir(path) == FR_OK;
    snprintf(path, sizeof(path), "%s/Docs/Sub", root);
    ok = ok && f_mkdir(path) == FR_OK;
    snprintf(path, sizeof(path), "%s/Docs/ReadMe.txt", root);
    ok = ok && write_file(path, kReadme);
    snprintf(path, sizeof(path), "%s/Docs/Sub/x", root);
    ok = ok && write_file(path, "x");
    snprintf(path, sizeof(path), "%s/\xc3\x91" "and\xc3\xba.txt", root);
    ok = ok && write_file(path, "u");
    ok = f_mount(nullptr, root, 0) == FR_OK && ok;
    fat_disk::unbind(kBuildDrive);
    return ok;
}

// An initramfs with nothing in it: the VFS is ready once it reads the trailer.
const char kEmptyArchive[] =
    "070701" "00000000" "00000000" "00000000" "00000000" "00000001" "00000000"
    "00000000" "00000000" "00000000" "00000000" "00000000" "0000000B" "00000000"
    "TRAILER!!!\0\0\0\0";

size_t g_mount = fs::kInvalidMount;

// The VFS and the registry from scratch, the image mounted at /boot.
bool mount_image(uint8_t* image) {
    hoststub::attach_device(image, kSectors, true);
    vfs::initialize(kEmptyArchive, sizeof(kEmptyArchive) - 1u);
    fs::initialize();
    static bool registered = false;
    if (!registered) {
        registered = fs::register_driver(fat::driver());
    }
    g_mount = fs::mount_any("/boot");
    return vfs::ready() && g_mount != fs::kInvalidMount && fs::attach(g_mount);
}

void unmount_image() {
    (void)fs::release(g_mount, &vfs::is_held);
}

bool reads(const char* path, const char* expected) {
    vfs::Vnode* node = vfs::open(kSystem, path, SAVANXP_OPEN_READ, 0);
    char buffer[64] = {};
    const size_t length = strlen(expected);
    return node != nullptr && node->size == length && vfs::read(*node, 0, buffer, length) == length &&
           memcmp(buffer, expected, length) == 0;
}

bool named(const char* path, const char* name) {
    const vfs::Vnode* node = vfs::resolve(path);
    return node != nullptr && strcmp(node->name, name) == 0;
}

bool spelled(const vfs::Vnode* node, const char* expected) {
    char path[vfs::kMaxPathLength] = {};
    return node != nullptr && vfs::path_of(*node, path, sizeof(path)) && strcmp(path, expected) == 0;
}

void names_fold_case(uint8_t* image) {
    printf("case: names inside a FAT volume fold case\n");
    if (!check(build_image(image) && mount_image(image), "mounted at /boot")) {
        return;
    }
    const vfs::Vnode* readme = vfs::resolve("/boot/Docs/ReadMe.txt");
    check(readme != nullptr && vfs::resolve("/boot/docs/README.TXT") == readme,
          "two spellings of one name are one file");
    check(reads("/boot/DOCS/readme.txt", kReadme), "and read the same bytes");
    check(vfs::resolve("/boot/\xc3\xb1" "AND\xc3\x9a.TXT") != nullptr, "beyond ASCII too, as FatFs folds it");
    check(vfs::resolve("/BOOT/Docs") == nullptr, "the tree above the mount point stays case-sensitive");

    vfs::Vnode* opened = vfs::open(kSystem, "/boot/docs/README.txt", SAVANXP_OPEN_WRITE | SAVANXP_OPEN_CREATE, 0644);
    check(opened == readme && reads("/boot/Docs/ReadMe.txt", kReadme),
          "creating it in another case opens the file there");

    vfs::Vnode* created = vfs::open(kSystem, "/boot/DOCS/sub/New.txt", SAVANXP_OPEN_WRITE | SAVANXP_OPEN_CREATE, 0644);
    check(spelled(created, "/boot/Docs/Sub/New.txt"), "a new name keeps the directories' spelling");
    check(created != nullptr && vfs::write(*created, 0, "new", 3, false) == 3 && reads("/boot/docs/sub/new.txt", "new"),
          "and is written and read through any spelling");
    check(vfs::mkdir(kSystem, "/boot/docs/SUB/Deeper", 0755) != nullptr &&
              spelled(vfs::resolve("/boot/docs/sub/deeper"), "/boot/Docs/Sub/Deeper"),
          "mkdir too");

    check(vfs::rename(kSystem, "/boot/docs/readme.txt", "/boot/DOCS/README.TXT"), "a rename that only changes case");
    check(named("/boot/Docs/readme.txt", "README.TXT") && reads("/boot/Docs/README.TXT", kReadme),
          "renames the file, keeping its bytes");
    check(vfs::rename(kSystem, "/boot/docs/README.TXT", "/boot/Docs/README.TXT") && named("/boot/docs/readme.txt", "README.TXT"),
          "the same leaf through other directory spellings is no change");
    check(vfs::rename(kSystem, "/boot/docs", "/boot/DOCS"), "a directory's case changes");
    check(named("/boot/docs", "DOCS") && reads("/boot/DOCS/Sub/New.txt", "new") && reads("/boot/docs/sub/x", "x"),
          "with what is below it still reachable");
    check(fs::sync(g_mount), "sync");
    unmount_image();

    check(mount_image(image), "mounted again");
    check(named("/boot/docs", "DOCS") && named("/boot/docs/readme.txt", "README.TXT"), "the new cases are on the disk");
    check(named("/boot/docs/sub/new.txt", "New.txt") && named("/boot/docs/sub", "Sub"),
          "and the created names as they were spelled");
    unmount_image();
    hoststub::detach_device();
}

// Paths past the old 255-byte limit (vfs::kMaxPathLength is 1024): six
// directories of 60-byte names, a file at the bottom, a rename of the top one
// that rewrites every path below it, and a remount.
void long_paths(uint8_t* image) {
    printf("case: paths longer than 255 bytes\n");
    if (!check(build_image(image) && mount_image(image), "mounted at /boot")) {
        return;
    }
    char path[vfs::kMaxPathLength] = "/boot";
    char level[64] = {};
    bool made = true;
    for (int depth = 0; depth < 6 && made; ++depth) {
        snprintf(level, sizeof(level), "/level-%d-%052d", depth, 0);
        strcat(path, level);
        made = vfs::mkdir(kSystem, path, 0755) != nullptr;
    }
    check(made && strlen(path) > 300, "six directories deep, past 300 bytes");
    strcat(path, "/bottom.txt");
    vfs::Vnode* bottom = vfs::open(kSystem, path, SAVANXP_OPEN_WRITE | SAVANXP_OPEN_CREATE, 0644);
    check(bottom != nullptr && vfs::write(*bottom, 0, "deep", 4, false) == 4 && reads(path, "deep"),
          "a file at the bottom is written and read");

    snprintf(level, sizeof(level), "/level-%d-%052d", 0, 0);
    char top[96] = "/boot";
    strcat(top, level);
    check(vfs::rename(kSystem, top, "/boot/a-longer-top-directory-name-so-that-every-path-below-it-grows-x"),
          "renaming the top directory");
    char moved[vfs::kMaxPathLength] = "/boot/a-longer-top-directory-name-so-that-every-path-below-it-grows-x";
    strcat(moved, path + strlen(top));
    check(strlen(moved) > strlen(path) && reads(moved, "deep"), "rewrites the longer paths below it");
    check(fs::sync(g_mount), "sync");
    unmount_image();
    check(mount_image(image) && reads(moved, "deep"), "and they are on the disk");

    char too_long[vfs::kMaxPathLength + 16] = "/boot/";
    memset(too_long + 6, 'x', sizeof(too_long) - 7);
    too_long[sizeof(too_long) - 1] = '\0';
    check(vfs::resolve(too_long) == nullptr && vfs::open(kSystem, too_long, SAVANXP_OPEN_READ, 0) == nullptr,
          "a path longer than 1023 bytes is refused");
    unmount_image();
    hoststub::detach_device();
}

} // namespace

int main() {
    printf("FAT VFS TEST START\n");
    uint8_t* image = static_cast<uint8_t*>(malloc(static_cast<size_t>(kSectors) * kSector));
    if (image == nullptr) {
        printf("FAT VFS TEST FAIL: no memory for the image\n");
        return 1;
    }
    names_fold_case(image);
    long_paths(image);
    free(image);
    printf("%s (%d checks, %d failures)\n", g_failures == 0 ? "FAT VFS TEST PASS" : "FAT VFS TEST FAIL", g_checks,
           g_failures);
    return g_failures == 0 ? 0 : 1;
}
