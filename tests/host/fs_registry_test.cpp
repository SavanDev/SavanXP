/*
 * fs_registry_test.cpp -- the filesystem registry (kernel/fs.cpp), run on the
 * host against fake block devices and fake drivers that only say which
 * devices they recognise.
 *
 * What each case pins:
 *   - the root is mounted only by a driver with can_be_root, even when a
 *     device ahead of the system disk is claimed by another driver (a FAT
 *     boot drive on AHCI port 0), and even when that driver probes first;
 *   - with no device a root driver claims, there is no root (the rescue boot);
 *   - outside "/", any driver may mount, by priority as before;
 *   - a mount that names its driver tries that one only;
 *   - an owner given to mount reaches a driver with adopt_owner, and only it.
 */

#include <stdio.h>

#include "kernel/block.hpp"
#include "kernel/fs.hpp"
#include "kernel/vfs.hpp"

extern "C" int strcmp(const char*, const char*);

// --- Fake block devices -----------------------------------------------------

namespace {

constexpr size_t kDevices = 3;
size_t g_device_count = kDevices;

} // namespace

namespace block {
bool ready() { return true; }
size_t device_count() { return g_device_count; }
bool device_info(size_t index, DeviceInfo& info) {
    info = {};
    if (index >= g_device_count) {
        return false;
    }
    info.present = true;
    info.id = static_cast<uint32_t>(index + 1);
    info.sector_count = 1024;
    info.writable = true;
    info.name = "fake";
    return true;
}
} // namespace block

// --- The VFS calls fs.cpp makes, none of them reached here ------------------

namespace vfs {
bool is_memory_path(const char*) { return false; }
int last_error() { return 0; }
bool detach_subtree(const char*, bool (*)(const Vnode&)) { return true; }
} // namespace vfs

// --- Fake drivers -----------------------------------------------------------

