#pragma once

#include <stddef.h>
#include <stdint.h>

namespace vfs {
struct Vnode;
struct Ownership;
}

/* The filesystem registry (docs/kernel/FILESYSTEMS.md), same pattern as
 * block::, display:: and audio::. Drivers do not compete for hardware here:
 * they compete to recognise a volume, so mount() stops at the first one that
 * claims the device, but can run again on another device and have another
 * driver win. fs:: decides what is mounted and where, not the driver.
 *
 * Once mounted, the VFS reaches the volume only through the Driver's
 * operations: a file vnode carries the driver that published it, and a path
 * belongs to the volume whose mount point is its longest prefix. */
namespace fs {

// A driver's own volume id. fs:: does not interpret it: it keeps it and hands
// it back to the driver that issued it.
using VolumeId = size_t;
constexpr VolumeId kInvalidVolume = static_cast<VolumeId>(-1);

constexpr size_t kInvalidMount = static_cast<size_t>(-1);
constexpr size_t kMaxDrivers = 4;
// Root + the installer's target + one removable medium. Each driver may set a
// lower limit of its own (sxfs:: does).
constexpr size_t kMaxMounts = 4;
constexpr size_t kMountPointCapacity = 32;

// Where the system disk is mounted: it is the root of the tree. The
// in-memory subtrees (vfs::is_memory_path) stay outside it.
constexpr const char* kRootMountPoint = "/";

struct VolumeStats {
    bool writable;
    size_t file_count;
    uint64_t total_bytes;
    uint64_t used_bytes;
    uint64_t free_bytes;
};

struct Driver {
    const char* name;
    // Higher probes first. It matters when two formats can be mistaken for
    // each other (a FAT32 with garbage where SxFS keeps its superblock).
    int priority;
    // Recognises the device and loads its metadata without touching the VFS.
    // kInvalidVolume is not an error: it means "not my format", and fs:: goes
    // on with the next driver.
    VolumeId (*probe)(size_t device_index, const char* mount_point);
    // Publishes the volume's tree under its mount point: directories with
    // vfs::ensure_directory, files with vfs::install_volume_file. Separate
    // from probe because at boot the metadata is read before the VFS is ready
    // to receive vnodes.
    bool (*attach)(VolumeId volume);
    // May be nullptr: the volume then stays mounted until reset.
    void (*unmount)(VolumeId volume);

    // File operations on a vnode the driver published. The VFS has already
    // checked type, writability and range; size is kept in the vnode by the
    // driver.
    bool (*read)(vfs::Vnode& node, size_t offset, void* buffer, size_t count);
    bool (*write)(vfs::Vnode& node, size_t offset, const void* buffer, size_t count, bool truncate, size_t& written);
    bool (*truncate)(vfs::Vnode& node, size_t size);
    bool (*unlink)(vfs::Vnode& node);
    // The directory is empty (the VFS checked).
    bool (*remove_directory)(vfs::Vnode& node);
    // Re-reads size and writability into a vnode about to be used.
    void (*refresh)(vfs::Vnode& node);

    // Path operations, on absolute normalized paths under the volume's mount
    // point. Create returns the published vnode, or nullptr when the volume
    // is full or read-only. `owner` is who creates it and the mode it gets;
    // the VFS has already checked the caller may.
    vfs::Vnode* (*create_file)(VolumeId volume, const char* path, const vfs::Ownership& owner);
    vfs::Vnode* (*create_directory)(VolumeId volume, const char* path, const vfs::Ownership& owner);
    // Both paths are on this volume (the VFS refuses a cross-volume rename).
    bool (*rename)(VolumeId volume, const char* old_path, const char* new_path);
    // Records new ownership for a vnode the driver published (chmod, chown);
    // the VFS has checked the caller may. nullptr: the format has none, and the
    // change is refused.
    bool (*set_ownership)(vfs::Vnode& node, uint32_t uid, uint32_t gid, uint16_t mode);

    bool (*stats)(VolumeId volume, VolumeStats& out);
    // Makes the volume's metadata durable and marks it clean. May be nullptr.
    bool (*sync)(VolumeId volume);
};

struct MountInfo {
    const Driver* driver;
    const char* driver_name;
    VolumeId volume;
    size_t device_index;
    const char* mount_point;
    bool attached;
};

// Empties the mount table; registered drivers stay. It does NOT unmount: it is
// the boot reset, sibling of block::probe_all() and sxfs::initialize().
void initialize();

bool register_driver(const Driver& driver);

// Tries the drivers on a device, by descending priority, and stops at the
// first that claims it. Returns the mount index or kInvalidMount.
size_t mount(size_t device_index, const char* mount_point);
// The same over every block device in order: the first one some driver claims
// wins. It is the boot path.
size_t mount_any(const char* mount_point);

bool attach(size_t mount_index);
void unmount(size_t mount_index);

size_t mount_count();
bool mount_info(size_t mount_index, MountInfo& out);
// Index of the volume mounted exactly at mount_point, or kInvalidMount.
size_t find(const char* mount_point);
// Index of the attached volume a path belongs to (longest mount point that is
// a whole-component prefix of it), or kInvalidMount for the in-memory tree.
// A path under an in-memory subtree (/dev, /rescue) never belongs to a volume,
// even with one mounted at "/".
size_t owner_of(const char* path);

// The part of a normalized absolute `path` inside the volume mounted at
// `mount_point`: "" for the mount point itself, "/a/b" for a path below it,
// nullptr when the path is outside. A mount point of "/" contains every
// absolute path, which is the one case a plain prefix test gets wrong.
const char* path_below(const char* mount_point, const char* path);
// The inverse: `mount_point` joined with a path relative to it ("a/b", or ""
// for the mount point). False if it does not fit in `capacity`.
bool join_mount_path(const char* mount_point, const char* relative, char* out, size_t capacity);

bool stats(size_t mount_index, VolumeStats& out);
bool sync(size_t mount_index);
// Every mounted volume; false if any failed.
bool sync_all();

} // namespace fs
