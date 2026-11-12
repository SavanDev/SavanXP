#pragma once

#include <stddef.h>
#include <stdint.h>

#include "kernel/credentials.hpp"

namespace fs {
struct Driver;
}

namespace vfs {

constexpr size_t kMaxPathLength = 256;
constexpr size_t kMaxPathComponentLength = 64;

/* The tree's layout (docs/kernel/FILESYSTEMS.md#the-root-is-the-system-disk).
 * The system disk is mounted at "/". Two subtrees stay in memory whatever is
 * mounted there: the devices, and the initramfs, published under /rescue so
 * the system can still reach a shell without a disk. */
constexpr const char* kDeviceRoot = "/dev";
constexpr const char* kRescueRoot = "/rescue";
// The disk used to be mounted at /disk. normalize_path() drops that first
// component, so binaries built before the move (an installed Doom carries
// /disk/lib in its loader) keep finding their files.
constexpr const char* kLegacyDiskAlias = "disk";

// The path (normalized, absolute) is kDeviceRoot, kRescueRoot or below one.
bool is_memory_path(const char* path);

enum class NodeType : uint8_t {
    invalid = 0,
    directory = 1,
    file = 2,
};

enum class Backend : uint8_t {
    memory = 0,
    // A file on a mounted volume: volume_driver (fs::Driver) serves it.
    volume = 1,
    device = 2,
};

struct Vnode {
    NodeType type;
    Backend backend;
    const char* name;
    void* data;
    size_t size;
    bool writable;
    int parent;
    int first_child;
    int next_sibling;
    // Set for Backend::volume, nullptr otherwise.
    const fs::Driver* volume_driver;
    // Ownership (docs/kernel/USERS_AND_PERMISSIONS.md). mode carries the type
    // bits plus 07777; 0 means "none recorded", which ownership_of() reads as
    // the system's with 0755 for a directory and 0644 for a file. A volume's
    // driver fills these from its own records; the initramfs from its CPIO
    // headers; a device from register_node.
    uint32_t uid;
    uint32_t gid;
    uint16_t mode;
    // Last modification, seconds since 1970 UTC; 0 when nothing recorded one.
    uint64_t mtime;
    // Bumps whenever the file's bytes or length change through the VFS. A
    // cache of the contents (a file-backed section) is valid only for the
    // generation it was read at.
    uint32_t content_generation;
    // The name is gone (unlink, or a rename over it) while something still
    // holds the node: it is out of every lookup and listing, and its contents
    // stay usable through the handles that have it until the last release().
    bool unlinked;
    // Holders counted on the node (retain/release): open handles, and the
    // file-backed sections read from it. Unlink, rename, rmdir and unmount
    // read it to decide whether a node can go.
    uint32_t references;
    // A volume directory published on demand (install_volume_directory)
    // carries its driver in volume_driver, with backend memory. Until its
    // first lookup or listing its entries are not in the tree; then the
    // driver's populate publishes them and this is set.
    bool populated;
    // When the directory was last looked into, on the VFS's own clock: the
    // bounded cache gives back the ones unvisited longest first.
    uint32_t last_used;
};

/* --- permissions ------------------------------------------------------------
 * The one check every driver inherits: it runs here, on the vnode, after the
 * path is normalized, and before a driver is reached. Unix semantics: owner
 * bits, else group bits, else other bits; acting as the system passes read and
 * write, and passes execute only if some x bit is set. Reaching a node needs
 * execute (search) on every directory above it. A refusal sets EACCES. */

constexpr uint32_t kAccessRead = 4;
constexpr uint32_t kAccessWrite = 2;
constexpr uint32_t kAccessExecute = 1;

constexpr uint16_t kModeTypeMask = 0170000;
constexpr uint16_t kModeDirectory = 0040000;
constexpr uint16_t kModeFile = 0100000;
constexpr uint16_t kModePermissions = 07777;
constexpr uint16_t kModeSticky = 01000;
// What a created file or directory gets when the caller says nothing.
constexpr uint16_t kDefaultFilePermissions = 0644;
constexpr uint16_t kDefaultDirectoryPermissions = 0755;

struct Ownership {
    uint32_t uid;
    uint32_t gid;
    uint16_t mode; // type bits plus permissions
};

Ownership ownership_of(const Vnode& node);
void set_ownership(Vnode& node, uint32_t uid, uint32_t gid, uint16_t mode);
bool permits(const cred::Credentials& who, const Vnode& node, uint32_t want);
// The path exists, every directory above it is searchable, and `want` is
// allowed on it.
bool may_access(const cred::Credentials& who, const char* path, uint32_t want);
// The same, for running it: a file with an x bit the caller may use.
bool may_execute(const cred::Credentials& who, const char* path);

void initialize(const void* archive, size_t size);
bool ready();
int last_error();
const Vnode* root();
bool normalize_path(const char* cwd, const char* input, char* output, size_t capacity);
const Vnode* resolve(const char* path);
// `create_permissions` is what a file created by this open gets (07777 bits);
// it is owned by the caller's euid and gid.
Vnode* open(const cred::Credentials& who, const char* path, uint32_t flags, uint16_t create_permissions);
// For drivers publishing a volume: creates the directories on the way, checks
// nothing.
Vnode* ensure_directory(const char* path);
Vnode* mkdir(const cred::Credentials& who, const char* path, uint16_t permissions);
Vnode* install_external_file(const char* path, Backend backend, void* data, size_t size, bool writable);
// What a filesystem driver calls from attach/create_file to publish a file.
// `data` is the driver's own record; the VFS does not read it.
// Releases the subtree rooted at the directory `path` -- a volume's mount
// point, at unmount -- and the directory itself: they leave the tree and
// their slots are reused. Refused, touching nothing, when `referenced` says
// any node of it is still held (last_error() is EBUSY), or when `path` is not
// a directory below "/" (EINVAL). Nodes are pointers others may keep; the
// predicate is how the caller proves nobody does.
bool detach_subtree(const char* path, bool (*referenced)(const Vnode& node));

Vnode* install_volume_file(const char* path, const fs::Driver& driver, void* data, size_t size, bool writable);
// A directory of a volume whose entries are published on demand: created (or
// found) at `path` and stamped with `driver`, whose populate publishes what it
// holds the first time it is looked into. A mount point too, at attach.
Vnode* install_volume_directory(const char* path, const fs::Driver& driver);

// Holders of a node: whoever keeps a Vnode pointer past the call that found
// it (an open handle, a file-backed section) retains it, and releases it when
// done. Unlink and rename keep a held file for its holders instead of freeing
// it; rmdir and unmount refuse a held node.
void retain(Vnode& node);
// One holder is gone. When it was the last one of an unlinked node, the file
// is freed (fs::Driver::release) and the node invalidated.
void release(Vnode& node);
bool is_held(const Vnode& node);
// The absolute path a node has now, rebuilt from its parents, so it follows
// renames of the node and of the directories above it. False when it does not
// fit in `capacity` or the node is not in the tree.
bool path_of(const Vnode& node, char* out, size_t capacity);

// Removes a file's name. A file something still holds keeps its contents for
// those holders (Vnode::unlinked); on a volume whose driver cannot do that,
// EBUSY.
bool unlink(const cred::Credentials& who, const char* path);
bool rmdir(const cred::Credentials& who, const char* path);
bool truncate(const cred::Credentials& who, const char* path, size_t size);
// An existing `new_path` is replaced in the same step: a file by a file, an
// empty directory by a directory (EISDIR, ENOTDIR, ENOTEMPTY otherwise). On a
// volume whose format cannot replace atomically, EEXIST. Across two volumes,
// EXDEV.
bool rename(const cred::Credentials& who, const char* old_path, const char* new_path);
// chmod: the owner or the system may set the permission bits (07777).
bool chmod(const cred::Credentials& who, const char* path, uint16_t permissions);
// chown: only the system gives a file away. kKeepId leaves that id as it is.
constexpr uint32_t kKeepId = 0xffffffffu;
bool chown(const cred::Credentials& who, const char* path, uint32_t uid, uint32_t gid);
// Both, on an open vnode rather than a path (fchmod).
bool chmod_node(const cred::Credentials& who, Vnode& node, uint16_t permissions);
// Bytes read; 0 at the end of the file AND on an error, which last_error()
// tells apart: it is 0 after a read that hit no error.
size_t read(Vnode& node, size_t offset, void* buffer, size_t count);
size_t write(Vnode& node, size_t offset, const void* buffer, size_t count, bool truncate);
const Vnode* child_at(const Vnode& directory, size_t index);
size_t child_count(const Vnode& directory);

/* --- the bounded cache (VFS-C, docs/roadmaps/open/VFS_ROADMAP.md) ------------
 * Published volume directories are given back when slots run low: their
 * entries leave the tree (the driver is told to forget each one) and the
 * directory is published again on its next look. A directory goes only with
 * nothing under it held, and only at the entry of a system call, under the
 * kernel lock. The rule that follows for all kernel code: a Vnode pointer
 * kept from one system call to the next must be retained. */
size_t free_slots();
// Gives back the least recently looked-into directories until `target_free`
// slots are free or nothing more can go. The slots given back.
size_t trim(size_t target_free);
// What a system call's entry calls: trims when free slots are below the
// low-water mark, and does not search again for a while after a trim that
// found too little.
void trim_if_low();

} // namespace vfs