namespace {

int g_failures = 0;

void check(bool condition, const char* what) {
    if (!condition) {
        printf("FAIL: %s\n", what);
        ++g_failures;
    }
}

// Which devices each fake format is on.
bool g_root_format_on[kDevices] = {};
bool g_fat_format_on[kDevices] = {};
size_t g_fat_root_probes = 0;
fs::Owner g_adopted = {};
bool g_adopt_called = false;

void adopt_owner(fs::VolumeId, const fs::Owner& owner) {
    g_adopted = owner;
    g_adopt_called = true;
}

fs::VolumeId probe_root_format(size_t device, const char*) {
    return device < kDevices && g_root_format_on[device] ? device : fs::kInvalidVolume;
}

fs::VolumeId probe_fat_format(size_t device, const char* mount_point) {
    if (strcmp(mount_point, fs::kRootMountPoint) == 0) {
        ++g_fat_root_probes;
    }
    return device < kDevices && g_fat_format_on[device] ? device : fs::kInvalidVolume;
}

bool attach(fs::VolumeId) { return true; }
fs::Status read(vfs::Vnode&, size_t, void*, size_t) { return fs::Status::ok; }
void refresh(vfs::Vnode&) {}
void unmount(fs::VolumeId) {}

const fs::Driver kRootDriver = {
    .name = "rootfs",
    .priority = 100,
    .can_be_root = true,
    .probe = &probe_root_format,
    .attach = &attach,
    .unmount = &unmount,
    .read = &read,
    .write = nullptr,
    .truncate = nullptr,
    .unlink = nullptr,
    .remove_directory = nullptr,
    .refresh = &refresh,
    .release = nullptr,
    .create_file = nullptr,
    .create_directory = nullptr,
    .rename = nullptr,
    .replaces_on_rename = false,
    .set_ownership = nullptr,
    .stats = nullptr,
    .sync = nullptr,
    .adopt_owner = nullptr,
};

// Probes before the root driver on purpose: priority alone must not let it
// take "/".
const fs::Driver kFatDriver = {
    .name = "fat",
    .priority = 200,
    .can_be_root = false,
    .probe = &probe_fat_format,
    .attach = &attach,
    .unmount = &unmount,
    .read = &read,
    .write = nullptr,
    .truncate = nullptr,
    .unlink = nullptr,
    .remove_directory = nullptr,
    .refresh = &refresh,
    .release = nullptr,
    .create_file = nullptr,
    .create_directory = nullptr,
    .rename = nullptr,
    .replaces_on_rename = false,
    .set_ownership = nullptr,
    .stats = nullptr,
    .sync = nullptr,
    .adopt_owner = &adopt_owner,
};

void reset(bool root0, bool root1, bool root2, bool fat0, bool fat1, bool fat2) {
    fs::initialize();
    g_device_count = kDevices;
    g_root_format_on[0] = root0;
    g_root_format_on[1] = root1;
    g_root_format_on[2] = root2;
    g_fat_format_on[0] = fat0;
    g_fat_format_on[1] = fat1;
    g_fat_format_on[2] = fat2;
    g_fat_root_probes = 0;
}

const char* driver_of(size_t mount) {
    fs::MountInfo info = {};
    return fs::mount_info(mount, info) ? info.driver_name : "";
}

size_t device_of(size_t mount) {
    fs::MountInfo info = {};
    return fs::mount_info(mount, info) ? info.device_index : static_cast<size_t>(-1);
}

void boot_drive_ahead_of_the_system_disk() {
    // ahci0 the FAT boot drive, ahci1 the system disk, ahci0p1 its partition.
    reset(false, true, false, true, false, true);
    const size_t root = fs::mount_any(fs::kRootMountPoint);
    check(root != fs::kInvalidMount, "the system disk mounts as /");
    check(strcmp(driver_of(root), "rootfs") == 0, "/ is mounted by the root driver");
    check(device_of(root) == 1, "/ is the system disk, not the boot drive");
    check(g_fat_root_probes == 0, "a driver without can_be_root is never probed for /");
}

void no_system_disk() {
    // The rescue boot: only the FAT boot drive is there.
    reset(false, false, false, true, false, true);
    check(fs::mount_any(fs::kRootMountPoint) == fs::kInvalidMount, "no root when only FAT is present");
    check(fs::mount_count() == 0, "a failed root mount takes no slot");
    check(fs::mount(0, fs::kRootMountPoint) == fs::kInvalidMount, "fs::mount refuses FAT at / too");
}

void elsewhere_any_driver() {
    reset(false, true, false, true, false, true);
    check(fs::mount_any(fs::kRootMountPoint) != fs::kInvalidMount, "root first");
    const size_t boot = fs::mount(2, "/boot");
    check(boot != fs::kInvalidMount, "FAT mounts outside /");
    check(strcmp(driver_of(boot), "fat") == 0, "and it is the FAT driver that mounts it");

    // Both formats claim device 0 here: below "/", priority decides as before.
    reset(true, true, false, true, false, false);
    const size_t other = fs::mount(0, "/mnt/both");
    check(strcmp(driver_of(other), "fat") == 0, "below /, the higher priority wins");

    // Naming a driver tries only that one, whatever the priorities.
    reset(true, true, false, true, false, false);
    const size_t named = fs::mount(0, "/mnt/named", "rootfs");
    check(strcmp(driver_of(named), "rootfs") == 0, "a named driver is the only one tried");
    check(fs::mount(1, "/mnt/nofat", "fat") == fs::kInvalidMount, "and it does not fall back to another");

    // An owner reaches the driver that claimed the volume, and only one with
    // the hook: a format that records owners itself ignores it.
    reset(false, false, false, true, false, false);
    g_adopt_called = false;
    const fs::Owner owner = {1000, 100};
    check(fs::mount(0, "/mnt/stick", nullptr, &owner) != fs::kInvalidMount, "mount with an owner");
    check(g_adopt_called && g_adopted.uid == 1000 && g_adopted.gid == 100, "the FAT driver adopts it");
    reset(true, false, false, false, false, false);
    g_adopt_called = false;
    check(fs::mount(0, "/mnt/own", nullptr, &owner) != fs::kInvalidMount && !g_adopt_called,
          "a driver without the hook mounts and ignores it");
    reset(false, false, false, true, false, false);
    g_adopt_called = false;
    check(fs::mount(0, "/mnt/plain") != fs::kInvalidMount && !g_adopt_called, "no owner, no call");
}

} // namespace

int main() {
    check(fs::register_driver(kRootDriver), "register the root driver");
    check(fs::register_driver(kFatDriver), "register the FAT driver");

    boot_drive_ahead_of_the_system_disk();
    no_system_disk();
    elsewhere_any_driver();

    if (g_failures != 0) {
        printf("fs-registry-test: %d failure(s)\n", g_failures);
        return 1;
    }
    printf("fs-registry-test: ok\n");
    return 0;
}
