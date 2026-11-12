#include "kernel/vfs.hpp"

#include <stdint.h>

#include "kernel/fs.hpp"
#include "kernel/string.hpp"
#include "abi/savanxp_abi.h"

namespace {

// The initramfs, /dev, and every file and directory of the mounted volumes.
constexpr size_t kMaxNodes = 2048;
constexpr size_t kMaxNameLength = vfs::kMaxPathComponentLength;
constexpr size_t kMaxDynamicFiles = 32;
constexpr size_t kDynamicFileCapacity = 16384;
// Components in one path: a 1 KiB path of short names can be deep.
constexpr size_t kMaxNormalizedPathComponents = 64;

struct NodeSlot {
    vfs::Vnode vnode;
    char name[kMaxNameLength];
};

struct DynamicFile {
    bool in_use;
    size_t size;
    uint8_t storage[kDynamicFileCapacity];
};

NodeSlot g_nodes[kMaxNodes] = {};
DynamicFile g_dynamic_files[kMaxDynamicFiles] = {};
size_t g_node_count = 0;
// Slots given back by retire_node and detach_subtree, linked through
// vnode.next_sibling. A slot is given back only when nothing holds its node,
// and its content_generation survives reuse and moves on, so a cache keyed on
// (vnode, generation) never takes the next file in the slot for the last one
// (docs/kernel/SECTIONS.md).
int g_free_nodes = -1;
size_t g_free_listed = 0;
// Stamps Vnode::last_used each time a directory is looked into, so trim()
// gives back the directories nobody visited lately first.
uint32_t g_use_clock = 0;
bool g_ready = false;
int g_last_error = SX_ENOENT;
bool node_is_held(const vfs::Vnode& node) {
    return node.references != 0;
}

// A node that has a name in the tree: not invalidated, and not unlinked while
// held.
bool node_is_named(const vfs::Vnode& node) {
    return node.type != vfs::NodeType::invalid && !node.unlinked;
}

bool parse_hex_u32(const char* text, size_t count, uint32_t& value_out) {
    uint32_t value = 0;
    for (size_t index = 0; index < count; ++index) {
        const char digit = text[index];
        uint32_t nibble = 0;
        if (digit >= '0' && digit <= '9') {
            nibble = static_cast<uint32_t>(digit - '0');
        } else if (digit >= 'a' && digit <= 'f') {
            nibble = static_cast<uint32_t>(digit - 'a' + 10);
        } else if (digit >= 'A' && digit <= 'F') {
            nibble = static_cast<uint32_t>(digit - 'A' + 10);
        } else {
            return false;
        }
        value = (value << 4) | nibble;
    }
    value_out = value;
    return true;
}

int add_node(vfs::NodeType type, const char* name, int parent) {
    int node_index = -1;
    if (g_free_nodes >= 0) {
        node_index = g_free_nodes;
        g_free_nodes = g_nodes[node_index].vnode.next_sibling;
        --g_free_listed;
    } else if (g_node_count < kMaxNodes) {
        node_index = static_cast<int>(g_node_count++);
    } else {
        return -1;
    }

    NodeSlot& slot = g_nodes[node_index];
    const uint32_t generation = slot.vnode.content_generation;
    memset(&slot, 0, sizeof(slot));
    slot.vnode.content_generation = generation;
    strcpy(slot.name, name);
    slot.vnode.type = type;
    slot.vnode.backend = vfs::Backend::memory;
    slot.vnode.name = slot.name;
    slot.vnode.parent = parent;
    slot.vnode.first_child = -1;
    slot.vnode.next_sibling = -1;

    if (parent >= 0) {
        NodeSlot& parent_slot = g_nodes[parent];
        if (parent_slot.vnode.first_child < 0) {
            parent_slot.vnode.first_child = node_index;
        } else {
            int sibling = parent_slot.vnode.first_child;
            while (g_nodes[sibling].vnode.next_sibling >= 0) {
                sibling = g_nodes[sibling].vnode.next_sibling;
            }
            g_nodes[sibling].vnode.next_sibling = node_index;
        }
    }

    return node_index;
}

void retire_node(vfs::Vnode& node);

// Publishes a volume directory's entries the first time it is looked into
// (VFS-C, docs/roadmaps/open/VFS_ROADMAP.md). It is marked published before
// the driver runs, because publishing looks the new names up in it. On a
// failure what was published is taken back -- none of it can be held yet --
// so the next look tries again; false, with last_error saying why.
bool populate_directory(int index) {
    vfs::Vnode& directory = g_nodes[index].vnode;
    directory.last_used = ++g_use_clock;
    if (directory.type != vfs::NodeType::directory || directory.populated || directory.volume_driver == nullptr ||
        directory.volume_driver->populate == nullptr) {
        return true;
    }
    const fs::Driver& driver = *directory.volume_driver;
    // New entries are appended: what follows the current last child is what
    // this populate published.
    int last_before = -1;
    for (int child = directory.first_child; child >= 0; child = g_nodes[child].vnode.next_sibling) {
        last_before = child;
    }
    directory.populated = true;
    const fs::Status status = driver.populate(directory);
    if (fs::ok(status)) {
        return true;
    }
    int child = last_before >= 0 ? g_nodes[last_before].vnode.next_sibling : directory.first_child;
    while (child >= 0) {
        const int next = g_nodes[child].vnode.next_sibling;
        vfs::Vnode& entry = g_nodes[child].vnode;
        if (!node_is_held(entry) && entry.first_child < 0) {
            if (driver.forget != nullptr) {
                driver.forget(entry);
            }
            retire_node(entry);
        }
        child = next;
    }
    directory.populated = false;
    g_last_error = fs::error_of(status);
    return false;
}

int find_child(int parent, const char* name) {
    if (!populate_directory(parent)) {
        return -1;
    }
    // A volume's directory compares names by its format's rules (VFS-C).
    const fs::Driver* driver = g_nodes[parent].vnode.volume_driver;
    bool (*same_name)(const char*, const char*) = driver != nullptr ? driver->same_name : nullptr;
    int index = g_nodes[parent].vnode.first_child;
    while (index >= 0) {
        const vfs::Vnode& child = g_nodes[index].vnode;
        if (node_is_named(child) &&
            (same_name != nullptr ? same_name(child.name, name) : strcmp(child.name, name) == 0)) {
            return index;
        }
        index = g_nodes[index].vnode.next_sibling;
    }
    return -1;
}

int ensure_directory_node(int parent, const char* name) {
    const int existing = find_child(parent, name);
    if (existing >= 0) {
        return existing;
    }
    return add_node(vfs::NodeType::directory, name, parent);
}

// Adds an initramfs entry below `parent`, creating the directories on the way.
// The CPIO header's uid, gid and mode become the entry's ownership.
bool add_entry(int parent, const char* path, vfs::NodeType type, const void* data, size_t size,
               uint32_t uid, uint32_t gid, uint32_t mode, uint32_t mtime) {
    char component[kMaxNameLength] = {};
    const char* cursor = path;

    while (*cursor == '/') {
        ++cursor;
    }

    if (*cursor == '\0') {
        return true;
    }

    for (;;) {
        size_t length = 0;
        while (cursor[length] != '\0' && cursor[length] != '/') {
            if (length + 1 < sizeof(component)) {
                component[length] = cursor[length];
            }
            ++length;
        }
        component[length < sizeof(component) ? length : (sizeof(component) - 1)] = '\0';

        if (cursor[length] == '/') {
            parent = ensure_directory_node(parent, component);
            if (parent < 0) {
                return false;
            }
            cursor += length + 1;
            continue;
        }

        int node = find_child(parent, component);
        if (node < 0) {
            node = add_node(type, component, parent);
        }
        if (node < 0) {
            return false;
        }
        g_nodes[node].vnode.type = type;
        g_nodes[node].vnode.data = const_cast<void*>(data);
        g_nodes[node].vnode.size = size;
        g_nodes[node].vnode.writable = false;
        g_nodes[node].vnode.uid = uid;
        g_nodes[node].vnode.gid = gid;
        g_nodes[node].vnode.mode = static_cast<uint16_t>(
            (type == vfs::NodeType::directory ? vfs::kModeDirectory : vfs::kModeFile) |
            (mode & vfs::kModePermissions));
        g_nodes[node].vnode.mtime = mtime;
        return true;
    }
}

int resolve_index(const char* path) {
    if (!g_ready || path == nullptr || g_node_count == 0) {
        return -1;
    }

    if (strcmp(path, "/") == 0 || *path == '\0') {
        return 0;
    }

    int current = 0;
    const char* cursor = path;
    while (*cursor == '/') {
        ++cursor;
    }

    char component[kMaxNameLength] = {};
    while (*cursor != '\0') {
        size_t length = 0;
        while (cursor[length] != '\0' && cursor[length] != '/') {
            if (length + 1 < sizeof(component)) {
                component[length] = cursor[length];
            }
            ++length;
        }
        component[length < sizeof(component) ? length : (sizeof(component) - 1)] = '\0';
        current = find_child(current, component);
        if (current < 0) {
            return -1;
        }
        cursor += length;
        while (*cursor == '/') {
            ++cursor;
        }
    }

    return current;
}

DynamicFile* allocate_dynamic_file() {
    for (DynamicFile& file : g_dynamic_files) {
        if (!file.in_use) {
            memset(&file, 0, sizeof(file));
            file.in_use = true;
            return &file;
        }
    }
    return nullptr;
}

void refresh_external_node(vfs::Vnode& node) {
    if (node.backend != vfs::Backend::volume || node.volume_driver == nullptr) {
        return;
    }
    node.volume_driver->refresh(node);
}

bool attach_dynamic_storage(vfs::Vnode& node, bool truncate_existing) {
    if (node.type != vfs::NodeType::file || node.backend != vfs::Backend::memory) {
        return false;
    }

    if (!node.writable) {
        DynamicFile* file = allocate_dynamic_file();
        if (file == nullptr) {
            return false;
        }

        if (!truncate_existing && node.data != nullptr && node.size != 0) {
            const size_t to_copy = node.size < kDynamicFileCapacity ? node.size : kDynamicFileCapacity;
            memcpy(file->storage, node.data, to_copy);
            file->size = to_copy;
        }

        node.data = file->storage;
        node.size = file->size;
        node.writable = true;
    }

    if (truncate_existing) {
        node.size = 0;
    }

    return true;
}

bool split_parent_path(const char* path, char* parent_path, size_t parent_capacity, char* leaf_name, size_t leaf_capacity) {
    if (path == nullptr || parent_path == nullptr || leaf_name == nullptr ||
        parent_capacity == 0 || leaf_capacity == 0) {
        return false;
    }

    size_t length = strlen(path);

    if (length == 0) {
        return false;
    }

    size_t slash = length;
    while (slash > 0 && path[slash - 1] != '/') {
        --slash;
    }

    const size_t leaf_length = length - slash;
    if (leaf_length == 0 || leaf_length >= leaf_capacity) {
        return false;
    }

    memcpy(leaf_name, path + slash, leaf_length);
    leaf_name[leaf_length] = '\0';

    if (slash == 0) {
        if (parent_capacity < 2) {
            return false;
        }
        parent_path[0] = '/';
        parent_path[1] = '\0';
        return true;
    }

    const size_t parent_length = slash > 1 ? (slash - 1) : 1;
    if (parent_length >= parent_capacity) {
        return false;
    }

    memcpy(parent_path, path, parent_length);
    parent_path[parent_length] = '\0';
    return true;
}

// The volume a path belongs to: its driver and volume id. False for the
// in-memory tree.
struct VolumeRef {
    const fs::Driver* driver;
    fs::VolumeId volume;
    size_t mount;
};

bool volume_for_path(const char* path, VolumeRef& out) {
    const size_t mount = fs::owner_of(path);
    fs::MountInfo info = {};
    if (mount == fs::kInvalidMount || !fs::mount_info(mount, info)) {
        return false;
    }
    out.driver = info.driver;
    out.volume = info.volume;
    out.mount = mount;
    return true;
}

// The read-only mount policy, enforced here so that no driver is asked: a
// volume mounted read-only, or one its driver could only bring up that way,
// takes no change (EROFS).
bool path_is_read_only(const char* path) {
    VolumeRef volume = {};
    return volume_for_path(path, volume) && fs::read_only(volume.mount);
}

// A mount point is the root of a volume: it cannot be removed or renamed
// from inside.
bool is_mount_point(const char* path) {
    return fs::find(path) != fs::kInvalidMount;
}

bool set_node_name(int index, const char* name) {
    if (index < 0 || static_cast<size_t>(index) >= g_node_count || name == nullptr) {
        return false;
    }
    if (strlen(name) >= kMaxNameLength) {
        return false;
    }
    strcpy(g_nodes[index].name, name);
    g_nodes[index].vnode.name = g_nodes[index].name;
    return true;
}

void detach_node(int index) {
    if (index <= 0 || static_cast<size_t>(index) >= g_node_count) {
        return;
    }

    const int parent = g_nodes[index].vnode.parent;
    if (parent < 0 || static_cast<size_t>(parent) >= g_node_count) {
        return;
    }

    if (g_nodes[parent].vnode.first_child == index) {
        g_nodes[parent].vnode.first_child = g_nodes[index].vnode.next_sibling;
    } else {
        int sibling = g_nodes[parent].vnode.first_child;
        while (sibling >= 0) {
            if (g_nodes[sibling].vnode.next_sibling == index) {
                g_nodes[sibling].vnode.next_sibling = g_nodes[index].vnode.next_sibling;
                break;
            }
            sibling = g_nodes[sibling].vnode.next_sibling;
        }
    }

    g_nodes[index].vnode.next_sibling = -1;
}

void attach_node(int index, int parent) {
    if (index < 0 || parent < 0 || static_cast<size_t>(index) >= g_node_count || static_cast<size_t>(parent) >= g_node_count) {
        return;
    }

    g_nodes[index].vnode.parent = parent;
    g_nodes[index].vnode.next_sibling = -1;
    if (g_nodes[parent].vnode.first_child < 0) {
        g_nodes[parent].vnode.first_child = index;
        return;
    }

    int sibling = g_nodes[parent].vnode.first_child;
    while (g_nodes[sibling].vnode.next_sibling >= 0) {
        sibling = g_nodes[sibling].vnode.next_sibling;
    }
    g_nodes[sibling].vnode.next_sibling = index;
}

bool node_is_ancestor(int ancestor, int node) {
    while (node >= 0) {
        if (node == ancestor) {
            return true;
        }
        node = g_nodes[node].vnode.parent;
    }
    return false;
}

int index_of(const vfs::Vnode& node) {
    return static_cast<int>(reinterpret_cast<const NodeSlot*>(&node) - g_nodes);
}

bool node_has_valid_children(const vfs::Vnode& node) {
    // An unpublished directory is not known to be empty until it is looked
    // into; if that fails, it is not taken for empty either.
    if (!populate_directory(index_of(node))) {
        return true;
    }
    int child = node.first_child;
    while (child >= 0) {
        if (node_is_named(g_nodes[child].vnode)) {
            return true;
        }
        child = g_nodes[child].vnode.next_sibling;
    }
    return false;
}

// A slot cleared for reuse. Its content generation is kept and moved on.
void clear_slot(int index) {
    const uint32_t generation = g_nodes[index].vnode.content_generation + 1u;
    memset(&g_nodes[index], 0, sizeof(g_nodes[index]));
    g_nodes[index].vnode.type = vfs::NodeType::invalid;
    g_nodes[index].vnode.parent = -1;
    g_nodes[index].vnode.first_child = -1;
    g_nodes[index].vnode.content_generation = generation;
    g_nodes[index].vnode.next_sibling = g_free_nodes;
    g_free_nodes = index;
    ++g_free_listed;
}

// Takes a node nothing holds out of the tree for good and gives its slot
// back. The callers have checked: no handle has it (node_is_held), its driver
// has dropped its record, and a directory has nothing left in its chain.
void retire_node(vfs::Vnode& node) {
    const int index = static_cast<int>(reinterpret_cast<NodeSlot*>(&node) - g_nodes);
    if (index <= 0 || static_cast<size_t>(index) >= g_node_count) {
        return;
    }
    if (node.backend == vfs::Backend::memory && node.data != nullptr) {
        for (DynamicFile& file : g_dynamic_files) {
            if (file.in_use && node.data == file.storage) {
                file.in_use = false;
            }
        }
    }
    detach_node(index);
    clear_slot(index);
}

// A directory whose chain still carries a file unlinked while held: the
// directory's slot cannot go while that file hangs from it.
bool node_has_unlinked_children(const vfs::Vnode& node) {
    for (int child = node.first_child; child >= 0; child = g_nodes[child].vnode.next_sibling) {
        if (g_nodes[child].vnode.unlinked) {
            return true;
        }
    }
    return false;
}

// A driver operation the driver does not have: a change on a volume that
// takes none is EROFS.
constexpr int kNoWriteOperation = SX_EROFS;

// The whole subtree under `root`, root first, into g_subtree. Iterative: a
// volume can be deeper than the kernel stack is kind to.
int g_subtree[kMaxNodes];

size_t collect_subtree(int root) {
    size_t count = 0;
    g_subtree[count++] = root;
    for (size_t at = 0; at < count; ++at) {
        for (int child = g_nodes[g_subtree[at]].vnode.first_child; child >= 0;
             child = g_nodes[child].vnode.next_sibling) {
            if (count >= kMaxNodes) {
                return count;
            }
            g_subtree[count++] = child;
        }
    }
    return count;
}

// Every directory above `index` lets `who` search it.
bool ancestors_searchable(const cred::Credentials& who, int index) {
    int ancestor = g_nodes[index].vnode.parent;
    while (ancestor >= 0) {
        if (!vfs::permits(who, g_nodes[ancestor].vnode, vfs::kAccessExecute)) {
            return false;
        }
        ancestor = g_nodes[ancestor].vnode.parent;
    }
    return true;
}

// `who` may add or remove an entry in directory `parent`: search on the way
// there, then write and search on the directory itself.
bool may_change_directory(const cred::Credentials& who, int parent) {
    return ancestors_searchable(who, parent) &&
        vfs::permits(who, g_nodes[parent].vnode, vfs::kAccessWrite | vfs::kAccessExecute);
}

// The sticky bit (/tmp is 1777): in such a directory only the entry's owner,
// the directory's owner or the system may remove or rename an entry.
bool sticky_allows(const cred::Credentials& who, int parent, const vfs::Vnode& entry) {
    const vfs::Ownership directory = vfs::ownership_of(g_nodes[parent].vnode);
    if ((directory.mode & vfs::kModeSticky) == 0 || cred::acts_as_system(who)) {
        return true;
    }
    return who.euid == vfs::ownership_of(entry).uid || who.euid == directory.uid;
}

} // namespace

