/*
 * block_writeback_test.cpp -- the block layer's writeback queue
 * (kernel/block.cpp), run on the host against fake disks that log every
 * command they receive.
 *
 * What the queue promises, and what each case pins:
 *   - a cached write returns without touching the disk, and reads see it;
 *   - writes reach the disk in issue order, with a device flush at every
 *     ordering point and never a write of a later epoch before that flush;
 *   - a step is bounded, adjacent writes merge, and big writes split;
 *   - sync() drains and flushes; write() is durable;
 *   - the queue never holds more than its budget;
 *   - a failed background write stops the queue instead of skipping ahead;
 *   - partitions and memory devices are written through.
 */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

#include "kernel/block.hpp"
#include "kernel/console.hpp"
#include "kernel/heap.hpp"

extern "C" int memcmp(const void*, const void*, size_t);
extern "C" void* memset(void*, int, size_t);
extern "C" void* memcpy(void*, const void*, size_t);

// --- Kernel services the block layer uses -----------------------------------

namespace heap {
void* allocate(size_t size, size_t) { return malloc(size); }
void free(void* address) { ::free(address); }
} // namespace heap

namespace console {
void printf(const char* format, ...) {
    va_list args;
    va_start(args, format);
    ::vprintf(format, args);
    va_end(args);
}
} // namespace console

