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
// Where the boot volume (the ESP, or the development boot drive) is mounted:
// the one holding boot/kernel.elf (storage::mount_boot_volume). Never
// unmounted.
constexpr const char* kBootMountPoint = "/boot";

// Who a volume belongs to when its format records no owners of its own
// (FAT): the person who asked to mount it. See Driver::adopt_owner.
struct Owner {
    uint32_t uid;
    uint32_t gid;
};

// Why a driver operation failed. The VFS turns it into the caller's errno; a
// driver says what happened instead of leaving the VFS to guess from a bool.
enum class Status : uint8_t {
    ok = 0,
    io_error,      // the device failed, or the on-disk structures are damaged
    no_space,      // no blocks, inodes or directory slots left
    read_only,     // the volume does not accept changes
    invalid,       // an argument the format cannot represent
    exists,        // the name is taken
    not_found,
    not_empty,     // a directory that still has entries
    not_directory,
    is_directory,
    unsupported,   // the driver has no such operation
    name_too_long, // a name or path past the format's limits
};

constexpr bool ok(Status status) {
    return status == Status::ok;
}
// The sx_status (errno) a failed operation reports. Never called with ok.
int error_of(Status status);

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
    // The format may hold the system's root. Only such a driver is tried for
    // kRootMountPoint: a FAT boot drive or a stick must never become "/",
    // whatever its position among the block devices.
    bool can_be_root;
    // Recognises the device and loads its metadata without touching the VFS.
    // kInvalidVolume is not an error: it means "not my format", and fs:: goes
    // on with the next driver.
    VolumeId (*probe)(size_t device_index, const char* mount_point);
    // Publishes the volume's tree under its mount point: directories with
    // vfs::ensure_directory, files with vfs::install_volume_file. Separate
    // from probe because at boot the metadata is read before the VFS is ready
    // to receive vnodes. probe has written nothing; with `read_only` attach
    // writes nothing either, and the driver refuses every change afterwards
    // (the VFS refuses first: fs::read_only). Without it, attach is where a
    // driver persists whatever recovery the volume needed.
    bool (*attach)(VolumeId volume, bool read_only);
    // May be nullptr: the volume then stays mounted until reset.
    void (*unmount)(VolumeId volume);

    // File operations on a vnode the driver published. The VFS has already
    // checked type, writability and range; size is kept in the vnode by the
    // driver. Every operation says why it failed (Status). Only read and
    // refresh are required: the VFS answers a missing one with EROFS for a
    // change and ENOSYS otherwise, so a driver that cannot write provides no
    // write operations at all.
    Status (*read)(vfs::Vnode& node, size_t offset, void* buffer, size_t count);
    Status (*write)(vfs::Vnode& node, size_t offset, const void* buffer, size_t count, bool truncate, size_t& written);
    Status (*truncate)(vfs::Vnode& node, size_t size);
    // Removes the file's name. With `keep_open` something still holds the
    // vnode: the driver keeps the file's contents readable and writable
    // through it until release() is called. Only passed to a driver that has
    // release; without one the VFS refuses to unlink a held file (EBUSY).
    Status (*unlink)(vfs::Vnode& node, bool keep_open);
    // The directory is empty (the VFS checked).
    Status (*remove_directory)(vfs::Vnode& node);
    // Re-reads size and writability into a vnode about to be used.
    void (*refresh)(vfs::Vnode& node);
    // The last holder of a vnode unlinked with keep_open (or replaced by a
    // rename with keep_replaced_open) is gone: the driver frees the file.
    // nullptr: the format cannot keep an unlinked file alive.
    void (*release)(vfs::Vnode& node);

    // Path operations, on absolute normalized paths under the volume's mount
    // point. Create hands back the published vnode. `owner` is who creates it
    // and the mode it gets; the VFS has already checked the caller may.
    Status (*create_file)(VolumeId volume, const char* path, const vfs::Ownership& owner, vfs::Vnode*& created);
    Status (*create_directory)(VolumeId volume, const char* path, const vfs::Ownership& owner, vfs::Vnode*& created);
    // Both paths are on this volume (the VFS refuses a cross-volume rename).
    // A driver with replaces_on_rename is also called when `new_path` exists,
    // after the VFS checked the types agree and a replaced directory is empty:
    // the old name's object takes the new name and what was there is freed, as
    // one atomic step. `keep_replaced_open` is keep_open for what is replaced.
    Status (*rename)(VolumeId volume, const char* old_path, const char* new_path, bool keep_replaced_open);
    // The format replaces an existing destination atomically. Without it the
    // VFS answers a rename onto an existing name with EEXIST rather than
    // imitate it with a delete and a rename.
    bool replaces_on_rename;
    // Records new ownership for a vnode the driver published (chmod, chown);
    // the VFS has checked the caller may. nullptr: the format has none, and the
    // change is refused.
    Status (*set_ownership)(vfs::Vnode& node, uint32_t uid, uint32_t gid, uint16_t mode);

    bool (*stats)(VolumeId volume, VolumeStats& out);
    // Makes the volume's metadata durable and marks it clean. May be nullptr.
    bool (*sync)(VolumeId volume);
    // For a format with no owners of its own: everything on the volume is
    // published as `owner`'s. Called by mount() between probe and attach.
    // nullptr: the format records owners itself, and the owner is ignored.
    void (*adopt_owner)(VolumeId volume, const Owner& owner);
    // Queue the same clean metadata/flush ordering without draining writeback.
    // A durability checkpoint must follow before reporting successful sync.
    bool (*sync_deferred)(VolumeId volume) = nullptr;
    // The first holder of a file vnode the driver published has arrived
    // (vfs::retain), and the last one has gone (vfs::release; before
    // release() for an unlinked file). Between the two the driver may keep
    // state open for the file, such as an open handle of its format. open
    // cannot fail: a driver that could not keep anything serves the file as
    // it does an unheld one. nullptr: nothing to keep.
    void (*open)(vfs::Vnode& node) = nullptr;
    void (*close)(vfs::Vnode& node) = nullptr;
    // Publishes the entries of a directory the driver installed with
    // vfs::install_volume_directory: files with vfs::install_volume_file,
    // subdirectories with install_volume_directory (themselves published
    // later, when looked into). Only that directory, never below it. On a
    // failure the VFS takes back what was published, calling forget for each.
    Status (*populate)(vfs::Vnode& directory) = nullptr;
    // The VFS is retiring a node the driver published, not because it was
    // deleted but to give its slot back (a failed populate, and later the
    // cache): the driver drops its own record of it. The entry stays on disk.
    void (*forget)(vfs::Vnode& node) = nullptr;
    // Whether two names are the same entry on this format, for the VFS's
    // lookups in the directories the driver publishes (VFS-C). nullptr: byte
    // for byte. FAT folds case as FatFs does, so "README.TXT" finds
    // "readme.txt" and creating one when the other exists opens it instead.
    bool (*same_name)(const char* a, const char* b) = nullptr;
};