namespace vfs {

Ownership ownership_of(const Vnode& node) {
    if (node.mode != 0) {
        return {node.uid, node.gid, node.mode};
    }
    const bool directory = node.type == NodeType::directory;
    return {cred::kSystemUid, cred::kSystemGid,
            static_cast<uint16_t>(directory ? (kModeDirectory | kDefaultDirectoryPermissions)
                                            : (kModeFile | kDefaultFilePermissions))};
}

void set_ownership(Vnode& node, uint32_t uid, uint32_t gid, uint16_t mode) {
    node.uid = uid;
    node.gid = gid;
    node.mode = mode;
}

bool permits(const cred::Credentials& who, const Vnode& node, uint32_t want) {
    const Ownership owner = ownership_of(node);
    if (cred::acts_as_system(who)) {
        // The system reads and writes anything, and runs only what something
        // marked runnable; a directory is always searchable to it.
        return (want & kAccessExecute) == 0 || node.type == NodeType::directory ||
            (owner.mode & 0111) != 0;
    }
    uint32_t granted = owner.mode & 07;
    if (who.euid == owner.uid) {
        granted = (owner.mode >> 6) & 07;
    } else if (who.gid == owner.gid) {
        granted = (owner.mode >> 3) & 07;
    }
    return (granted & want) == want;
}

namespace {

bool path_of_node(const Vnode& node, char* out, size_t capacity);

// The path a driver is given for a new name: the directories above it as the
// tree spells them, and the leaf as the caller does. On a volume that folds
// case the caller may have spelled the directories differently, and drivers
// that keep paths compare them byte for byte.
bool spelled_path(int parent, const char* leaf, char* out, size_t capacity) {
    if (!path_of_node(g_nodes[parent].vnode, out, capacity)) {
        return false;
    }
    size_t length = strlen(out);
    const size_t leaf_length = strlen(leaf);
    if (length + 1 + leaf_length + 1 > capacity) {
        return false;
    }
    if (length > 1) {
        out[length++] = '/';
    }
    memcpy(out + length, leaf, leaf_length + 1);
    return true;
}

} // namespace

bool may_access(const cred::Credentials& who, const char* path, uint32_t want) {
    PathBuffer normalized;
    if (normalized == nullptr) {
        g_last_error = SX_ENOMEM;
        return false;
    }
    if (path == nullptr || !normalize_path("/", path, normalized, normalized.capacity())) {
        g_last_error = SX_EINVAL;
        return false;
    }
    const int index = resolve_index(normalized);
    if (index < 0) {
        g_last_error = SX_ENOENT;
        return false;
    }
    refresh_external_node(g_nodes[index].vnode);
    if (!ancestors_searchable(who, index) || !permits(who, g_nodes[index].vnode, want)) {
        g_last_error = SX_EACCES;
        return false;
    }
    return true;
}

bool may_execute(const cred::Credentials& who, const char* path) {
    return may_access(who, path, kAccessExecute);
}

bool normalize_path(const char* cwd, const char* input, char* output, size_t capacity) {
    if (cwd == nullptr || input == nullptr || output == nullptr || capacity < 2) {
        return false;
    }

    PathBuffer working;
    if (working == nullptr) {
        g_last_error = SX_ENOMEM;
        return false;
    }
    size_t working_length = 0;
    if (input[0] == '/') {
        working_length = strlen(input);
        if (working_length >= working.capacity()) {
            return false;
        }
        memcpy(working, input, working_length + 1);
    } else {
        const size_t cwd_length = strlen(cwd);
        const size_t input_length = strlen(input);
        if (cwd_length + (cwd_length > 1 ? 1u : 0u) + input_length >= working.capacity()) {
            return false;
        }
        memcpy(working, cwd, cwd_length + 1);
        working_length = cwd_length;
        if (working_length > 1 && working[working_length - 1] != '/') {
            working[working_length++] = '/';
            working[working_length] = '\0';
        }
        memcpy(working + working_length, input, input_length + 1);
    }

    // Each component is where it starts in `working`, and how long it is:
    // copying them out would put kMaxNormalizedPathComponents names on the
    // kernel stack.
    const char* components[kMaxNormalizedPathComponents] = {};
    size_t lengths[kMaxNormalizedPathComponents] = {};
    size_t component_count = 0;
    char* cursor = working;
    while (*cursor != '\0') {
        while (*cursor == '/') {
            ++cursor;
        }
        if (*cursor == '\0') {
            break;
        }

        char* start = cursor;
        while (*cursor != '\0' && *cursor != '/') {
            ++cursor;
        }

        const size_t length = static_cast<size_t>(cursor - start);
        if (length == 0) {
            continue;
        }
        if (length == 1 && start[0] == '.') {
            continue;
        }
        if (length == 2 && start[0] == '.' && start[1] == '.') {
            if (component_count > 0) {
                --component_count;
            }
            continue;
        }
        if (component_count >= kMaxNormalizedPathComponents || length >= kMaxNameLength) {
            return false;
        }
        components[component_count] = start;
        lengths[component_count] = length;
        ++component_count;
    }

    // The legacy /disk prefix (kLegacyDiskAlias) names the root.
    size_t first = 0;
    const size_t alias_length = strlen(kLegacyDiskAlias);
    if (component_count > 0 && lengths[0] == alias_length && memcmp(components[0], kLegacyDiskAlias, alias_length) == 0) {
        first = 1;
    }

    size_t written = 0;
    output[written++] = '/';
    for (size_t index = first; index < component_count; ++index) {
        const size_t length = lengths[index];
        if (written + length + 1 >= capacity) {
            return false;
        }
        memcpy(output + written, components[index], length);
        written += length;
        if (index + 1 < component_count) {
            output[written++] = '/';
        }
    }
    output[written] = '\0';
    return true;
}

bool is_memory_path(const char* path) {
    if (path == nullptr) {
        return false;
    }
    const char* const roots[] = {kDeviceRoot, kRescueRoot};
    for (const char* root : roots) {
        const size_t length = strlen(root);
        if (strncmp(path, root, length) == 0 && (path[length] == '\0' || path[length] == '/')) {
            return true;
        }
    }
    return false;
}

void initialize(const void* archive, size_t size) {
    memset(g_nodes, 0, sizeof(g_nodes));
    memset(g_dynamic_files, 0, sizeof(g_dynamic_files));
    g_node_count = 0;
    g_free_nodes = -1;
    g_free_listed = 0;
    g_use_clock = 0;
    g_ready = false;

    if (add_node(NodeType::directory, "/", -1) != 0) {
        return;
    }
    // The initramfs is published under /rescue (kRescueRoot), not at the root:
    // the root belongs to the system disk.
    const int rescue = add_node(NodeType::directory, kRescueRoot + 1, 0);
    if (rescue < 0) {
        return;
    }

    if (archive == nullptr || size < 110) {
        return;
    }

    const auto* base = static_cast<const uint8_t*>(archive);
    const uint8_t* cursor = base;
    const uint8_t* end = base + size;

    while (static_cast<size_t>(end - cursor) >= 110) {
        const char* header = reinterpret_cast<const char*>(cursor);
        if (memcmp(header, "070701", 6) != 0) {
            return;
        }

        uint32_t mode = 0;
        uint32_t uid = 0;
        uint32_t gid = 0;
        uint32_t mtime = 0;
        uint32_t file_size = 0;
        uint32_t name_size = 0;
        if (!parse_hex_u32(header + 14, 8, mode) ||
            !parse_hex_u32(header + 22, 8, uid) ||
            !parse_hex_u32(header + 30, 8, gid) ||
            !parse_hex_u32(header + 46, 8, mtime) ||
            !parse_hex_u32(header + 54, 8, file_size) ||
            !parse_hex_u32(header + 94, 8, name_size)) {
            return;
        }
        cursor += 110;

        if (name_size == 0 || name_size > kMaxPathLength ||
            name_size > static_cast<size_t>(end - cursor)) {
            return;
        }

        const char* name = reinterpret_cast<const char*>(cursor);
        bool terminated = false;
        for (uint32_t index = 0; index < name_size; ++index) {
            if (name[index] == '\0' && index + 1 == name_size) {
                terminated = true;
                break;
            }
            if (name[index] == '\0') {
                return;
            }
        }
        if (!terminated) {
            return;
        }

        const size_t name_end_offset = static_cast<size_t>(cursor - base) + name_size;
        const size_t name_padding = (4 - (name_end_offset & 3u)) & 3u;
        if (name_padding > static_cast<size_t>(end - cursor) - name_size) {
            return;
        }
        cursor += name_size + name_padding;
        if (strcmp(name, "TRAILER!!!") == 0) {
            g_ready = true;
            return;
        }

        if (file_size > static_cast<size_t>(end - cursor)) {
            return;
        }
        const uint8_t* data = cursor;
        const size_t data_end_offset = static_cast<size_t>(cursor - base) + file_size;
        const size_t data_padding = (4 - (data_end_offset & 3u)) & 3u;
        if (data_padding > static_cast<size_t>(end - cursor) - file_size) {
            return;
        }
        cursor += file_size + data_padding;

        const uint32_t file_type = mode & 0170000u;
        if (file_type != 0 && file_type != 0040000u && file_type != 0100000u) {
            return;
        }
        const NodeType type = file_type == 0040000u ? NodeType::directory : NodeType::file;
        if (!add_entry(rescue, name, type, data, file_size, uid, gid, mode, mtime)) {
            return;
        }
    }
}

bool ready() {
    return g_ready;
}

int last_error() {
    return g_last_error;
}

const Vnode* root() {
    return g_node_count != 0 ? &g_nodes[0].vnode : nullptr;
}

const Vnode* resolve(const char* path) {
    PathBuffer normalized;
    if (normalized == nullptr) {
        g_last_error = SX_ENOMEM;
        return nullptr;
    }
    if (!normalize_path("/", path != nullptr ? path : "", normalized, normalized.capacity())) {
        return nullptr;
    }
    const int index = resolve_index(normalized);
    if (index < 0) {
        return nullptr;
    }
    refresh_external_node(g_nodes[index].vnode);
    return &g_nodes[index].vnode;
}

Vnode* open(const cred::Credentials& who, const char* path, uint32_t flags, uint16_t create_permissions) {
    g_last_error = SX_ENOENT;
    if (!g_ready || path == nullptr || *path == '\0') {
        g_last_error = SX_EINVAL;
        return nullptr;
    }

    PathBuffer normalized;
    if (normalized == nullptr) {
        g_last_error = SX_ENOMEM;
        return nullptr;
    }
    if (!normalize_path("/", path, normalized, normalized.capacity())) {
        g_last_error = SX_EINVAL;
        return nullptr;
    }

    const bool wants_write = (flags & SAVANXP_OPEN_WRITE) != 0;
    const bool wants_create = (flags & SAVANXP_OPEN_CREATE) != 0;
    const bool wants_truncate = (flags & SAVANXP_OPEN_TRUNCATE) != 0;

    int index = resolve_index(normalized);
    if (index >= 0) {
        Vnode& node = g_nodes[index].vnode;
        refresh_external_node(node);
        // No access mode is a query open, NT's CreateFile with no desired
        // access: reaching the node is the whole check. The subsystem must
        // then grant a query-only handle, never read
        // (docs/kernel/USERS_AND_PERMISSIONS.md#what-the-check-covers).
        const uint32_t want = ((flags & SAVANXP_OPEN_READ) != 0 ? kAccessRead : 0u) |
            ((wants_write || wants_truncate) ? kAccessWrite : 0u);
        if (!ancestors_searchable(who, index) || !permits(who, node, want)) {
            g_last_error = SX_EACCES;
            return nullptr;
        }
        if ((wants_write || wants_truncate) && node.backend == Backend::memory) {
            if (!attach_dynamic_storage(node, wants_truncate)) {
                g_last_error = SX_ENOMEM;
                return nullptr;
            }
        } else if ((wants_write || wants_truncate) && node.backend == Backend::volume) {
            if (!node.writable) {
                g_last_error = path_is_read_only(normalized) ? SX_EROFS : SX_EBADF;
                return nullptr;
            }
            if (wants_truncate) {
                size_t written = 0;
                if (node.volume_driver->write == nullptr) {
                    g_last_error = kNoWriteOperation;
                    return nullptr;
                }
                const fs::Status truncated = node.volume_driver->write(node, 0, nullptr, 0, true, written);
                if (!fs::ok(truncated)) {
                    g_last_error = fs::error_of(truncated);
                    return nullptr;
                }
                ++node.content_generation;
            }
        }
        return &node;
    }

    if (!wants_write || !wants_create) {
        return nullptr;
    }

    PathBuffer parent_path;
    if (parent_path == nullptr) {
        g_last_error = SX_ENOMEM;
        return nullptr;
    }
    char leaf_name[kMaxNameLength] = {};
    if (!split_parent_path(normalized, parent_path, parent_path.capacity(), leaf_name, sizeof(leaf_name))) {
        g_last_error = SX_EINVAL;
        return nullptr;
    }

    index = resolve_index(parent_path);
    if (index < 0) {
        g_last_error = SX_ENOENT;
        return nullptr;
    }

    Vnode& parent = g_nodes[index].vnode;
    if (parent.type != NodeType::directory) {
        g_last_error = SX_EINVAL;
        return nullptr;
    }
    if (!may_change_directory(who, index)) {
        g_last_error = SX_EACCES;
        return nullptr;
    }

    const Ownership owner = {who.euid, who.gid,
                             static_cast<uint16_t>(kModeFile | (create_permissions & kModePermissions))};
    VolumeRef volume = {};
    if (volume_for_path(normalized, volume)) {
        if (volume.driver->create_file == nullptr || fs::read_only(volume.mount)) {
            g_last_error = kNoWriteOperation;
            return nullptr;
        }
        PathBuffer spelled;
        if (spelled == nullptr) {
            g_last_error = SX_ENOMEM;
            return nullptr;
        }
        if (!spelled_path(index, leaf_name, spelled, spelled.capacity())) {
            g_last_error = SX_EINVAL;
            return nullptr;
        }
        Vnode* node = nullptr;
        const fs::Status created = volume.driver->create_file(volume.volume, spelled, owner, node);
        if (!fs::ok(created) || node == nullptr) {
            g_last_error = fs::ok(created) ? SX_ENOSPC : fs::error_of(created);
            return nullptr;
        }
        return node;
    }

    // The in-memory tree is /dev and /rescue: the system's in every case.
    if (!cred::acts_as_system(who)) {
        g_last_error = SX_EACCES;
        return nullptr;
    }
    const int node_index = add_node(NodeType::file, leaf_name, index);
    if (node_index < 0) {
        g_last_error = SX_ENOMEM;
        return nullptr;
    }

    Vnode& node = g_nodes[node_index].vnode;
    if (!attach_dynamic_storage(node, true)) {
        g_last_error = SX_ENOMEM;
        return nullptr;
    }
    set_ownership(node, owner.uid, owner.gid, owner.mode);

    return &node;
}

Vnode* ensure_directory(const char* path) {
    if (!g_ready || path == nullptr || *path == '\0') {
        g_last_error = SX_EINVAL;
        return nullptr;
    }

    PathBuffer normalized;
    if (normalized == nullptr) {
        g_last_error = SX_ENOMEM;
        return nullptr;
    }
    if (!normalize_path("/", path, normalized, normalized.capacity())) {
        g_last_error = SX_EINVAL;
        return nullptr;
    }

    const int existing = resolve_index(normalized);
    if (existing >= 0) {
        return g_nodes[existing].vnode.type == NodeType::directory ? &g_nodes[existing].vnode : nullptr;
    }

    char component[kMaxNameLength] = {};
    const char* cursor = normalized;
    int parent = 0;

    while (*cursor == '/') {
        ++cursor;
    }

    if (*cursor == '\0') {
        return &g_nodes[0].vnode;
    }

    for (;;) {
        size_t length = 0;
        while (cursor[length] != '\0' && cursor[length] != '/') {
            if (length + 1 < sizeof(component)) {
                component[length] = cursor[length];
            }
            ++length;
        }
        component[length < sizeof(component) ? length : (sizeof(component) - 1)] = '\0';
        parent = ensure_directory_node(parent, component);
        if (parent < 0) {
            g_last_error = SX_ENOMEM;
            return nullptr;
        }
        if (cursor[length] == '\0') {
            return &g_nodes[parent].vnode;
        }
        cursor += length;
        while (*cursor == '/') {
            ++cursor;
        }
    }
}

bool detach_subtree(const char* path, bool (*referenced)(const Vnode& node)) {
    PathBuffer normalized;
    if (normalized == nullptr) {
        g_last_error = SX_ENOMEM;
        return false;
    }
    if (!g_ready || path == nullptr || !normalize_path("/", path, normalized, normalized.capacity())) {
        g_last_error = SX_EINVAL;
        return false;
    }
    const int root = resolve_index(normalized);
    if (root <= 0 || g_nodes[root].vnode.type != NodeType::directory) {
        g_last_error = SX_EINVAL;
        return false;
    }
    const size_t count = collect_subtree(root);
    // Invalidated nodes count too: an unlinked file can still be open.
    for (size_t index = 0; referenced != nullptr && index < count; ++index) {
        if (referenced(g_nodes[g_subtree[index]].vnode)) {
            g_last_error = SX_EBUSY;
            return false;
        }
    }
    detach_node(root);
    for (size_t index = 0; index < count; ++index) {
        clear_slot(g_subtree[index]);
    }
    return true;
}

Vnode* mkdir(const cred::Credentials& who, const char* path, uint16_t permissions) {
    g_last_error = SX_ENOENT;
    if (!g_ready || path == nullptr || *path == '\0') {
        g_last_error = SX_EINVAL;
        return nullptr;
    }

    PathBuffer normalized;
    if (normalized == nullptr) {
        g_last_error = SX_ENOMEM;
        return nullptr;
    }
    if (!normalize_path("/", path, normalized, normalized.capacity())) {
        g_last_error = SX_EINVAL;
        return nullptr;
    }

    const int existing = resolve_index(normalized);
    if (existing >= 0) {
        if (g_nodes[existing].vnode.type == NodeType::directory) {
            g_last_error = SX_EEXIST;
            return &g_nodes[existing].vnode;
        }
        g_last_error = SX_EEXIST;
        return nullptr;
    }

    PathBuffer parent_path;
    if (parent_path == nullptr) {
        g_last_error = SX_ENOMEM;
        return nullptr;
    }
    char leaf_name[kMaxNameLength] = {};
    if (!split_parent_path(normalized, parent_path, parent_path.capacity(), leaf_name, sizeof(leaf_name))) {
        g_last_error = SX_EINVAL;
        return nullptr;
    }
    const int parent = resolve_index(parent_path);
    if (parent < 0 || g_nodes[parent].vnode.type != NodeType::directory) {
        g_last_error = SX_ENOENT;
        return nullptr;
    }
    if (!may_change_directory(who, parent)) {
        g_last_error = SX_EACCES;
        return nullptr;
    }

    const Ownership owner = {who.euid, who.gid,
                             static_cast<uint16_t>(kModeDirectory | (permissions & kModePermissions))};
    VolumeRef volume = {};
    if (volume_for_path(normalized, volume)) {
        if (volume.driver->create_directory == nullptr || fs::read_only(volume.mount)) {
            g_last_error = kNoWriteOperation;
            return nullptr;
        }
        PathBuffer spelled;
        if (spelled == nullptr) {
            g_last_error = SX_ENOMEM;
            return nullptr;
        }
        if (!spelled_path(parent, leaf_name, spelled, spelled.capacity())) {
            g_last_error = SX_EINVAL;
            return nullptr;
        }
        Vnode* directory = nullptr;
        const fs::Status created = volume.driver->create_directory(volume.volume, spelled, owner, directory);
        if (!fs::ok(created) || directory == nullptr) {
            g_last_error = fs::ok(created) ? SX_ENOSPC : fs::error_of(created);
            return nullptr;
        }
        return directory;
    }

    if (!cred::acts_as_system(who)) {
        g_last_error = SX_EACCES;
        return nullptr;
    }
    Vnode* directory = ensure_directory(normalized);
    if (directory != nullptr) {
        set_ownership(*directory, owner.uid, owner.gid, owner.mode);
    }
    return directory;
}

Vnode* install_external_file(const char* path, Backend backend, void* data, size_t size, bool writable) {
    if (!g_ready || path == nullptr || *path == '\0') {
        g_last_error = SX_EINVAL;
        return nullptr;
    }

    PathBuffer normalized;
    if (normalized == nullptr) {
        g_last_error = SX_ENOMEM;
        return nullptr;
    }
    if (!normalize_path("/", path, normalized, normalized.capacity())) {
        g_last_error = SX_EINVAL;
        return nullptr;
    }

    PathBuffer parent_path;
    if (parent_path == nullptr) {
        g_last_error = SX_ENOMEM;
        return nullptr;
    }
    char leaf_name[kMaxNameLength] = {};
    if (!split_parent_path(normalized, parent_path, parent_path.capacity(), leaf_name, sizeof(leaf_name))) {
        g_last_error = SX_EINVAL;
        return nullptr;
    }

    Vnode* parent = ensure_directory(parent_path);
    if (parent == nullptr || parent->type != NodeType::directory) {
        g_last_error = SX_ENOENT;
        return nullptr;
    }

    const int parent_index = resolve_index(parent_path);
    if (parent_index < 0) {
        g_last_error = SX_ENOENT;
        return nullptr;
    }

    int index = find_child(parent_index, leaf_name);
    if (index < 0) {
        index = add_node(NodeType::file, leaf_name, parent_index);
        if (index < 0) {
            g_last_error = SX_ENOMEM;
            return nullptr;
        }
    }

    Vnode& node = g_nodes[index].vnode;
    node.type = NodeType::file;
    node.backend = backend;
    node.data = data;
    node.size = size;
    node.writable = writable;
    node.volume_driver = nullptr;
    return &node;
}

Vnode* install_volume_file(const char* path, const fs::Driver& driver, void* data, size_t size, bool writable) {
    Vnode* node = install_external_file(path, Backend::volume, data, size, writable);
    if (node != nullptr) {
        node->volume_driver = &driver;
    }
    return node;
}

Vnode* install_volume_directory(const char* path, const fs::Driver& driver) {
    Vnode* node = ensure_directory(path);
    if (node != nullptr && node->volume_driver != &driver) {
        node->volume_driver = &driver;
        node->populated = false;
    }
    return node;
}

Vnode* publish_volume_directory(const char* path, const fs::Driver& driver) {
    Vnode* node = ensure_directory(path);
    if (node != nullptr && node->volume_driver == nullptr) {
        node->volume_driver = &driver;
        node->populated = false;
    }
    return node;
}

bool unlink(const cred::Credentials& who, const char* path) {
    g_last_error = SX_ENOENT;
    if (!g_ready || path == nullptr || *path == '\0' || strcmp(path, "/") == 0) {
        g_last_error = SX_EINVAL;
        return false;
    }

    PathBuffer normalized;
    if (normalized == nullptr) {
        g_last_error = SX_ENOMEM;
        return false;
    }
    if (!normalize_path("/", path, normalized, normalized.capacity())) {
        g_last_error = SX_EINVAL;
        return false;
    }

    const int index = resolve_index(normalized);
    if (index < 0) {
        return false;
    }

    Vnode& node = g_nodes[index].vnode;
    if (node.type == NodeType::directory) {
        g_last_error = SX_EISDIR;
        return false;
    }
    if (!may_change_directory(who, node.parent) || !sticky_allows(who, node.parent, node)) {
        g_last_error = SX_EACCES;
        return false;
    }

    // A file something holds loses its name and keeps its contents for
    // those holders; release() frees it when the last one goes.
    const bool held = node_is_held(node);
    switch (node.backend) {
        case Backend::memory:
            break;
        case Backend::volume: {
            const fs::Driver& driver = *node.volume_driver;
            if (driver.unlink == nullptr || path_is_read_only(normalized)) {
                g_last_error = kNoWriteOperation;
                return false;
            }
            if (held && driver.release == nullptr) {
                g_last_error = SX_EBUSY;
                return false;
            }
            const fs::Status removed = driver.unlink(node, held);
            if (!fs::ok(removed)) {
                g_last_error = removed == fs::Status::unsupported ? SX_EBUSY : fs::error_of(removed);
                return false;
            }
            break;
        }
        default:
            g_last_error = SX_EINVAL;
            return false;
    }
    if (held) {
        node.unlinked = true;
    } else {
        retire_node(node);
    }
    return true;
}

void retain(Vnode& node) {
    if (node.references++ == 0 && node.backend == Backend::volume && node.volume_driver != nullptr &&
        node.volume_driver->open != nullptr) {
        node.volume_driver->open(node);
    }
}

bool is_held(const Vnode& node) {
    return node_is_held(node);
}

void release(Vnode& node) {
    // An unbalanced release must not wrap the count into "held forever".
    if (node.references == 0) {
        return;
    }
    --node.references;
    if (node_is_held(node)) {
        return;
    }
    if (node.backend == Backend::volume && node.volume_driver != nullptr && node.volume_driver->close != nullptr) {
        node.volume_driver->close(node);
    }
    if (!node.unlinked || node.type == NodeType::invalid) {
        return;
    }
    if (node.backend == Backend::volume && node.volume_driver != nullptr &&
        node.volume_driver->release != nullptr) {
        node.volume_driver->release(node);
    }
    retire_node(node);
}

bool rmdir(const cred::Credentials& who, const char* path) {
    g_last_error = SX_ENOENT;
    if (!g_ready || path == nullptr || *path == '\0' || strcmp(path, "/") == 0) {
        g_last_error = SX_EINVAL;
        return false;
    }

    PathBuffer normalized;
    if (normalized == nullptr) {
        g_last_error = SX_ENOMEM;
        return false;
    }
    if (!normalize_path("/", path, normalized, normalized.capacity()) || is_mount_point(normalized)) {
        g_last_error = SX_EINVAL;
        return false;
    }

    const int index = resolve_index(normalized);
    if (index < 0) {
        return false;
    }

    Vnode& node = g_nodes[index].vnode;
    if (node.type != NodeType::directory) {
        g_last_error = SX_ENOTDIR;
        return false;
    }
    if (node_has_valid_children(node)) {
        g_last_error = SX_ENOTEMPTY;
        return false;
    }
    if (!may_change_directory(who, node.parent) || !sticky_allows(who, node.parent, node)) {
        g_last_error = SX_EACCES;
        return false;
    }
    // A directory someone has open, or one a held unlinked file still hangs
    // from, stays until they let go.
    if (node_is_held(node) || node_has_unlinked_children(node)) {
        g_last_error = SX_EBUSY;
        return false;
    }

    VolumeRef volume = {};
    if (volume_for_path(normalized, volume)) {
        if (volume.driver->remove_directory == nullptr || fs::read_only(volume.mount)) {
            g_last_error = kNoWriteOperation;
            return false;
        }
        const fs::Status removed = volume.driver->remove_directory(node);
        if (!fs::ok(removed)) {
            g_last_error = fs::error_of(removed);
            return false;
        }
    }

    retire_node(node);
    return true;
}

bool truncate(const cred::Credentials& who, const char* path, size_t size) {
    g_last_error = SX_ENOENT;
    if (!g_ready || path == nullptr || *path == '\0') {
        g_last_error = SX_EINVAL;
        return false;
    }

    PathBuffer normalized;
    if (normalized == nullptr) {
        g_last_error = SX_ENOMEM;
        return false;
    }
    if (!normalize_path("/", path, normalized, normalized.capacity())) {
        g_last_error = SX_EINVAL;
        return false;
    }

    const int index = resolve_index(normalized);
    if (index < 0) {
        return false;
    }

    Vnode& node = g_nodes[index].vnode;
    if (node.type != NodeType::file) {
        g_last_error = SX_EISDIR;
        return false;
    }
    if (!ancestors_searchable(who, index) || !permits(who, node, kAccessWrite)) {
        g_last_error = SX_EACCES;
        return false;
    }
    if (!node.writable) {
        g_last_error = path_is_read_only(normalized) ? SX_EROFS : SX_EBADF;
        return false;
    }

    switch (node.backend) {
        case Backend::memory: {
            if (node.data == nullptr) {
                g_last_error = SX_EBADF;
                return false;
            }
            if (size > kDynamicFileCapacity) {
                g_last_error = SX_ENOSPC;
                return false;
            }
            if (size > node.size) {
                memset(static_cast<uint8_t*>(node.data) + node.size, 0, size - node.size);
            }
            node.size = size;
            ++node.content_generation;
            return true;
        }
        case Backend::volume: {
            if (node.volume_driver->truncate == nullptr) {
                g_last_error = kNoWriteOperation;
                return false;
            }
            const fs::Status truncated = node.volume_driver->truncate(node, size);
            if (!fs::ok(truncated)) {
                g_last_error = fs::error_of(truncated);
                return false;
            }
            ++node.content_generation;
            return true;
        }
        default:
            g_last_error = SX_EINVAL;
            return false;
    }
}

bool rename(const cred::Credentials& who, const char* old_path, const char* new_path) {
    g_last_error = SX_ENOENT;
    if (!g_ready || old_path == nullptr || new_path == nullptr ||
        *old_path == '\0' || *new_path == '\0' ||
        strcmp(old_path, "/") == 0 || strcmp(new_path, "/") == 0 ||
        strcmp(old_path, new_path) == 0) {
        g_last_error = SX_EINVAL;
        return false;
    }

    PathBuffer normalized_old;
    if (normalized_old == nullptr) {
        g_last_error = SX_ENOMEM;
        return false;
    }
    PathBuffer normalized_new;
    if (normalized_new == nullptr) {
        g_last_error = SX_ENOMEM;
        return false;
    }
    if (!normalize_path("/", old_path, normalized_old, normalized_old.capacity()) ||
        !normalize_path("/", new_path, normalized_new, normalized_new.capacity()) ||
        is_mount_point(normalized_old) || is_mount_point(normalized_new)) {
        g_last_error = SX_EINVAL;
        return false;
    }

    const int node_index = resolve_index(normalized_old);
    if (node_index < 0) {
        return false;
    }
    PathBuffer parent_path;
    if (parent_path == nullptr) {
        g_last_error = SX_ENOMEM;
        return false;
    }
    char leaf_name[kMaxNameLength] = {};
    if (!split_parent_path(normalized_new, parent_path, parent_path.capacity(), leaf_name, sizeof(leaf_name))) {
        g_last_error = SX_EINVAL;
        return false;
    }
    int replaced_index = resolve_index(normalized_new);
    if (replaced_index == node_index) {
        // Two spellings of one name: the /disk alias, nothing to do; or, on
        // a volume that folds case, a new case for the same name, which is
        // a rename that replaces nothing.
        if (strcmp(g_nodes[node_index].name, leaf_name) == 0) {
            return true;
        }
        replaced_index = -1;
    }

    const int new_parent = resolve_index(parent_path);
    if (new_parent < 0) {
        g_last_error = SX_ENOENT;
        return false;
    }
    if (g_nodes[new_parent].vnode.type != NodeType::directory) {
        g_last_error = SX_ENOTDIR;
        return false;
    }

    Vnode& node = g_nodes[node_index].vnode;
    if (node.type == NodeType::directory && node_is_ancestor(node_index, new_parent)) {
        g_last_error = SX_EINVAL;
        return false;
    }
    if (!may_change_directory(who, node.parent) || !sticky_allows(who, node.parent, node) ||
        !may_change_directory(who, new_parent)) {
        g_last_error = SX_EACCES;
        return false;
    }

    // Both ends on the same volume, or both in the in-memory tree.
    if (fs::owner_of(normalized_old) != fs::owner_of(normalized_new)) {
        g_last_error = SX_EXDEV;
        return false;
    }
    VolumeRef volume = {};
    const bool on_volume = volume_for_path(normalized_old, volume);
    if (on_volume && fs::read_only(volume.mount)) {
        g_last_error = kNoWriteOperation;
        return false;
    }

    // What is at the new name is replaced, if the two agree in type and the
    // caller could have removed it.
    Vnode* replaced = replaced_index >= 0 ? &g_nodes[replaced_index].vnode : nullptr;
    bool keep_replaced = false;
    if (replaced != nullptr) {
        if (node.type == NodeType::directory && replaced->type != NodeType::directory) {
            g_last_error = SX_ENOTDIR;
            return false;
        }
        if (node.type != NodeType::directory && replaced->type == NodeType::directory) {
            g_last_error = SX_EISDIR;
            return false;
        }
        if (replaced->type == NodeType::directory && node_has_valid_children(*replaced)) {
            g_last_error = SX_ENOTEMPTY;
            return false;
        }
        if (!sticky_allows(who, new_parent, *replaced)) {
            g_last_error = SX_EACCES;
            return false;
        }
        if (replaced->backend == Backend::device) {
            g_last_error = SX_EINVAL;
            return false;
        }
        const bool held = node_is_held(*replaced);
        if (replaced->type == NodeType::directory && (held || node_has_unlinked_children(*replaced))) {
            g_last_error = SX_EBUSY;
            return false;
        }
        if (on_volume && !volume.driver->replaces_on_rename) {
            g_last_error = SX_EEXIST;
            return false;
        }
        if (held && on_volume && volume.driver->release == nullptr) {
            g_last_error = SX_EBUSY;
            return false;
        }
        keep_replaced = held;
    }

    // Everything the tree needs is checked: once the driver succeeds, moving
    // the node cannot fail, so the disk never gets ahead of the tree.
    if (on_volume) {
        if (volume.driver->rename == nullptr) {
            g_last_error = kNoWriteOperation;
            return false;
        }
        // The driver gets the names as the tree spells them (spelled_path).
        PathBuffer spelled_old;
        if (spelled_old == nullptr) {
            g_last_error = SX_ENOMEM;
            return false;
        }
        PathBuffer spelled_new;
        if (spelled_new == nullptr) {
            g_last_error = SX_ENOMEM;
            return false;
        }
        if (!path_of_node(node, spelled_old, spelled_old.capacity()) ||
            !spelled_path(new_parent, leaf_name, spelled_new, spelled_new.capacity())) {
            g_last_error = SX_EINVAL;
            return false;
        }
        const fs::Status renamed = volume.driver->rename(volume.volume, spelled_old, spelled_new, keep_replaced);
        if (!fs::ok(renamed)) {
            g_last_error = renamed == fs::Status::unsupported ? SX_EBUSY : fs::error_of(renamed);
            return false;
        }
    }

    if (replaced != nullptr) {
        if (keep_replaced) {
            replaced->unlinked = true;
        } else {
            retire_node(*replaced);
        }
    }
    detach_node(node_index);
    (void)set_node_name(node_index, leaf_name);
    attach_node(node_index, new_parent);
    return true;
}

size_t read(Vnode& node, size_t offset, void* buffer, size_t count) {
    g_last_error = 0;
    if (node.type != NodeType::file || buffer == nullptr || count == 0) {
        return 0;
    }

    if (offset >= node.size) {
        return 0;
    }

    const size_t available = node.size - offset;
    const size_t to_copy = available < count ? available : count;
    switch (node.backend) {
        case Backend::memory:
            if (node.data == nullptr) {
                return 0;
            }
            memcpy(buffer, static_cast<const uint8_t*>(node.data) + offset, to_copy);
            return to_copy;
        case Backend::volume: {
            const fs::Status status = node.volume_driver->read(node, offset, buffer, to_copy);
            if (!fs::ok(status)) {
                // Not the end of the file: the caller must not take it for one.
                g_last_error = fs::error_of(status);
                return 0;
            }
            return to_copy;
        }
        default:
            return 0;
    }
}

size_t write(Vnode& node, size_t offset, const void* buffer, size_t count, bool truncate) {
    g_last_error = SX_EINVAL;
    if (node.type != NodeType::file || !node.writable) {
        g_last_error = SX_EBADF;
        return 0;
    }

    switch (node.backend) {
        case Backend::memory: {
            if (node.data == nullptr || buffer == nullptr) {
                g_last_error = SX_EBADF;
                return 0;
            }
            if (truncate) {
                node.size = 0;
                offset = 0;
            }

            if (offset > kDynamicFileCapacity) {
                g_last_error = SX_ENOSPC;
                return 0;
            }

            const size_t available = kDynamicFileCapacity - offset;
            const size_t to_copy = available < count ? available : count;
            memcpy(static_cast<uint8_t*>(node.data) + offset, buffer, to_copy);

            const size_t new_size = offset + to_copy;
            if (new_size > node.size) {
                node.size = new_size;
            }
            ++node.content_generation;
            return to_copy;
        }
        case Backend::volume: {
            size_t written = 0;
            if (node.volume_driver->write == nullptr) {
                g_last_error = kNoWriteOperation;
                return 0;
            }
            const fs::Status status = node.volume_driver->write(node, offset, buffer, count, truncate, written);
            // A truncating write may have emptied the file before it failed.
            ++node.content_generation;
            if (!fs::ok(status) && written == 0) {
                g_last_error = fs::error_of(status);
                return 0;
            }
            return written;
        }
        default:
            g_last_error = SX_EINVAL;
            return 0;
    }
}

namespace {

// The absolute path of a node, rebuilt from its parents. False if it does not
// fit or the node is not in the table.
bool path_of_node(const Vnode& node, char* out, size_t capacity) {
    const NodeSlot* slot = reinterpret_cast<const NodeSlot*>(&node);
    if (slot < g_nodes || slot >= g_nodes + g_node_count || capacity < 2) {
        return false;
    }
    const char* names[kMaxNormalizedPathComponents];
    size_t depth = 0;
    for (int index = static_cast<int>(slot - g_nodes); index > 0; index = g_nodes[index].vnode.parent) {
        if (depth == kMaxNormalizedPathComponents) {
            return false;
        }
        names[depth++] = g_nodes[index].name;
    }
    size_t written = 0;
    out[written++] = '/';
    for (size_t level = depth; level > 0; --level) {
        const size_t length = strlen(names[level - 1]);
        if (written + length + 2 > capacity) {
            return false;
        }
        if (written > 1) {
            out[written++] = '/';
        }
        memcpy(out + written, names[level - 1], length);
        written += length;
    }
    out[written] = '\0';
    return true;
}

// Records ownership on `node`, through the driver of the volume it is on when
// it is on one (a volume's directories are in-memory vnodes, found by path).
bool store_ownership(Vnode& node, const Ownership& owner) {
    PathBuffer path;
    if (path == nullptr) {
        g_last_error = SX_ENOMEM;
        return false;
    }
    if (!path_of_node(node, path, path.capacity())) {
        g_last_error = SX_EINVAL;
        return false;
    }
    VolumeRef volume = {};
    bool on_volume = volume_for_path(path, volume);
    if (on_volume && fs::read_only(volume.mount)) {
        g_last_error = kNoWriteOperation;
        return false;
    }
    if (node.backend == Backend::volume && node.volume_driver != nullptr) {
        // A file carries its driver, which also holds for one whose name is
        // gone (fchmod on an unlinked file).
        volume.driver = node.volume_driver;
        on_volume = true;
    }
    if (on_volume) {
        if (volume.driver->set_ownership == nullptr) {
            g_last_error = SX_EINVAL;
            return false;
        }
        const fs::Status stored = volume.driver->set_ownership(node, owner.uid, owner.gid, owner.mode);
        if (!fs::ok(stored)) {
            g_last_error = fs::error_of(stored);
            return false;
        }
    }
    set_ownership(node, owner.uid, owner.gid, owner.mode);
    return true;
}

int resolve_reachable(const cred::Credentials& who, const char* path) {
    PathBuffer normalized;
    if (normalized == nullptr) {
        g_last_error = SX_ENOMEM;
        return -1;
    }
    if (path == nullptr || !normalize_path("/", path, normalized, normalized.capacity())) {
        g_last_error = SX_EINVAL;
        return -1;
    }
    const int index = resolve_index(normalized);
    if (index < 0) {
        g_last_error = SX_ENOENT;
        return -1;
    }
    if (!ancestors_searchable(who, index)) {
        g_last_error = SX_EACCES;
        return -1;
    }
    return index;
}

} // namespace

bool chmod_node(const cred::Credentials& who, Vnode& node, uint16_t permissions) {
    Ownership owner = ownership_of(node);
    if (!cred::acts_as_system(who) && who.euid != owner.uid) {
        g_last_error = SX_EPERM;
        return false;
    }
    owner.mode = static_cast<uint16_t>((owner.mode & kModeTypeMask) | (permissions & kModePermissions));
    return store_ownership(node, owner);
}

bool chmod(const cred::Credentials& who, const char* path, uint16_t permissions) {
    const int index = resolve_reachable(who, path);
    return index >= 0 && chmod_node(who, g_nodes[index].vnode, permissions);
}

bool chown(const cred::Credentials& who, const char* path, uint32_t uid, uint32_t gid) {
    const int index = resolve_reachable(who, path);
    if (index < 0) {
        return false;
    }
    if (!cred::acts_as_system(who)) {
        g_last_error = SX_EPERM;
        return false;
    }
    Vnode& node = g_nodes[index].vnode;
    Ownership owner = ownership_of(node);
    if (uid != kKeepId) {
        owner.uid = uid;
    }
    if (gid != kKeepId) {
        owner.gid = gid;
    }
    return store_ownership(node, owner);
}

const Vnode* child_at(const Vnode& directory, size_t index) {
    if (!populate_directory(index_of(directory))) {
        return nullptr;
    }
    int child = directory.first_child;
    size_t current = 0;
    while (child >= 0) {
        if (node_is_named(g_nodes[child].vnode)) {
            if (current == index) {
                return &g_nodes[child].vnode;
            }
            ++current;
        }
        child = g_nodes[child].vnode.next_sibling;
    }
    return nullptr;
}

size_t child_count(const Vnode& directory) {
    if (!populate_directory(index_of(directory))) {
        return 0;
    }
    size_t count = 0;
    int child = directory.first_child;
    while (child >= 0) {
        if (node_is_named(g_nodes[child].vnode)) {
            ++count;
        }
        child = g_nodes[child].vnode.next_sibling;
    }
    return count;
}

bool path_of(const Vnode& node, char* out, size_t capacity) {
    return node.type != NodeType::invalid && !node.unlinked && path_of_node(node, out, capacity);
}

/* --- the bounded cache (VFS-C) ---------------------------------------------
 * Published volume directories are given back when the slots run low. What
 * makes it safe is where it runs: trim_if_low() is called at the entry of a
 * system call, under the kernel lock, where no kernel code is halfway through
 * a lookup. A Vnode pointer kept past the call that found it must be retained
 * (vfs::retain); an unretained one is only good until the next system call. */

namespace {

// Free slots below which a system call's entry trims, and how many it frees
// up to when it does.
constexpr size_t kTrimLowWater = kMaxNodes / 8;
constexpr size_t kTrimTarget = kMaxNodes / 4;
// After a trim that could not reach the low-water mark, the entry does not
// try again until this many more directory looks have happened: they are
// what makes something evictable, and a full tree of held or always-resident
// nodes would otherwise be searched on every system call.
constexpr uint32_t kTrimRetryLooks = 64;

bool g_trim_skip[kMaxNodes];
bool g_trim_stalled = false;
uint32_t g_trim_stalled_at = 0;

// A published, on-demand directory with entries, nothing holding it.
bool trim_candidate(int index) {
    const Vnode& node = g_nodes[index].vnode;
    return !g_trim_skip[index] && node.type == NodeType::directory && node.populated && node.first_child >= 0 &&
           node.volume_driver != nullptr && node.volume_driver->populate != nullptr && !node_is_held(node);
}

// The directory nobody looked into for the longest time. The ages are taken
// against the clock, so a wrapped clock still orders them.
int oldest_trim_candidate() {
    int best = -1;
    uint32_t best_age = 0;
    for (size_t index = 1; index < g_node_count; ++index) {
        if (!trim_candidate(static_cast<int>(index))) {
            continue;
        }
        const uint32_t age = g_use_clock - g_nodes[index].vnode.last_used;
        if (best < 0 || age > best_age) {
            best = static_cast<int>(index);
            best_age = age;
        }
    }
    return best;
}

// Everything under the directory can go: nothing in it is held or unlinked,
// it all belongs to the directory's driver, and no other volume is mounted
// inside it. Leaves the subtree in g_subtree, `count` entries.
bool subtree_can_go(int index, size_t& count) {
    const Vnode& directory = g_nodes[index].vnode;
    count = collect_subtree(index);
    if (count >= kMaxNodes) {
        return false;
    }
    for (size_t at = 0; at < count; ++at) {
        const Vnode& node = g_nodes[g_subtree[at]].vnode;
        if (node_is_held(node) || node.unlinked || node.volume_driver != directory.volume_driver) {
            return false;
        }
        if (at != 0 && node.type == NodeType::directory) {
            PathBuffer path;
            if (path == nullptr) {
                g_last_error = SX_ENOMEM;
                return false;
            }
            if (!path_of_node(node, path, path.capacity()) || is_mount_point(path)) {
                return false;
            }
        }
    }
    return true;
}

} // namespace

size_t free_slots() {
    return (kMaxNodes - g_node_count) + g_free_listed;
}

size_t trim(size_t target_free) {
    if (!g_ready) {
        return 0;
    }
    const int saved_error = g_last_error;
    memset(g_trim_skip, 0, sizeof(g_trim_skip));
    size_t given_back = 0;
    while (free_slots() < target_free) {
        const int directory = oldest_trim_candidate();
        if (directory < 0) {
            break;
        }
        g_trim_skip[directory] = true;
        size_t count = 0;
        if (!subtree_can_go(directory, count)) {
            continue;
        }
        const fs::Driver& driver = *g_nodes[directory].vnode.volume_driver;
        // Deepest first: collect_subtree lists a directory before its
        // entries, so walking it backwards empties each chain before the
        // directory that owns it goes.
        for (size_t at = count; at-- > 1;) {
            Vnode& node = g_nodes[g_subtree[at]].vnode;
            if (driver.forget != nullptr) {
                driver.forget(node);
            }
            retire_node(node);
        }
        g_nodes[directory].vnode.populated = false;
        given_back += count - 1;
    }
    g_last_error = saved_error;
    return given_back;
}

void trim_if_low() {
    if (!g_ready || free_slots() >= kTrimLowWater) {
        g_trim_stalled = false;
        return;
    }
    if (g_trim_stalled && g_use_clock - g_trim_stalled_at < kTrimRetryLooks) {
        return;
    }
    trim(kTrimTarget);
    g_trim_stalled = free_slots() < kTrimLowWater;
    g_trim_stalled_at = g_use_clock;
}

} // namespace vfs
