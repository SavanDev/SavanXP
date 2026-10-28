#pragma once

#include <stddef.h>
#include <stdint.h>

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
};

void initialize(const void* archive, size_t size);
bool ready();
int last_error();
const Vnode* root();
bool normalize_path(const char* cwd, const char* input, char* output, size_t capacity);
const Vnode* resolve(const char* path);
Vnode* open(const char* path, uint32_t flags);
Vnode* ensure_directory(const char* path);
Vnode* mkdir(const char* path);
Vnode* install_external_file(const char* path, Backend backend, void* data, size_t size, bool writable);
// What a filesystem driver calls from attach/create_file to publish a file.
// `data` is the driver's own record; the VFS does not read it.
Vnode* install_volume_file(const char* path, const fs::Driver& driver, void* data, size_t size, bool writable);
bool unlink(const char* path);
bool rmdir(const char* path);
bool truncate(const char* path, size_t size);
bool rename(const char* old_path, const char* new_path);
size_t read(Vnode& node, size_t offset, void* buffer, size_t count);
size_t write(Vnode& node, size_t offset, const void* buffer, size_t count, bool truncate);
const Vnode* child_at(const Vnode& directory, size_t index);
size_t child_count(const Vnode& directory);

} // namespace vfs
