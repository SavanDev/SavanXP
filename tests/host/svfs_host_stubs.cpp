#include "svfs_host_stubs.hpp"

#include "kernel/block.hpp"
#include "kernel/string.hpp"

// Este TU implementa a mano las funciones de kernel/string.hpp (que en el
// kernel viven en runtime.cpp) a proposito, y NO incluye <string.h>: las del
// header del kernel tienen linkage C++ y chocarian con las declaraciones C de
// la libc del host. Las mem* si son extern "C" y las pone la libc.

namespace {

struct Device {
    uint8_t* image;
    uint32_t sector_count;
    bool present;
    bool writable;
};

Device g_device = {};
size_t g_rejected_writes = 0;
size_t g_pending_write_failures = 0;

constexpr size_t kMaxNodes = 256;
constexpr size_t kPathCapacity = 264;

struct Entry {
    bool in_use;
    char path[kPathCapacity];
    vfs::Vnode node;
};

Entry g_nodes[kMaxNodes] = {};
size_t g_node_count = 0;

Entry* find_entry(const char* path) {
    for (size_t index = 0; index < g_node_count; ++index) {
        if (g_nodes[index].in_use && strcmp(g_nodes[index].path, path) == 0) {
            return &g_nodes[index];
        }
    }
    return nullptr;
}

// Nombre = ultimo componente de la ruta, apuntando dentro del propio buffer.
const char* leaf_of(const char* path) {
    const char* leaf = path;
    for (const char* cursor = path; *cursor != 0; ++cursor) {
        if (*cursor == '/') {
            leaf = cursor + 1;
        }
    }
    return leaf;
}

Entry* intern(const char* path) {
    Entry* existing = find_entry(path);
    if (existing != nullptr) {
        return existing;
    }
    if (path == nullptr || strlen(path) >= kPathCapacity || g_node_count >= kMaxNodes) {
        return nullptr;
    }
    Entry& entry = g_nodes[g_node_count++];
    entry.in_use = true;
    strcpy(entry.path, path);
    entry.node = {};
    entry.node.name = leaf_of(entry.path);
    return &entry;
}

} // namespace

namespace hoststub {

void attach_device(uint8_t* image, uint32_t sector_count, bool writable) {
    g_device.image = image;
    g_device.sector_count = sector_count;
    g_device.present = image != nullptr && sector_count > 0;
    g_device.writable = writable;
    g_rejected_writes = 0;
    g_pending_write_failures = 0;
}

void detach_device() {
    g_device = {};
    g_rejected_writes = 0;
    g_pending_write_failures = 0;
}

void fail_next_writes(size_t count) {
    g_pending_write_failures = count;
}

size_t rejected_writes() {
    return g_rejected_writes;
}

void reset_vfs() {
    memset(g_nodes, 0, sizeof(g_nodes));
    g_node_count = 0;
}

vfs::Vnode* find_node(const char* path) {
    Entry* entry = find_entry(path);
    return entry != nullptr ? &entry->node : nullptr;
}

size_t node_count() {
    return g_node_count;
}

} // namespace hoststub

// --- block:: ----------------------------------------------------------------
// Solo lo que usa el driver. La validacion de rango y del flag de solo-lectura
// la hace el core de block:: en el kernel, asi que se replica aca.

namespace {

bool range_ok(size_t index, uint32_t lba, uint32_t sector_count) {
    if (index != 0 || !g_device.present || sector_count == 0) {
        return false;
    }
    return static_cast<uint64_t>(lba) + sector_count <= g_device.sector_count;
}

size_t byte_offset(uint32_t lba) {
    return static_cast<size_t>(lba) * block::kSectorSize;
}

size_t byte_length(uint32_t sector_count) {
    return static_cast<size_t>(sector_count) * block::kSectorSize;
}

} // namespace

namespace block {

bool device_info(size_t index, DeviceInfo& info) {
    if (index != 0) {
        return false;
    }
    info.present = g_device.present;
    info.sector_count = g_device.sector_count;
    info.writable = g_device.writable;
    info.name = "hoststub0";
    return true;
}

bool read(size_t index, uint32_t lba, uint32_t sector_count, void* buffer) {
    if (buffer == nullptr || !range_ok(index, lba, sector_count)) {
        return false;
    }
    memcpy(buffer, g_device.image + byte_offset(lba), byte_length(sector_count));
    return true;
}

bool write(size_t index, uint32_t lba, uint32_t sector_count, const void* buffer) {
    if (buffer == nullptr || !range_ok(index, lba, sector_count)) {
        return false;
    }
    if (!g_device.writable || g_pending_write_failures > 0) {
        if (g_pending_write_failures > 0) {
            --g_pending_write_failures;
        }
        ++g_rejected_writes;
        return false;
    }
    memcpy(g_device.image + byte_offset(lba), buffer, byte_length(sector_count));
    return true;
}

} // namespace block

// --- vfs:: ------------------------------------------------------------------

namespace vfs {

Vnode* ensure_directory(const char* path) {
    Entry* entry = intern(path);
    if (entry == nullptr) {
        return nullptr;
    }
    entry->node.type = NodeType::directory;
    entry->node.backend = Backend::memory;
    return &entry->node;
}

Vnode* install_external_file(const char* path, Backend backend, void* data, size_t size, bool writable) {
    Entry* entry = intern(path);
    if (entry == nullptr) {
        return nullptr;
    }
    entry->node.type = NodeType::file;
    entry->node.backend = backend;
    entry->node.data = data;
    entry->node.size = size;
    entry->node.writable = writable;
    return &entry->node;
}

} // namespace vfs

// --- kernel/string.hpp ------------------------------------------------------

size_t strlen(const char* text) {
    size_t length = 0;
    while (text[length] != 0) {
        ++length;
    }
    return length;
}

int strcmp(const char* left, const char* right) {
    while (*left != 0 && *left == *right) {
        ++left;
        ++right;
    }
    return static_cast<int>(static_cast<unsigned char>(*left)) -
        static_cast<int>(static_cast<unsigned char>(*right));
}

int strncmp(const char* left, const char* right, size_t count) {
    for (size_t index = 0; index < count; ++index) {
        const unsigned char a = static_cast<unsigned char>(left[index]);
        const unsigned char b = static_cast<unsigned char>(right[index]);
        if (a != b || a == 0) {
            return static_cast<int>(a) - static_cast<int>(b);
        }
    }
    return 0;
}

char* strcpy(char* destination, const char* source) {
    char* cursor = destination;
    while ((*cursor++ = *source++) != 0) {
    }
    return destination;
}
