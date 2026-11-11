#include "kernel/block.hpp"

#include <stdint.h>

#include "kernel/console.hpp"
#include "kernel/heap.hpp"
#include "kernel/string.hpp"

namespace {

// Four ATA slots, four AHCI ports, virtio-blk and the LiveCD's ramdisk, plus
// the partitions partition:: cuts out of them (up to 16), with room left for
// new drivers without touching the core.
constexpr size_t kMaxBlockDevices = 32;

struct Device {
    bool present;
    uint32_t id;
    const block::DeviceOps* ops;
    void* context;
    uint32_t sector_count;
    bool writable;
    const char* name;
    const block::DeviceIdentity* identity;
    // Writeback state: the device took writes since its last flush, or a
    // background write failed and the queue for it is stuck (see fail()).
    bool unflushed;
    bool failed;
};

Device g_devices[kMaxBlockDevices] = {};
size_t g_device_count = 0;
uint32_t g_next_id = 1;
uint32_t g_generation = 1;
bool g_ready = false;

const block::Driver* g_drivers[block::kMaxDrivers] = {};
size_t g_driver_count = 0;

bool device_for_index(size_t index, Device*& device) {
    if (index >= g_device_count) {
        device = nullptr;
        return false;
    }
    device = &g_devices[index];
    return device->present;
}

// Validacion comun a cualquier medio. La hace el core una sola vez y los
// drivers se quedan solo con lo suyo: el tope de 255 sectores por comando, por
// ejemplo, es del protocolo ATA y no del contrato de block::.
bool request_in_range(const Device& device, uint32_t lba, uint32_t sector_count, const void* buffer) {
    if (!device.present || device.ops == nullptr || buffer == nullptr || sector_count == 0) {
        return false;
    }
    return lba < device.sector_count && sector_count <= (device.sector_count - lba);
}

// --- Writeback queue ----------------------------------------------------
//
// One FIFO for every device with deferred_writes. A record is a write (its
// data copied in) or an ordering point (sector_count == 0). Records reach their
// device in queue order, and an ordering point becomes a device flush, so the
// medium sees exactly the order a synchronous caller would have produced, only
// later. See docs/kernel/BLOCK_WRITEBACK.md.

struct Pending {
    Pending* next;
    size_t device;
    uint32_t lba;
    uint32_t sector_count; // 0: an ordering point
    uint32_t written;      // sectors already on the device, for a record split across steps
    uint64_t queued_ms;
    uint64_t ticket;
    uint8_t* data;         // right after the record, in the same allocation
};

// Beyond this a writer waits, draining the queue itself, so a fast writer
// cannot fill kernel memory faster than the disk empties it.
constexpr uint64_t kMaxQueuedBytes = 8u * 1024u * 1024u;
// One step moves at most this much, so an idle CPU hands the processor back
// within one device command.
constexpr uint32_t kStepSectors = 128;
// A queue nobody drains because no CPU goes idle is drained from the timer:
// once the oldest record is this old, one step per kForcedStepIntervalMs.
constexpr uint64_t kMaxAgeMs = 1000;
constexpr uint64_t kForcedStepIntervalMs = 100;

Pending* g_queue_head = nullptr;
Pending* g_queue_tail = nullptr;
uint64_t g_clock_ms = 0;
uint64_t g_last_forced_ms = 0;
block::WritebackStats g_stats = {};
uint64_t g_last_ticket = 0, g_completed_ticket = 0;
uint8_t g_stage[static_cast<size_t>(kStepSectors) * block::kSectorSize];

bool defers(const Device& device) {
    return device.ops->deferred_writes;
}

// --- Read cache ---------------------------------------------------------
//
// Small reads of a physical disk (a directory block, an ELF header, 64 bytes
// of a section table) are answered from memory the second time. Direct
// mapped, one sector per slot, for devices with deferred_writes only: a
// partition forwards to its disk and is served by the disk's slots, so one
// sector has one copy whatever name it is read under.
//
// Coherence is one rule: a slot always holds the newest bytes ISSUED for its
// sector, queued or landed. Every write to such a device passes through
// write_cached(), which refreshes the slots it covers before it returns, and
// a miss fills its slots from the device with the queue laid over it. So a
// hit needs no look at the queue. See docs/kernel/BLOCK_WRITEBACK.md.
constexpr size_t kCacheSlots = 2048; // 1 MiB of sectors
// Longer requests are bulk file data: they bypass the cache instead of
// evicting the small records it is for.
constexpr uint32_t kCacheMaxRequest = 8;

struct CacheSlot {
    bool valid;
    uint16_t device;
    uint32_t lba;
};

CacheSlot g_cache_slots[kCacheSlots] = {};
uint8_t g_cache_data[kCacheSlots][block::kSectorSize];
block::ReadCacheStats g_cache_stats = {};

size_t cache_slot_for(size_t device, uint32_t lba) {
    return (static_cast<size_t>(lba) * 2654435761u + device * 40503u) % kCacheSlots;
}

const uint8_t* cache_find(size_t device, uint32_t lba) {
    const size_t slot = cache_slot_for(device, lba);
    const CacheSlot& entry = g_cache_slots[slot];
    return entry.valid && entry.device == device && entry.lba == lba ? g_cache_data[slot] : nullptr;
}

void cache_store(size_t device, uint32_t lba, const uint8_t* sector) {
    const size_t slot = cache_slot_for(device, lba);
    g_cache_slots[slot] = {true, static_cast<uint16_t>(device), lba};
    memcpy(g_cache_data[slot], sector, block::kSectorSize);
}

// A write: the slots that hold one of its sectors get the new bytes. A small
// write also takes the slots it does not hold, since what was just written is
// what is read next (a directory block, then its lookup).
void cache_note_write(size_t device, uint32_t lba, uint32_t sector_count, const uint8_t* data) {
    for (uint32_t offset = 0; offset < sector_count; ++offset) {
        const uint8_t* sector = data + static_cast<size_t>(offset) * block::kSectorSize;
        if (sector_count <= kCacheMaxRequest || cache_find(device, lba + offset) != nullptr) {
            cache_store(device, lba + offset, sector);
        }
    }
}

void cache_forget_device(size_t device) {
    for (CacheSlot& entry : g_cache_slots) {
        if (entry.valid && entry.device == device) {
            entry.valid = false;
        }
    }
}

void enqueue(Pending* record) {
    record->next = nullptr;
    record->queued_ms = g_clock_ms;
    record->ticket = ++g_last_ticket;
    if (g_queue_tail != nullptr) {
        g_queue_tail->next = record;
    } else {
        g_queue_head = record;
    }
    g_queue_tail = record;
}

void pop_head() {
    Pending* record = g_queue_head;
    g_queue_head = record->next;
    if (g_queue_head == nullptr) {
        g_queue_tail = nullptr;
    }
    g_stats.queued_bytes -= static_cast<uint64_t>(record->sector_count - record->written) * block::kSectorSize;
    g_completed_ticket = record->ticket;
    heap::free(record);
}

// A device that refused a background write keeps its queue: dropping the
// record and going on would let later records land without it, which is the
// one thing the ordering promises not to do. Its writers start failing.
void fail(Device& device, const char* what) {
    if (!device.failed) {
        console::printf("block: %s: background %s failed; queued writes kept, device now refuses writes\n",
            device.name, what);
    }
    device.failed = true;
    ++g_stats.failures;
}

bool step() {
    Pending* record = g_queue_head;
    if (record == nullptr) {
        return false;
    }
    Device& device = g_devices[record->device];
    if (device.failed) {
        return false;
    }
    ++g_stats.steps;

    if (record->sector_count == 0) {
        if (device.unflushed && device.ops->flush != nullptr) {
            if (!device.ops->flush(device.context)) {
                fail(device, "flush");
                return false;
            }
            ++g_stats.device_flushes;
        }
        device.unflushed = false;
        pop_head();
        return true;
    }

    // The head record, from where an earlier step left it, plus any whole
    // records right behind it that continue it on the same device, up to one
    // step's worth. A lone record is written from its own buffer.
    uint32_t remaining = record->sector_count - record->written;
    uint32_t count = remaining < kStepSectors ? remaining : kStepSectors;
    const uint32_t lba = record->lba + record->written;
    const uint8_t* source = record->data + static_cast<size_t>(record->written) * block::kSectorSize;
    size_t merged = 0;
    if (count == remaining) {
        for (Pending* next = record->next; next != nullptr && next->sector_count != 0 &&
             next->device == record->device && next->lba == lba + count &&
             count + next->sector_count <= kStepSectors; next = next->next) {
            if (merged == 0) {
                memcpy(g_stage, source, static_cast<size_t>(count) * block::kSectorSize);
                source = g_stage;
            }
            memcpy(g_stage + static_cast<size_t>(count) * block::kSectorSize, next->data,
                static_cast<size_t>(next->sector_count) * block::kSectorSize);
            count += next->sector_count;
            ++merged;
        }
    }

    if (!device.ops->write(device.context, lba, count, source)) {
        fail(device, "write");
        return false;
    }
    ++g_stats.device_writes;
    device.unflushed = true;

    if (count < remaining) {
        record->written += count;
        g_stats.queued_bytes -= static_cast<uint64_t>(count) * block::kSectorSize;
        return true;
    }
    pop_head();
    for (size_t index = 0; index < merged; ++index) {
        pop_head();
    }
    return true;
}

// Drains until `bytes` more fit in the queue. False if the queue is stuck.
bool make_room(uint64_t bytes) {
    bool waited = false;
    while (g_queue_head != nullptr && g_stats.queued_bytes + bytes > kMaxQueuedBytes) {
        waited = true;
        if (!step()) {
            return false;
        }
    }
    if (waited) {
        ++g_stats.forced_drains;
    }
    return true;
}

bool drain_all() {
    while (g_queue_head != nullptr) {
        if (!step()) {
            return false;
        }
    }
    bool ok = true;
    for (size_t index = 0; index < g_device_count; ++index) {
        Device& device = g_devices[index];
        if (!device.present || !defers(device) || !device.unflushed) {
            continue;
        }
        if (device.ops->flush != nullptr && !device.ops->flush(device.context)) {
            fail(device, "flush");
            ok = false;
            continue;
        }
        ++g_stats.device_flushes;
        device.unflushed = false;
    }
    return ok;
}

// Lays every queued write for `index` that overlaps the range over what the
// device returned, oldest first, so the newest copy of each sector wins.
void overlay_queued(size_t index, uint32_t lba, uint32_t sector_count, uint8_t* buffer) {
    const uint32_t end = lba + sector_count;
    for (Pending* record = g_queue_head; record != nullptr; record = record->next) {
        if (record->device != index || record->sector_count == 0) {
            continue;
        }
        const uint32_t record_end = record->lba + record->sector_count;
        const uint32_t first = record->lba > lba ? record->lba : lba;
        const uint32_t last = record_end < end ? record_end : end;
        if (first >= last) {
            continue;
        }
        memcpy(buffer + static_cast<size_t>(first - lba) * block::kSectorSize,
            record->data + static_cast<size_t>(first - record->lba) * block::kSectorSize,
            static_cast<size_t>(last - first) * block::kSectorSize);
    }
}

bool queue_has_device(size_t index) {
    for (Pending* record = g_queue_head; record != nullptr; record = record->next) {
        if (record->device == index) {
            return true;
        }
    }
    return false;
}

} // namespace

