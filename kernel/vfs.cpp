#include "kernel/vfs.hpp"

#include <stdint.h>

#include "kernel/svfs.hpp"
#include "kernel/string.hpp"
#include "shared/syscall.h"

namespace {

constexpr size_t kMaxNodes = 768;
constexpr size_t kMaxNameLength = vfs::kMaxPathComponentLength;
constexpr size_t kMaxDynamicFiles = 32;
constexpr size_t kDynamicFileCapacity = 16384;
constexpr size_t kMaxNormalizedPathComponents = 32;

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
bool g_ready = false;
int g_last_error = SAVANXP_ENOENT;

uint32_t parse_hex_u32(const char* text, size_t count) {
    uint32_t value = 0;
    for (size_t index = 0; index < count; ++index) {
        value <<= 4;
        const char digit = text[index];
        if (digit >= '0' && digit <= '9') {
            value |= static_cast<uint32_t>(digit - '0');
        } else if (digit >= 'a' && digit <= 'f') {
            value |= static_cast<uint32_t>(digit - 'a' + 10);
        } else if (digit >= 'A' && digit <= 'F') {
            value |= static_cast<uint32_t>(digit - 'A' + 10);
        }
    }
    return value;
}

const uint8_t* align4(const uint8_t* cursor, const uint8_t* base) {
    const uintptr_t offset = static_cast<uintptr_t>(cursor - base);
    const uintptr_t aligned = (offset + 3u) & ~static_cast<uintptr_t>(3u);
    return base + aligned;
}

int add_node(vfs::NodeType type, const char* name, int parent) {
    if (g_node_count == kMaxNodes) {
        return -1;
    }

    NodeSlot& slot = g_nodes[g_node_count];
    memset(&slot, 0, sizeof(slot));
    strcpy(slot.name, name);
    slot.vnode.type = type;
    slot.vnode.backend = vfs::Backend::memory;
    slot.vnode.name = slot.name;
    slot.vnode.parent = parent;
    slot.vnode.first_child = -1;
    slot.vnode.next_sibling = -1;

    const int node_index = static_cast<int>(g_node_count++);
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

int find_child(int parent, const char* name) {
    int index = g_nodes[parent].vnode.first_child;
    while (index >= 0) {
        if (g_nodes[index].vnode.type != vfs::NodeType::invalid &&
            strcmp(g_nodes[index].vnode.name, name) == 0) {
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

void add_entry(const char* path, vfs::NodeType type, const void* data, size_t size) {
    char component[kMaxNameLength] = {};
    const char* cursor = path;
    int parent = 0;

    while (*cursor == '/') {
        ++cursor;
    }

    if (*cursor == '\0') {
        return;
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
                return;
            }
            cursor += length + 1;
            continue;
        }

        int node = find_child(parent, component);
        if (node < 0) {
            node = add_node(type, component, parent);
        }
        if (node >= 0) {
            g_nodes[node].vnode.type = type;
            g_nodes[node].vnode.data = const_cast<void*>(data);
            g_nodes[node].vnode.size = size;
            g_nodes[node].vnode.writable = false;
        }
        return;
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
    if (node.backend != vfs::Backend::svfs) {
        return;
    }
    svfs::refresh_vnode(node);
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

bool is_svfs_path(const char* path) {
    return path != nullptr &&
        strncmp(path, "/disk", 5) == 0 &&
        (path[5] == '\0' || path[5] == '/');
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

bool node_has_valid_children(const vfs::Vnode& node) {
    int child = node.first_child;
    while (child >= 0) {
        if (g_nodes[child].vnode.type != vfs::NodeType::invalid) {
            return true;
        }
        child = g_nodes[child].vnode.next_sibling;
    }
    return false;
}

void invalidate_node(vfs::Vnode& node) {
    node.type = vfs::NodeType::invalid;
    node.data = nullptr;
    node.size = 0;
    node.writable = false;
}

} // namespace

namespace vfs {

bool normalize_path(const char* cwd, const char* input, char* output, size_t capacity) {
    if (cwd == nullptr || input == nullptr || output == nullptr || capacity < 2) {
        return false;
    }

    char working[kMaxPathLength] = {};
    size_t working_length = 0;
    if (input[0] == '/') {
        working_length = strlen(input);
        if (working_length >= sizeof(working)) {
            return false;
        }
        memcpy(working, input, working_length + 1);
    } else {
        const size_t cwd_length = strlen(cwd);
        const size_t input_length = strlen(input);
        if (cwd_length + (cwd_length > 1 ? 1u : 0u) + input_length >= sizeof(working)) {
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

    char components[kMaxNormalizedPathComponents][kMaxNameLength] = {};
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
        memcpy(components[component_count], start, length);
        components[component_count][length] = '\0';
        ++component_count;
    }

    size_t written = 0;
    output[written++] = '/';
    for (size_t index = 0; index < component_count; ++index) {
        const size_t length = strlen(components[index]);
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

void initialize(const void* archive, size_t size) {
    memset(g_nodes, 0, sizeof(g_nodes));
    memset(g_dynamic_files, 0, sizeof(g_dynamic_files));
    g_node_count = 0;
    g_ready = false;

    if (add_node(NodeType::directory, "/", -1) != 0) {
        return;
    }

    if (archive == nullptr || size < 110) {
        return;
    }

    const auto* base = static_cast<const uint8_t*>(archive);
    const uint8_t* cursor = base;
    const uint8_t* end = base + size;

    while (cursor + 110 <= end) {
        const char* header = reinterpret_cast<const char*>(cursor);
        if (memcmp(header, "070701", 6) != 0) {
            break;
        }

        const uint32_t mode = parse_hex_u32(header + 14, 8);
        const uint32_t file_size = parse_hex_u32(header + 54, 8);
        const uint32_t name_size = parse_hex_u32(header + 94, 8);
        cursor += 110;

        if (cursor + name_size > end || name_size == 0) {
            break;
        }

        const char* name = reinterpret_cast<const char*>(cursor);
        cursor = align4(cursor + name_size, base);
        if (strcmp(name, "TRAILER!!!") == 0) {
            g_ready = true;
            return;
        }

        if (cursor + file_size > end) {
            break;
        }

        const NodeType type = (mode & 0040000u) != 0 ? NodeType::directory : NodeType::file;
        add_entry(name, type, cursor, file_size);
        cursor = align4(cursor + file_size, base);
    }

    g_ready = true;
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
    char normalized[kMaxPathLength] = {};
    if (!normalize_path("/", path != nullptr ? path : "", normalized, sizeof(normalized))) {
        return nullptr;
    }
    const int index = resolve_index(normalized);
    if (index < 0) {
        return nullptr;
    }
    refresh_external_node(g_nodes[index].vnode);
    return &g_nodes[index].vnode;
}

Vnode* open(const char* path, uint32_t flags) {
    g_last_error = SAVANXP_ENOENT;
    if (!g_ready || path == nullptr || *path == '\0') {
        g_last_error = SAVANXP_EINVAL;
        return nullptr;
    }

    char normalized[kMaxPathLength] = {};
    if (!normalize_path("/", path, normalized, sizeof(normalized))) {
        g_last_error = SAVANXP_EINVAL;
        return nullptr;
    }

    const bool wants_write = (flags & SAVANXP_OPEN_WRITE) != 0;
    const bool wants_create = (flags & SAVANXP_OPEN_CREATE) != 0;
    const bool wants_truncate = (flags & SAVANXP_OPEN_TRUNCATE) != 0;

    int index = resolve_index(normalized);
    if (index >= 0) {
        Vnode& node = g_nodes[index].vnode;
        refresh_external_node(node);
        if ((wants_write || wants_truncate) && node.backend == Backend::memory) {
            if (!attach_dynamic_storage(node, wants_truncate)) {
                g_last_error = SAVANXP_ENOMEM;
                return nullptr;
            }
        } else if ((wants_write || wants_truncate) && node.backend == Backend::svfs) {
            if (!node.writable) {
                g_last_error = SAVANXP_EBADF;
                return nullptr;
            }
            if (wants_truncate) {
                svfs::FileRecord* file = svfs::file_from_vnode(node);
                if (file == nullptr) {
                    g_last_error = SAVANXP_EBADF;
                    return nullptr;
                }
                size_t written = 0;
                if (!svfs::write_file(*file, 0, nullptr, 0, true, written)) {
                    g_last_error = SAVANXP_EINVAL;
                    return nullptr;
                }
                node.size = file->size;
            }
        }
        return &node;
    }

    if (!wants_write || !wants_create) {
        return nullptr;
    }

    char parent_path[kMaxPathLength] = {};
    char leaf_name[kMaxNameLength] = {};
    if (!split_parent_path(normalized, parent_path, sizeof(parent_path), leaf_name, sizeof(leaf_name))) {
        g_last_error = SAVANXP_EINVAL;
        return nullptr;
    }

    index = resolve_index(parent_path);
    if (index < 0) {
        g_last_error = SAVANXP_ENOENT;
        return nullptr;
    }

    Vnode& parent = g_nodes[index].vnode;
    if (parent.type != NodeType::directory) {
        g_last_error = SAVANXP_EINVAL;
        return nullptr;
    }

    if (is_svfs_path(normalized)) {
        svfs::FileRecord* file = svfs::create_file(normalized);
        if (file == nullptr) {
            g_last_error = SAVANXP_ENOSPC;
            return nullptr;
        }
        if (file->vnode != nullptr) {
            return file->vnode;
        }
        Vnode* node = install_external_file(normalized, Backend::svfs, file, file->size, true);
        if (node == nullptr) {
            g_last_error = SAVANXP_ENOMEM;
        }
        return node;
    }

    const int node_index = add_node(NodeType::file, leaf_name, index);
    if (node_index < 0) {
        g_last_error = SAVANXP_ENOMEM;
        return nullptr;
    }

    Vnode& node = g_nodes[node_index].vnode;
    if (!attach_dynamic_storage(node, true)) {
        g_last_error = SAVANXP_ENOMEM;
        return nullptr;
    }

    return &node;
}

Vnode* ensure_directory(const char* path) {
    if (!g_ready || path == nullptr || *path == '\0') {
        g_last_error = SAVANXP_EINVAL;
        return nullptr;
    }

    char normalized[kMaxPathLength] = {};
    if (!normalize_path("/", path, normalized, sizeof(normalized))) {
        g_last_error = SAVANXP_EINVAL;
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
            g_last_error = SAVANXP_ENOMEM;
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

Vnode* mkdir(const char* path) {
    g_last_error = SAVANXP_ENOENT;
    if (!g_ready || path == nullptr || *path == '\0') {
        g_last_error = SAVANXP_EINVAL;
        return nullptr;
    }

    char normalized[kMaxPathLength] = {};
    if (!normalize_path("/", path, normalized, sizeof(normalized))) {
        g_last_error = SAVANXP_EINVAL;
        return nullptr;
    }

    const int existing = resolve_index(normalized);
    if (existing >= 0) {
        if (g_nodes[existing].vnode.type == NodeType::directory) {
            g_last_error = SAVANXP_EEXIST;
            return &g_nodes[existing].vnode;
        }
        g_last_error = SAVANXP_EEXIST;
        return nullptr;
    }

    if (is_svfs_path(normalized)) {
        svfs::FileRecord* directory = svfs::create_directory(normalized);
        if (directory == nullptr) {
            g_last_error = SAVANXP_EINVAL;
            return nullptr;
        }
        return directory->vnode != nullptr ? directory->vnode : ensure_directory(normalized);
    }

    return ensure_directory(normalized);
}

Vnode* install_external_file(const char* path, Backend backend, void* data, size_t size, bool writable) {
    if (!g_ready || path == nullptr || *path == '\0') {
        g_last_error = SAVANXP_EINVAL;
        return nullptr;
    }

    char normalized[kMaxPathLength] = {};
    if (!normalize_path("/", path, normalized, sizeof(normalized))) {
        g_last_error = SAVANXP_EINVAL;
        return nullptr;
    }

    char parent_path[kMaxPathLength] = {};
    char leaf_name[kMaxNameLength] = {};
    if (!split_parent_path(normalized, parent_path, sizeof(parent_path), leaf_name, sizeof(leaf_name))) {
        g_last_error = SAVANXP_EINVAL;
        return nullptr;
    }

    Vnode* parent = ensure_directory(parent_path);
    if (parent == nullptr || parent->type != NodeType::directory) {
        g_last_error = SAVANXP_ENOENT;
        return nullptr;
    }

    const int parent_index = resolve_index(parent_path);
    if (parent_index < 0) {
        g_last_error = SAVANXP_ENOENT;
        return nullptr;
    }

    int index = find_child(parent_index, leaf_name);
    if (index < 0) {
        index = add_node(NodeType::file, leaf_name, parent_index);
        if (index < 0) {
            g_last_error = SAVANXP_ENOMEM;
            return nullptr;
        }
    }

    Vnode& node = g_nodes[index].vnode;
    node.type = NodeType::file;
    node.backend = backend;
    node.data = data;
    node.size = size;
    node.writable = writable;
    return &node;
}

bool unlink(const char* path) {
    g_last_error = SAVANXP_ENOENT;
    if (!g_ready || path == nullptr || *path == '\0' || strcmp(path, "/") == 0) {
        g_last_error = SAVANXP_EINVAL;
        return false;
    }

    char normalized[kMaxPathLength] = {};
    if (!normalize_path("/", path, normalized, sizeof(normalized))) {
        g_last_error = SAVANXP_EINVAL;
        return false;
    }

    const int index = resolve_index(normalized);
    if (index < 0) {
        return false;
    }

    Vnode& node = g_nodes[index].vnode;
    if (node.type == NodeType::directory) {
        g_last_error = SAVANXP_EISDIR;
        return false;
    }

    switch (node.backend) {
        case Backend::memory:
            invalidate_node(node);
            return true;
        case Backend::svfs: {
            svfs::FileRecord* file = svfs::file_from_vnode(node);
            if (file == nullptr || !svfs::unlink_file(*file)) {
                g_last_error = SAVANXP_EINVAL;
                return false;
            }
            invalidate_node(node);
            return true;
        }
        default:
            g_last_error = SAVANXP_EINVAL;
            return false;
    }
}

bool rmdir(const char* path) {
    g_last_error = SAVANXP_ENOENT;
    if (!g_ready || path == nullptr || *path == '\0' || strcmp(path, "/") == 0 || strcmp(path, "/disk") == 0) {
        g_last_error = SAVANXP_EINVAL;
        return false;
    }

    char normalized[kMaxPathLength] = {};
    if (!normalize_path("/", path, normalized, sizeof(normalized))) {
        g_last_error = SAVANXP_EINVAL;
        return false;
    }

    const int index = resolve_index(normalized);
    if (index < 0) {
        return false;
    }

    Vnode& node = g_nodes[index].vnode;
    if (node.type != NodeType::directory) {
        g_last_error = SAVANXP_ENOTDIR;
        return false;
    }
    if (node_has_valid_children(node)) {
        g_last_error = SAVANXP_ENOTEMPTY;
        return false;
    }

    if (is_svfs_path(normalized)) {
        svfs::FileRecord* file = svfs::file_from_vnode(node);
        if (file == nullptr || !svfs::remove_directory(*file)) {
            if (g_last_error == SAVANXP_ENOENT) {
                g_last_error = SAVANXP_EINVAL;
            }
            return false;
        }
    }

    invalidate_node(node);
    return true;
}

bool truncate(const char* path, size_t size) {
    g_last_error = SAVANXP_ENOENT;
    if (!g_ready || path == nullptr || *path == '\0') {
        g_last_error = SAVANXP_EINVAL;
        return false;
    }

    char normalized[kMaxPathLength] = {};
    if (!normalize_path("/", path, normalized, sizeof(normalized))) {
        g_last_error = SAVANXP_EINVAL;
        return false;
    }

    const int index = resolve_index(normalized);
    if (index < 0) {
        return false;
    }

    Vnode& node = g_nodes[index].vnode;
    if (node.type != NodeType::file) {
        g_last_error = SAVANXP_EISDIR;
        return false;
    }
    if (!node.writable) {
        g_last_error = SAVANXP_EBADF;
        return false;
    }

    switch (node.backend) {
        case Backend::memory: {
            if (node.data == nullptr) {
                g_last_error = SAVANXP_EBADF;
                return false;
            }
            if (size > kDynamicFileCapacity) {
                g_last_error = SAVANXP_ENOSPC;
                return false;
            }
            if (size > node.size) {
                memset(static_cast<uint8_t*>(node.data) + node.size, 0, size - node.size);
            }
            node.size = size;
            return true;
        }
        case Backend::svfs: {
            svfs::FileRecord* file = svfs::file_from_vnode(node);
            if (file == nullptr || !svfs::truncate_file(*file, size)) {
                g_last_error = size > node.size ? SAVANXP_ENOSPC : SAVANXP_EINVAL;
                return false;
            }
            node.size = file->size;
            return true;
        }
        default:
            g_last_error = SAVANXP_EINVAL;
            return false;
    }
}

bool rename(const char* old_path, const char* new_path) {
    g_last_error = SAVANXP_ENOENT;
    if (!g_ready || old_path == nullptr || new_path == nullptr ||
        *old_path == '\0' || *new_path == '\0' ||
        strcmp(old_path, "/") == 0 || strcmp(new_path, "/") == 0 ||
        strcmp(old_path, new_path) == 0 ||
        strcmp(old_path, "/disk") == 0 || strcmp(new_path, "/disk") == 0) {
        g_last_error = SAVANXP_EINVAL;
        return false;
    }

    char normalized_old[kMaxPathLength] = {};
    char normalized_new[kMaxPathLength] = {};
    if (!normalize_path("/", old_path, normalized_old, sizeof(normalized_old)) ||
        !normalize_path("/", new_path, normalized_new, sizeof(normalized_new))) {
        g_last_error = SAVANXP_EINVAL;
        return false;
    }

    const int node_index = resolve_index(normalized_old);
    if (node_index < 0) {
        return false;
    }
    if (resolve_index(normalized_new) >= 0) {
        g_last_error = SAVANXP_EEXIST;
        return false;
    }

    char parent_path[kMaxPathLength] = {};
    char leaf_name[kMaxNameLength] = {};
    if (!split_parent_path(normalized_new, parent_path, sizeof(parent_path), leaf_name, sizeof(leaf_name))) {
        g_last_error = SAVANXP_EINVAL;
        return false;
    }

    const int new_parent = resolve_index(parent_path);
    if (new_parent < 0) {
        g_last_error = SAVANXP_ENOENT;
        return false;
    }
    if (g_nodes[new_parent].vnode.type != NodeType::directory) {
        g_last_error = SAVANXP_ENOTDIR;
        return false;
    }
    if (find_child(new_parent, leaf_name) >= 0) {
        g_last_error = SAVANXP_EEXIST;
        return false;
    }

    Vnode& node = g_nodes[node_index].vnode;
    if (node.type == NodeType::directory && node_is_ancestor(node_index, new_parent)) {
        g_last_error = SAVANXP_EINVAL;
        return false;
    }

    if (is_svfs_path(normalized_old) != is_svfs_path(normalized_new)) {
        g_last_error = SAVANXP_EINVAL;
        return false;
    }
    if (is_svfs_path(normalized_old) && !svfs::rename_path(normalized_old, normalized_new)) {
        g_last_error = SAVANXP_EINVAL;
        return false;
    }

    detach_node(node_index);
    if (!set_node_name(node_index, leaf_name)) {
        g_last_error = SAVANXP_EINVAL;
        return false;
    }
    attach_node(node_index, new_parent);
    return true;
}

size_t read(Vnode& node, size_t offset, void* buffer, size_t count) {
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
        case Backend::svfs: {
            svfs::FileRecord* file = svfs::file_from_vnode(node);
            if (file == nullptr || !svfs::read_file(*file, offset, buffer, to_copy)) {
                return 0;
            }
            node.size = file->size;
            return to_copy;
        }
        default:
            return 0;
    }
}

size_t write(Vnode& node, size_t offset, const void* buffer, size_t count, bool truncate) {
    g_last_error = SAVANXP_EINVAL;
    if (node.type != NodeType::file || !node.writable) {
        g_last_error = SAVANXP_EBADF;
        return 0;
    }

    switch (node.backend) {
        case Backend::memory: {
            if (node.data == nullptr || buffer == nullptr) {
                g_last_error = SAVANXP_EBADF;
                return 0;
            }
            if (truncate) {
                node.size = 0;
                offset = 0;
            }

            if (offset > kDynamicFileCapacity) {
                g_last_error = SAVANXP_ENOSPC;
                return 0;
            }

            const size_t available = kDynamicFileCapacity - offset;
            const size_t to_copy = available < count ? available : count;
            memcpy(static_cast<uint8_t*>(node.data) + offset, buffer, to_copy);

            const size_t new_size = offset + to_copy;
            if (new_size > node.size) {
                node.size = new_size;
            }
            return to_copy;
        }
        case Backend::svfs: {
            svfs::FileRecord* file = svfs::file_from_vnode(node);
            if (file == nullptr) {
                g_last_error = SAVANXP_EBADF;
                return 0;
            }
            size_t written = 0;
            if (!svfs::write_file(*file, offset, buffer, count, truncate, written)) {
                g_last_error = SAVANXP_ENOSPC;
                return 0;
            }
            node.size = file->size;
            return written;
        }
        default:
            g_last_error = SAVANXP_EINVAL;
            return 0;
    }
}

const Vnode* child_at(const Vnode& directory, size_t index) {
    int child = directory.first_child;
    size_t current = 0;
    while (child >= 0) {
        if (g_nodes[child].vnode.type != NodeType::invalid) {
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
    size_t count = 0;
    int child = directory.first_child;
    while (child >= 0) {
        if (g_nodes[child].vnode.type != NodeType::invalid) {
            ++count;
        }
        child = g_nodes[child].vnode.next_sibling;
    }
    return count;
}

} // namespace vfs