struct MountInfo {
    const Driver* driver;
    const char* driver_name;
    VolumeId volume;
    size_t device_index;
    const char* mount_point;
    bool attached;
    // Mounted read-only on request (attach's flag).
    bool read_only;
};

// Empties the mount table; registered drivers stay. It does NOT unmount: it is
// the boot reset, sibling of block::probe_all() and sxfs::initialize().
void initialize();

bool register_driver(const Driver& driver);

// Tries the drivers on a device, by descending priority, and stops at the
// first that claims it; for kRootMountPoint, only those with can_be_root.
// With `driver_name`, only that driver is tried. With `owner`, a format that
// has no owners publishes everything as that person's (adopt_owner); without,
// as the system's. Returns the mount index or kInvalidMount.
size_t mount(size_t device_index, const char* mount_point, const char* driver_name = nullptr,
             const Owner* owner = nullptr);
// The same over every block device in order: the first one some driver claims
// wins. It is the boot path.
size_t mount_any(const char* mount_point);

// `read_only`: nothing is written to the device, at attach or later, and
// every change through the VFS is EROFS.
bool attach(size_t mount_index, bool read_only = false);
// Whether the volume takes no changes: mounted read-only, or one its driver
// could only bring up that way (a device that refuses writes, a recovery
// that could not be persisted).
bool read_only(size_t mount_index);
// Detaches the volume from the VFS and releases it: synced, its tree gone
// from the VFS (vfs::detach_subtree), the driver's state freed, the slot
// free. 0; -EBUSY when `referenced` says a node of it is still held (an open
// file, a mapped program), and nothing changes; -EINVAL for "/" or a driver
// without unmount; -EIO if the sync failed.
int release(size_t mount_index, bool (*referenced)(const vfs::Vnode& node));
// Bumps on every mount and release.
uint32_t generation();

// One past the highest mount slot in use: iterate and skip the holes
// mount_info() reports as false.
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
bool sync_all_deferred();

} // namespace fs