namespace {

int g_failures = 0;
int g_checks = 0;

bool check(bool ok, const char* what) {
    ++g_checks;
    if (!ok) {
        ++g_failures;
        printf("  FAIL %s\n", what);
    } else {
        printf("  ok   %s\n", what);
    }
    return ok;
}

// --- Fake disks -------------------------------------------------------------

constexpr uint32_t kSectors = 8192;
constexpr size_t kSectorSize = block::kSectorSize;
constexpr size_t kMaxEvents = 4096;

struct Event {
    char kind; // 'W' write, 'F' flush
    uint32_t lba;
    uint32_t count;
};

struct FakeDisk {
    uint8_t* image;
    Event events[kMaxEvents];
    size_t event_count;
    bool fail_writes;
};

FakeDisk g_disk = {};   // device 0: a physical disk, deferred
FakeDisk g_memory = {}; // device 1: memory, written through

void log_event(FakeDisk& disk, char kind, uint32_t lba, uint32_t count) {
    if (disk.event_count < kMaxEvents) {
        disk.events[disk.event_count++] = {kind, lba, count};
    }
}

bool fake_read(void* context, uint32_t lba, uint32_t count, void* buffer) {
    FakeDisk& disk = *static_cast<FakeDisk*>(context);
    memcpy(buffer, disk.image + lba * kSectorSize, count * kSectorSize);
    return true;
}

bool fake_write(void* context, uint32_t lba, uint32_t count, const void* buffer) {
    FakeDisk& disk = *static_cast<FakeDisk*>(context);
    if (disk.fail_writes) {
        return false;
    }
    memcpy(disk.image + lba * kSectorSize, buffer, count * kSectorSize);
    log_event(disk, 'W', lba, count);
    return true;
}

bool fake_flush(void* context) {
    log_event(*static_cast<FakeDisk*>(context), 'F', 0, 0);
    return true;
}

const block::DeviceOps kDiskOps = {&fake_read, &fake_write, &fake_flush, /*deferred_writes=*/true};
const block::DeviceOps kMemoryOps = {&fake_read, &fake_write, nullptr, /*deferred_writes=*/false};

// Device 2: a partition of device 0 starting at sector 1000, forwarding the
// way kernel/partition.cpp does.
constexpr uint32_t kPartitionStart = 1000;
bool part_read(void*, uint32_t lba, uint32_t count, void* buffer) {
    return block::read(0, kPartitionStart + lba, count, buffer);
}
bool part_write(void*, uint32_t lba, uint32_t count, const void* buffer) {
    return block::write_cached(0, kPartitionStart + lba, count, buffer);
}
bool part_flush(void*) {
    return block::flush(0);
}
const block::DeviceOps kPartitionOps = {&part_read, &part_write, &part_flush, /*deferred_writes=*/false};

void enumerate() {
    (void)block::register_device(kDiskOps, &g_disk, kSectors, true, "disk");
    (void)block::register_device(kMemoryOps, &g_memory, kSectors, true, "mem");
    (void)block::register_device(kPartitionOps, nullptr, 1000, true, "diskp1");
}

const block::Driver kDriver = {"fake", 1, &enumerate};

// Fresh disks and a fresh device table. The queue must already be empty: every
// case ends with a sync.
void reset() {
    memset(g_disk.image, 0, kSectors * kSectorSize);
    memset(g_memory.image, 0, kSectors * kSectorSize);
    g_disk.event_count = 0;
    g_memory.event_count = 0;
    g_disk.fail_writes = false;
    (void)block::probe_all();
}

void fill(uint8_t* sector, uint8_t value, uint32_t count = 1) {
    memset(sector, value, count * kSectorSize);
}

bool sector_is(const uint8_t* image, uint32_t lba, uint8_t value) {
    for (size_t index = 0; index < kSectorSize; ++index) {
        if (image[lba * kSectorSize + index] != value) {
            return false;
        }
    }
    return true;
}

size_t drain() {
    size_t steps = 0;
    while (block::writeback_step()) {
        ++steps;
    }
    return steps;
}

// --- Cases ------------------------------------------------------------------

void case_cached_write_is_deferred_and_visible() {
    printf("case: a cached write waits in the queue and reads see it\n");
    reset();
    uint8_t data[kSectorSize];
    uint8_t back[kSectorSize];
    fill(data, 0xa1);
    check(block::write_cached(0, 10, 1, data), "write_cached accepted");
    check(g_disk.event_count == 0, "the disk received nothing");
    check(sector_is(g_disk.image, 10, 0), "the disk still holds the old sector");
    check(block::writeback_pending(), "the queue has work");
    check(block::read(0, 10, 1, back) && memcmp(back, data, kSectorSize) == 0,
          "a read returns the queued data");

    fill(data, 0xa2);
    check(block::write_cached(0, 10, 1, data), "a second write to the same sector");
    check(block::read(0, 10, 1, back) && memcmp(back, data, kSectorSize) == 0,
          "the newest queued copy wins");
    check(block::sync(0), "sync");
    check(sector_is(g_disk.image, 10, 0xa2), "the disk ends with the newest copy");
    check(!block::writeback_pending(), "the queue is empty after sync");
}

void case_order_and_ordering_points() {
    printf("case: writes reach the disk in order, a flush at every ordering point\n");
    reset();
    uint8_t data[kSectorSize];
    fill(data, 1);
    block::write_cached(0, 100, 1, data);
    block::write_cached(0, 200, 1, data);
    block::flush(0);
    block::flush(0); // two in a row are one
    block::write_cached(0, 300, 1, data);
    block::flush(0);
    block::write_cached(0, 400, 1, data);
    check(g_disk.event_count == 0, "nothing reached the disk while queuing");
    drain();
    const char* expected = "WWFWFW";
    bool shape = g_disk.event_count == 6;
    for (size_t index = 0; shape && index < 6; ++index) {
        shape = g_disk.events[index].kind == expected[index];
    }
    check(shape, "the disk saw W W F W F W");
    check(g_disk.events[0].lba == 100 && g_disk.events[1].lba == 200 &&
          g_disk.events[3].lba == 300 && g_disk.events[5].lba == 400, "in issue order");
    check(block::sync(0), "sync");
    check(g_disk.event_count == 7 && g_disk.events[6].kind == 'F', "sync flushes the last write");

    const size_t before = g_disk.event_count;
    check(block::flush(0) && !block::writeback_pending(), "an ordering point with nothing written is free");
    check(block::sync(0) && g_disk.event_count == before, "and costs the disk nothing");
}

void case_steps_merge_and_split() {
    printf("case: a step merges adjacent writes and splits a big one\n");
    reset();
    uint8_t* data = static_cast<uint8_t*>(malloc(300 * kSectorSize));
    for (uint32_t index = 0; index < 4; ++index) {
        fill(data, static_cast<uint8_t>(10 + index));
        block::write_cached(0, 50 + index, 1, data);
    }
    check(block::writeback_step(), "one step");
    check(g_disk.event_count == 1 && g_disk.events[0].lba == 50 && g_disk.events[0].count == 4,
          "four adjacent one-sector writes went out as one 4-sector write");
    check(sector_is(g_disk.image, 53, 13), "with each sector's own data");
    check(!block::writeback_pending(), "and the queue is empty");

    fill(data, 0x77, 300);
    block::write_cached(0, 1000, 300, data);
    check(drain() == 3, "a 300-sector write takes three steps");
    check(g_disk.event_count == 4 && g_disk.events[1].count == 128 && g_disk.events[2].count == 128 &&
          g_disk.events[3].count == 44 && g_disk.events[3].lba == 1256, "128 + 128 + 44 sectors, in place");
    check(sector_is(g_disk.image, 1299, 0x77), "the last sector landed");
    check(block::sync(0), "sync");

    // Adjacent on disk but on both sides of an ordering point: merging them
    // would let the second land with (or before) the first's flush.
    const size_t before = g_disk.event_count;
    fill(data, 0x21);
    block::write_cached(0, 2000, 1, data);
    block::flush(0);
    block::write_cached(0, 2001, 1, data);
    drain();
    check(g_disk.event_count == before + 3 && g_disk.events[before].kind == 'W' &&
          g_disk.events[before].count == 1 && g_disk.events[before + 1].kind == 'F' &&
          g_disk.events[before + 2].lba == 2001,
          "adjacent writes across an ordering point are not merged");
    check(block::sync(0), "sync");
    free(data);
}

void case_write_is_durable() {
    printf("case: block::write is on the disk when it returns\n");
    reset();
    uint8_t data[kSectorSize];
    fill(data, 0x5a);
    block::write_cached(0, 7, 1, data);
    check(block::write(0, 8, 1, data), "write");
    check(sector_is(g_disk.image, 7, 0x5a) && sector_is(g_disk.image, 8, 0x5a),
          "it and everything queued before it are on the disk");
    check(g_disk.event_count >= 2 && g_disk.events[g_disk.event_count - 1].kind == 'F',
          "and flushed");
    check(!block::writeback_pending(), "nothing left queued");
}

void case_queue_has_a_budget() {
    printf("case: the queue never holds more than its budget\n");
    reset();
    uint8_t* data = static_cast<uint8_t*>(malloc(128 * kSectorSize));
    fill(data, 0x33, 128);
    const block::WritebackStats before = block::writeback_stats();
    // 160 writes of 64 KiB: 10 MiB, more than the 8 MiB budget.
    bool ok = true;
    for (uint32_t index = 0; index < 160 && ok; ++index) {
        ok = block::write_cached(0, (index * 128) % (kSectors - 128), 128, data);
    }
    const block::WritebackStats after = block::writeback_stats();
    check(ok, "every write accepted");
    check(after.forced_drains > before.forced_drains, "writers waited for room");
    check(after.peak_queued_bytes <= 8u * 1024u * 1024u, "the queue stayed within 8 MiB");
    check(g_disk.event_count > 0, "and the disk received the overflow");
    check(block::sync(0), "sync");
    free(data);
}

void case_failure_stops_the_queue() {
    printf("case: a failed background write stops the queue\n");
    reset();
    uint8_t data[kSectorSize];
    fill(data, 1);
    block::write_cached(0, 1, 1, data);
    block::flush(0);
    block::write_cached(0, 2, 1, data);
    g_disk.fail_writes = true;
    check(!block::writeback_step(), "the step reports no progress");
    check(!block::writeback_pending(), "and the idle loop is told there is nothing to do");
    check(!block::write_cached(0, 3, 1, data), "new writes to that disk are refused");
    check(!block::sync(0), "sync reports the failure");
    check(sector_is(g_disk.image, 2, 0), "the write after the failed one never landed");
    // Bring the device back for the cases that follow: a fresh probe clears
    // `failed`, and the healthy disk drains what was kept.
    g_disk.fail_writes = false;
    (void)block::probe_all();
    check(block::sync(0), "after recovery the kept writes drain");
    check(sector_is(g_disk.image, 1, 1) && sector_is(g_disk.image, 2, 1), "in order, none skipped");
}

void case_memory_and_partitions_write_through() {
    printf("case: memory devices and partitions are written through\n");
    reset();
    uint8_t data[kSectorSize];
    uint8_t back[kSectorSize];
    fill(data, 0x44);
    check(block::write_cached(1, 5, 1, data), "write to the memory device");
    check(sector_is(g_memory.image, 5, 0x44) && !block::writeback_pending(),
          "it landed at once, nothing queued");

    fill(data, 0x55);
    check(block::write_cached(2, 5, 1, data), "write through the partition");
    check(block::writeback_pending() && sector_is(g_disk.image, kPartitionStart + 5, 0),
          "the partition's disk queued it");
    check(block::read(2, 5, 1, back) && memcmp(back, data, kSectorSize) == 0,
          "a read through the partition sees it");
    check(block::sync(2), "sync through the partition");
    check(sector_is(g_disk.image, kPartitionStart + 5, 0x55), "it reached the disk at the offset");
}

void case_aging_drains_without_idle() {
    printf("case: the timer drains a queue no idle core empties\n");
    reset();
    uint8_t data[kSectorSize];
    fill(data, 9);
    block::writeback_tick(10000);
    block::write_cached(0, 1, 1, data);
    block::write_cached(0, 3, 1, data);
    block::writeback_tick(10500);
    check(g_disk.event_count == 0, "half a second old: left alone");
    block::writeback_tick(11000);
    check(g_disk.event_count == 1, "one second old: one step");
    block::writeback_tick(11050);
    check(g_disk.event_count == 1, "not again within 100 ms");
    block::writeback_tick(11100);
    check(g_disk.event_count == 2, "then the next step");
    check(block::sync(0), "sync");
}

} // namespace

int main() {
    printf("BLOCK WRITEBACK TEST START\n");
    g_disk.image = static_cast<uint8_t*>(calloc(kSectors, kSectorSize));
    g_memory.image = static_cast<uint8_t*>(calloc(kSectors, kSectorSize));
    block::register_driver(kDriver);

    case_cached_write_is_deferred_and_visible();
    case_order_and_ordering_points();
    case_steps_merge_and_split();
    case_write_is_durable();
    case_queue_has_a_budget();
    case_failure_stops_the_queue();
    case_memory_and_partitions_write_through();
    case_aging_drains_without_idle();

    printf("%s (%d checks, %d failures)\n",
           g_failures == 0 ? "BLOCK WRITEBACK TEST PASS" : "BLOCK WRITEBACK TEST FAIL",
           g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