namespace block {

bool register_driver(const Driver& driver) {
    if (g_driver_count >= kMaxDrivers || driver.enumerate == nullptr) {
        return false;
    }
    g_drivers[g_driver_count] = &driver;
    ++g_driver_count;
    return true;
}

size_t register_device(
    const DeviceOps& ops,
    void* context,
    uint32_t sector_count,
    bool writable,
    const char* name,
    const DeviceIdentity* identity)
{
    if (ops.read == nullptr || sector_count == 0) {
        return kInvalidDevice;
    }
    size_t slot = 0;
    while (slot < kMaxBlockDevices && g_devices[slot].present) {
        ++slot;
    }
    if (slot >= kMaxBlockDevices) {
        return kInvalidDevice;
    }

    Device& device = g_devices[slot];
    device = {};
    cache_forget_device(slot); // the index may have been another disk's
    device.present = true;
    device.id = g_next_id++;
    device.ops = &ops;
    device.context = context;
    device.sector_count = sector_count;
    // A driver with no write op is read-only even if it asks to be writable.
    device.writable = writable && ops.write != nullptr;
    device.name = name;
    device.identity = identity;
    if (slot + 1 > g_device_count) {
        g_device_count = slot + 1;
    }
    ++g_generation;
    g_ready = true;
    return slot;
}

bool unregister_device(size_t index) {
    Device* device = nullptr;
    if (!device_for_index(index, device) || queue_has_device(index)) {
        return false;
    }
    *device = {};
    cache_forget_device(index);
    while (g_device_count > 0 && !g_devices[g_device_count - 1].present) {
        --g_device_count;
    }
    ++g_generation;
    return true;
}

size_t probe_all() {
    memset(g_devices, 0, sizeof(g_devices));
    g_device_count = 0;
    g_ready = false;

    // Seleccion directa sobre el array en vez de ordenarlo, igual que
    // display::bind_best. La diferencia es que aca no se corta en el primero:
    // enumeran todos los drivers y sus devices conviven.
    bool enumerated[kMaxDrivers] = {};
    for (size_t attempt = 0; attempt < g_driver_count; ++attempt) {
        size_t best = kMaxDrivers;
        for (size_t i = 0; i < g_driver_count; ++i) {
            if (enumerated[i]) {
                continue;
            }
            if (best == kMaxDrivers || g_drivers[i]->priority > g_drivers[best]->priority) {
                best = i;
            }
        }
        if (best == kMaxDrivers) {
            break;
        }

        enumerated[best] = true;
        g_drivers[best]->enumerate();
    }
    return g_device_count;
}

bool ready() {
    return g_ready;
}

uint32_t generation() {
    return g_generation;
}

size_t device_count() {
    return g_device_count;
}

bool device_info(size_t index, DeviceInfo& info) {
    Device* device = nullptr;
    if (!device_for_index(index, device)) {
        return false;
    }

    info.present = device->present;
    info.id = device->id;
    info.sector_count = device->sector_count;
    info.writable = device->writable;
    info.name = device->name;
    info.identity = device->identity;
    return true;
}

void copy_ata_string(const uint16_t* identify, size_t first_word, size_t word_count, char* out, size_t capacity) {
    if (out == nullptr || capacity == 0) {
        return;
    }
    size_t length = 0;
    for (size_t word = 0; word < word_count && length + 2 < capacity; ++word) {
        const uint16_t value = identify[first_word + word];
        out[length++] = static_cast<char>(value >> 8);
        out[length++] = static_cast<char>(value & 0xff);
    }
    // Keep printable ASCII only; firmware pads with spaces (and some with NULs).
    size_t kept = 0;
    for (size_t index = 0; index < length; ++index) {
        const char c = out[index];
        out[kept++] = (c >= 0x20 && c < 0x7f) ? c : ' ';
    }
    size_t start = 0;
    while (start < kept && out[start] == ' ') {
        ++start;
    }
    while (kept > start && out[kept - 1] == ' ') {
        --kept;
    }
    if (start > 0) {
        memmove(out, out + start, kept - start);
    }
    out[kept - start] = 0;
}

bool read(size_t index, uint32_t lba, uint32_t sector_count, void* buffer) {
    Device* device = nullptr;
    if (!device_for_index(index, device) ||
        !request_in_range(*device, lba, sector_count, buffer)) {
        return false;
    }
    uint8_t* out = static_cast<uint8_t*>(buffer);
    const bool cached = defers(*device) && sector_count <= kCacheMaxRequest;
    if (cached) {
        uint32_t held = 0;
        while (held < sector_count && cache_find(index, lba + held) != nullptr) {
            ++held;
        }
        if (held == sector_count) {
            for (uint32_t offset = 0; offset < sector_count; ++offset) {
                memcpy(out + static_cast<size_t>(offset) * kSectorSize, cache_find(index, lba + offset), kSectorSize);
            }
            ++g_cache_stats.hits;
            return true;
        }
        ++g_cache_stats.misses;
    }
    if (!device->ops->read(device->context, lba, sector_count, buffer)) {
        return false;
    }
    if (defers(*device)) {
        overlay_queued(index, lba, sector_count, out);
    }
    if (cached) {
        // What the caller gets is the newest view, which is what a slot holds.
        for (uint32_t offset = 0; offset < sector_count; ++offset) {
            cache_store(index, lba + offset, out + static_cast<size_t>(offset) * kSectorSize);
        }
    }
    return true;
}

bool write_cached(size_t index, uint32_t lba, uint32_t sector_count, const void* buffer) {
    Device* device = nullptr;
    if (!device_for_index(index, device) || !device->writable ||
        !request_in_range(*device, lba, sector_count, buffer)) {
        return false;
    }
    if (!defers(*device)) {
        return device->ops->write(device->context, lba, sector_count, buffer);
    }
    if (device->failed) {
        return false;
    }

    const uint64_t bytes = static_cast<uint64_t>(sector_count) * kSectorSize;
    if (!make_room(bytes)) {
        return false;
    }
    // From here the write is issued, whichever way it travels: reads must see
    // it. If the write-through below fails, its sectors are dropped instead.
    cache_note_write(index, lba, sector_count, static_cast<const uint8_t*>(buffer));
    void* memory = heap::allocate(sizeof(Pending) + static_cast<size_t>(bytes));
    if (memory == nullptr) {
        // No heap for the copy: empty the queue and write through, which keeps
        // the order because nothing is left in front of this write.
        const bool drain_ok = drain_all();
        if (!drain_ok || !device->ops->write(device->context, lba, sector_count, buffer)) {
            // Not issued after all: the device may hold either copy.
            cache_forget_device(index);
            return false;
        }
        device->unflushed = true;
        return true;
    }
    Pending* record = static_cast<Pending*>(memory);
    record->device = index;
    record->lba = lba;
    record->sector_count = sector_count;
    record->written = 0;
    record->data = reinterpret_cast<uint8_t*>(record + 1);
    memcpy(record->data, buffer, static_cast<size_t>(bytes));
    enqueue(record);
    g_stats.queued_bytes += bytes;
    if (g_stats.queued_bytes > g_stats.peak_queued_bytes) {
        g_stats.peak_queued_bytes = g_stats.queued_bytes;
    }
    return true;
}

bool flush(size_t index) {
    Device* device = nullptr;
    if (!device_for_index(index, device) || !device->writable) {
        return false;
    }
    if (!defers(*device)) {
        return device->ops->flush == nullptr || device->ops->flush(device->context);
    }
    if (device->failed) {
        return false;
    }
    // Nothing of this device is queued or waiting in its cache: there is
    // nothing to order. Two ordering points in a row are one.
    if (!device->unflushed && !queue_has_device(index)) {
        return true;
    }
    if (g_queue_tail != nullptr && g_queue_tail->device == index && g_queue_tail->sector_count == 0) {
        return true;
    }
    void* memory = heap::allocate(sizeof(Pending));
    if (memory == nullptr) {
        // An ordering point that cannot be queued is honoured by draining.
        return drain_all();
    }
    Pending* record = static_cast<Pending*>(memory);
    record->device = index;
    record->lba = 0;
    record->sector_count = 0;
    record->written = 0;
    record->data = nullptr;
    enqueue(record);
    return true;
}

bool sync(size_t index) {
    Device* device = nullptr;
    if (!device_for_index(index, device)) {
        return false;
    }
    return drain_all();
}

void report_health(bool failed) {
    for (size_t i = 0; i < g_driver_count; ++i) {
        if (g_drivers[i]->report_health != nullptr) {
            g_drivers[i]->report_health(failed);
        }
    }
}

bool sync_all() {
    return drain_all();
}
bool writeback_checkpoint(uint64_t& ticket) {
    for (size_t i = 0; i < g_device_count; ++i) {
        const Device& device = g_devices[i];
        if (device.present && device.writable && defers(device) && !flush(i)) return false;
    }
    ticket = g_last_ticket;
    return true;
}
WritebackCompletion writeback_completion(uint64_t ticket) {
    if (ticket > g_last_ticket) return WritebackCompletion::invalid;
    if (ticket <= g_completed_ticket) return WritebackCompletion::complete;
    if (g_queue_head && g_devices[g_queue_head->device].failed) return WritebackCompletion::failed;
    return WritebackCompletion::pending;
}

bool write(size_t index, uint32_t lba, uint32_t sector_count, const void* buffer) {
    return write_cached(index, lba, sector_count, buffer) && flush(index) && sync(index);
}

bool writeback_step() {
    return step();
}

void writeback_tick(uint64_t now_ms) {
    g_clock_ms = now_ms;
    if (g_queue_head == nullptr || now_ms - g_queue_head->queued_ms < kMaxAgeMs ||
        now_ms - g_last_forced_ms < kForcedStepIntervalMs) {
        return;
    }
    g_last_forced_ms = now_ms;
    (void)step();
}

bool writeback_pending() {
    return g_queue_head != nullptr && !g_devices[g_queue_head->device].failed;
}

WritebackStats writeback_stats() {
    return g_stats;
}

ReadCacheStats read_cache_stats() {
    return g_cache_stats;
}

} // namespace block
