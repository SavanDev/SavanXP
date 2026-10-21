#include "kernel/block.hpp"

#include <stdint.h>

#include "kernel/console.hpp"
#include "kernel/heap.hpp"
#include "kernel/string.hpp"

namespace {

// Los 4 slots ATA + el ramdisk del LiveCD, mas las particiones que el driver
// partition:: recorta encima de ellos (hasta 8), con lugar de sobra para lo
// que sumen drivers nuevos sin volver a tocar el core.
constexpr size_t kMaxBlockDevices = 16;

struct Device {
    bool present;
    const block::DeviceOps* ops;
    void* context;
    uint32_t sector_count;
    bool writable;
    const char* name;
    // Writeback state: the device took writes since its last flush, or a
    // background write failed and the queue for it is stuck (see fail()).
    bool unflushed;
    bool failed;
};

Device g_devices[kMaxBlockDevices] = {};
size_t g_device_count = 0;
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
uint8_t g_stage[static_cast<size_t>(kStepSectors) * block::kSectorSize];

bool defers(const Device& device) {
    return device.ops->deferred_writes;
}

void enqueue(Pending* record) {
    record->next = nullptr;
    record->queued_ms = g_clock_ms;
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

bool register_device(
    const DeviceOps& ops,
    void* context,
    uint32_t sector_count,
    bool writable,
    const char* name)
{
    if (ops.read == nullptr || sector_count == 0 || g_device_count >= kMaxBlockDevices) {
        return false;
    }

    Device& device = g_devices[g_device_count];
    device.present = true;
    device.ops = &ops;
    device.context = context;
    device.sector_count = sector_count;
    // Un driver sin write op queda solo-lectura aunque lo pida escribible.
    device.writable = writable && ops.write != nullptr;
    device.name = name;
    ++g_device_count;
    g_ready = true;
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

size_t device_count() {
    return g_device_count;
}

bool device_info(size_t index, DeviceInfo& info) {
    Device* device = nullptr;
    if (!device_for_index(index, device)) {
        return false;
    }

    info.present = device->present;
    info.sector_count = device->sector_count;
    info.writable = device->writable;
    info.name = device->name;
    return true;
}

bool read(size_t index, uint32_t lba, uint32_t sector_count, void* buffer) {
    Device* device = nullptr;
    if (!device_for_index(index, device) ||
        !request_in_range(*device, lba, sector_count, buffer)) {
        return false;
    }
    if (!device->ops->read(device->context, lba, sector_count, buffer)) {
        return false;
    }
    if (defers(*device)) {
        overlay_queued(index, lba, sector_count, static_cast<uint8_t*>(buffer));
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
    void* memory = heap::allocate(sizeof(Pending) + static_cast<size_t>(bytes));
    if (memory == nullptr) {
        // No heap for the copy: empty the queue and write through, which keeps
        // the order because nothing is left in front of this write.
        if (!drain_all()) {
            return false;
        }
        if (!device->ops->write(device->context, lba, sector_count, buffer)) {
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

bool sync_all() {
    return drain_all();
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

} // namespace block
