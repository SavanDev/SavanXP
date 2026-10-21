#include "sxfs_host_stubs.hpp"

#include "kernel/block.hpp"
#include "kernel/string.hpp"

// Las funciones de kernel/string.hpp que usa este TU las pone
// kernel_string_stubs.cpp, compartido por todos los tests de host. Aca NO se
// incluye <string.h> a proposito: las del header del kernel tienen linkage C++
// y chocarian con las declaraciones C de la libc. Las mem* si son extern "C" y
// las pone la libc.

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
bool g_power_cut_armed = false;
size_t g_power_cut_remaining = 0;
size_t g_sectors_written = 0;
size_t g_flushes = 0;

// The device's volatile write cache: block::write_cached queues here, reads see
// the queue, and block::flush lands it on the image. Losing power drops it.
struct PendingWrite {
    uint32_t lba;
    uint32_t sector_count;
    size_t arena_offset;
};
constexpr size_t kMaxPendingWrites = 8192;
constexpr size_t kPendingArenaBytes = 4u << 20;
PendingWrite g_pending[kMaxPendingWrites] = {};
size_t g_pending_count = 0;
uint8_t g_pending_arena[kPendingArenaBytes];
size_t g_pending_arena_used = 0;

void drop_pending_writes() {
    g_pending_count = 0;
    g_pending_arena_used = 0;
}

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
    g_power_cut_armed = false;
    g_power_cut_remaining = 0;
    g_sectors_written = 0;
    g_flushes = 0;
    drop_pending_writes();
}

void detach_device() {
    g_device = {};
    g_rejected_writes = 0;
    g_pending_write_failures = 0;
    g_power_cut_armed = false;
    g_power_cut_remaining = 0;
    g_sectors_written = 0;
    g_flushes = 0;
    drop_pending_writes();
}

void fail_next_writes(size_t count) {
    g_pending_write_failures = count;
}

size_t rejected_writes() {
    return g_rejected_writes;
}

void power_cut_after(size_t sectors) {
    g_power_cut_armed = true;
    g_power_cut_remaining = sectors;
}

void clear_power_cut() {
    g_power_cut_armed = false;
    g_power_cut_remaining = 0;
}

size_t sectors_written() {
    return g_sectors_written;
}

size_t flushes() {
    return g_flushes;
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
    auto* out = static_cast<uint8_t*>(buffer);
    memcpy(out, g_device.image + byte_offset(lba), byte_length(sector_count));
    // A real device answers reads from its cache, so queued writes are seen,
    // later ones over earlier ones.
    for (size_t index_pending = 0; index_pending < g_pending_count; ++index_pending) {
        const PendingWrite& pending = g_pending[index_pending];
        const uint32_t first = pending.lba > lba ? pending.lba : lba;
        const uint32_t pending_end = pending.lba + pending.sector_count;
        const uint32_t read_end = lba + sector_count;
        const uint32_t last = pending_end < read_end ? pending_end : read_end;
        if (first >= last) {
            continue;
        }
        memcpy(out + byte_offset(first - lba),
               g_pending_arena + pending.arena_offset + byte_offset(first - pending.lba),
               byte_length(last - first));
    }
    return true;
}

bool write_cached(size_t index, uint32_t lba, uint32_t sector_count, const void* buffer) {
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
    if (g_pending_count == kMaxPendingWrites ||
        byte_length(sector_count) > kPendingArenaBytes - g_pending_arena_used) {
        return false; // the stub's cache is full: a test bug, not a device one
    }
    PendingWrite& pending = g_pending[g_pending_count++];
    pending.lba = lba;
    pending.sector_count = sector_count;
    pending.arena_offset = g_pending_arena_used;
    memcpy(g_pending_arena + g_pending_arena_used, buffer, byte_length(sector_count));
    g_pending_arena_used += byte_length(sector_count);
    return true;
}

// Lands the queue on the image in REVERSE order: the worst reordering a cache
// may do, so a power cut in the middle of a flush leaves exactly what a missing
// ordering point would let through. The cut counts sectors as they land.
bool flush(size_t index) {
    if (index != 0 || !g_device.present || !g_device.writable) {
        return false;
    }
    ++g_flushes;
    bool complete = true;
    for (size_t step = g_pending_count; step > 0; --step) {
        const PendingWrite& pending = g_pending[step - 1u];
        uint32_t landed = pending.sector_count;
        if (g_power_cut_armed && g_power_cut_remaining < landed) {
            landed = static_cast<uint32_t>(g_power_cut_remaining);
        }
        if (landed != 0) {
            memcpy(g_device.image + byte_offset(pending.lba),
                   g_pending_arena + pending.arena_offset, byte_length(landed));
        }
        g_sectors_written += landed;
        if (g_power_cut_armed) {
            g_power_cut_remaining -= landed;
        }
        if (landed != pending.sector_count) {
            complete = false;
            break;
        }
    }
    drop_pending_writes();
    if (!complete) {
        ++g_rejected_writes;
    }
    return complete;
}

bool write(size_t index, uint32_t lba, uint32_t sector_count, const void* buffer) {
    return write_cached(index, lba, sector_count, buffer) && flush(index);
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
