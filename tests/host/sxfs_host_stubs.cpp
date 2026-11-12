#include "sxfs_host_stubs.hpp"

#include "kernel/block.hpp"
#include "kernel/fs.hpp"
#include "kernel/rtc.hpp"
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
// 0 = disarmed; otherwise the write that brings it to 1 fails.
size_t g_fail_write_countdown = 0;
size_t g_writes_issued = 0;
size_t g_reads_issued = 0;
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
constexpr size_t kMaxPendingWrites = 16384;
constexpr size_t kPendingArenaBytes = 16u << 20;
PendingWrite g_pending[kMaxPendingWrites] = {};
size_t g_pending_count = 0;
uint8_t g_pending_arena[kPendingArenaBytes];
size_t g_pending_arena_used = 0;

// Sectors the flush in progress has already landed.
constexpr uint32_t kMaxLandedSectors = 1u << 21;
uint8_t g_landed[kMaxLandedSectors / 8u];

void drop_pending_writes() {
    g_pending_count = 0;
    g_pending_arena_used = 0;
}

bool g_fail_reads = false;

#ifndef SXFS_HOST_REAL_VFS
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
#endif // SXFS_HOST_REAL_VFS

} // namespace

namespace hoststub {

void attach_device(uint8_t* image, uint32_t sector_count, bool writable) {
    g_device.image = image;
    g_device.sector_count = sector_count;
    g_device.present = image != nullptr && sector_count > 0;
    g_device.writable = writable;
    g_rejected_writes = 0;
    g_pending_write_failures = 0;
    g_fail_write_countdown = 0;
    g_writes_issued = 0;
    g_reads_issued = 0;
    g_fail_reads = false;
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
    g_fail_write_countdown = 0;
    g_writes_issued = 0;
    g_reads_issued = 0;
    g_fail_reads = false;
    g_power_cut_armed = false;
    g_power_cut_remaining = 0;
    g_sectors_written = 0;
    g_flushes = 0;
    drop_pending_writes();
}

void fail_next_writes(size_t count) {
    g_pending_write_failures = count;
}

void fail_write_number(size_t ordinal) {
    g_fail_write_countdown = ordinal;
}

size_t writes_issued() {
    return g_writes_issued;
}

size_t reads_issued() {
    return g_reads_issued;
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

void fail_reads(bool fail) {
    g_fail_reads = fail;
}

#ifndef SXFS_HOST_REAL_VFS
void reset_vfs() {
    memset(g_nodes, 0, sizeof(g_nodes));
    g_node_count = 0;
}

// What the real VFS does on a lookup: every directory above `path` that a
// driver publishes on demand is published first (fs::Driver::populate).
bool populate_path(const char* path) {
    char prefix[kPathCapacity] = {};
    const size_t length = path != nullptr ? strlen(path) : 0;
    if (length == 0 || length >= sizeof(prefix)) {
        return false;
    }
    // end == 0 stands for "/" itself, the directory above everything.
    for (size_t end = 0; end < length; ++end) {
        if (end != 0 && path[end] != '/') {
            continue;
        }
        if (end == 0) {
            strcpy(prefix, "/");
        } else {
            memcpy(prefix, path, end);
            prefix[end] = '\0';
        }
        // The stub has no entries for the directories above a mount point.
        Entry* directory = find_entry(prefix);
        if (directory == nullptr) {
            continue;
        }
        vfs::Vnode& node = directory->node;
        if (node.type == vfs::NodeType::directory && node.volume_driver != nullptr && !node.populated &&
            node.volume_driver->populate != nullptr) {
            node.populated = true;
            if (!fs::ok(node.volume_driver->populate(node))) {
                node.populated = false;
                return false;
            }
        }
    }
    return true;
}

vfs::Vnode* find_node(const char* path) {
    if (!populate_path(path)) {
        return nullptr;
    }
    Entry* entry = find_entry(path);
    return entry != nullptr ? &entry->node : nullptr;
}

size_t node_count() {
    return g_node_count;
}
#endif // SXFS_HOST_REAL_VFS

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
    ++g_reads_issued;
    if (buffer == nullptr || g_fail_reads || !range_ok(index, lba, sector_count)) {
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
    ++g_writes_issued;
    if (g_fail_write_countdown > 0 && --g_fail_write_countdown == 0) {
        ++g_pending_write_failures;
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
    // Reordering is between different sectors only: two cached writes of one
    // sector are one cache line, and the later one is what the medium gets.
    // Walking backwards, a sector a later write already landed is skipped.
    memset(g_landed, 0, sizeof(g_landed));
    for (size_t step = g_pending_count; step > 0 && complete; --step) {
        const PendingWrite& pending = g_pending[step - 1u];
        for (uint32_t sector = 0; sector < pending.sector_count; ++sector) {
            const uint32_t lba = pending.lba + sector;
            if (lba < kMaxLandedSectors && (g_landed[lba / 8u] & (1u << (lba % 8u))) != 0) {
                continue;
            }
            if (g_power_cut_armed && g_power_cut_remaining == 0) {
                complete = false;
                break;
            }
            memcpy(g_device.image + byte_offset(lba),
                   g_pending_arena + pending.arena_offset + byte_offset(sector), byte_length(1));
            if (lba < kMaxLandedSectors) {
                g_landed[lba / 8u] |= static_cast<uint8_t>(1u << (lba % 8u));
            }
            ++g_sectors_written;
            if (g_power_cut_armed) {
                --g_power_cut_remaining;
            }
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

void report_health(bool) {
}

// One device here: draining everything is flushing it.
bool sync_all() {
    return flush(0);
}

} // namespace block

// --- rtc:: ------------------------------------------------------------------
// A fixed date: the volume tests compare metadata, not times.

namespace rtc {

uint64_t epoch_seconds() {
    return 1700000000u;
}

} // namespace rtc

// --- vfs:: ------------------------------------------------------------------
// The driver tests publish into this table. A test that links the real
// kernel/vfs.cpp defines SXFS_HOST_REAL_VFS and leaves it out.

#ifndef SXFS_HOST_REAL_VFS

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
    entry->node.volume_driver = nullptr;
    return &entry->node;
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

bool path_of(const Vnode& node, char* out, size_t capacity) {
    for (size_t index = 0; index < g_node_count; ++index) {
        if (g_nodes[index].in_use && &g_nodes[index].node == &node) {
            if (strlen(g_nodes[index].path) >= capacity) {
                return false;
            }
            strcpy(out, g_nodes[index].path);
            return true;
        }
    }
    return false;
}

void set_ownership(Vnode& node, uint32_t uid, uint32_t gid, uint16_t mode) {
    node.uid = uid;
    node.gid = gid;
    node.mode = mode;
}

} // namespace vfs

#endif // SXFS_HOST_REAL_VFS

// --- block::, the part only the mount registry asks for ------------------------

namespace block {

bool ready() {
    return true;
}

size_t device_count() {
    return 1;
}

} // namespace block
