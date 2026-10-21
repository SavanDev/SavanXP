# Block writeback

Writes to a physical disk do not happen in the syscall that makes them. The
block layer copies them into a queue in kernel memory and returns; the queue
reaches the disk later, in small pieces, while the CPU has nothing else to do.
This page is why, how the order is kept, and what it costs in durability.

The code is in [`kernel/block.cpp`](../../kernel/block.cpp); the API is in
[`include/kernel/block.hpp`](../../include/kernel/block.hpp).

## Why

The kernel is non-preemptible: a syscall runs with interrupts off from entry to
exit, and the storage drivers poll the device to completion inside it
([`SMP_ROADMAP.md`](SMP_ROADMAP.md)). Any disk write therefore froze the whole
system, cursor and audio included, for as long as the device took, and on QEMU a
device flush is an `fsync` of the image on the host. Quitting Doom from its menu
spent 247–293 ms frozen inside the write syscalls even after the journal work in
[`SXFS_JOURNAL.md`](../fs/SXFS_JOURNAL.md).

Making the drivers interrupt-driven would not help by itself: a syscall cannot
sleep on its kernel stack here, and an SxFS commit is several dependent writes.
The queue moves the device time out of the syscall instead, and hands it out in
pieces small enough that the scheduler gets the CPU back between them.

## The contract

| call | returns when |
| --- | --- |
| `block::write_cached` | the data is copied into the queue |
| `block::flush` | an ordering point is queued: nothing issued after it reaches the disk before everything issued before it |
| `block::sync`, `block::sync_all` | the whole queue is on the medium and every disk flushed |
| `block::write` | `write_cached` + `flush` + `sync`: durable |
| `block::read` | the device data with every queued write laid over it, oldest first |

Only devices whose `DeviceOps::deferred_writes` is set queue: the physical disks
(`ahci`, `ata`, `virtio-blk`). A partition forwards to its disk, which queues; a
ramdisk is memory and is written through.

## Keeping the order

The queue is one FIFO of records for every disk. A record is a write, with its
data copied in, or an ordering point. Records reach their disk in queue order,
and an ordering point becomes a device flush (skipped if the disk took no write
since its last flush). So the medium receives exactly the sequence a
synchronous caller would have produced, with the same flushes between the same
writes, only later. Every crash-safety argument made against synchronous
ordering points, like the journal's, holds unchanged.

What changes is **durability**: a write is safe once the queue reaches it, not
when the syscall returns. A power cut loses the tail of the queue and never
reorders it. `SYS_SYNC` (and the `sync` command) and `/dev/power`'s shutdown and
reboot wait for the queue. On QEMU, closing the window kills the guest without
either; the idle drain usually empties the queue within milliseconds of the last
write. If it did not, the image is consistent but may have a pending journal,
which the host tools refuse (`sxfs_sync` leaves the image untouched); one boot of
the guest replays it.

## Draining

- **Idle.** The idle task is a user-mode loop of `SYS_YIELD`. When nothing else
  is runnable, the yield handler runs one `block::writeback_step()` instead of
  halting, and returns. A step is one device command: up to 128 sectors (64 KiB)
  of adjacent writes merged from consecutive records, or one device flush. On
  the way back to user mode interrupts are enabled again, so a process that
  became runnable gets the CPU after at most one step.
- **Age.** If no CPU goes idle (a process that never sleeps), the timer tick on
  the bootstrap core does one step every 100 ms once the oldest record is a
  second old.
- **Budget.** The queue holds at most 8 MiB. A writer that would exceed it
  drains the queue itself until its data fits, so a fast writer is slowed to the
  disk's pace instead of exhausting kernel memory.
- **No heap.** A write whose copy cannot be allocated drains the whole queue
  and goes through directly, which keeps the order because nothing is left in
  front of it.

## Failure

A background write or flush that fails is not skipped: dropping it and going on
would let later records land without it, which is the one thing the order
promises not to allow. The disk is marked failed and logged once
(`block: <disk>: background write failed`), its records stay queued, the idle
loop stops trying, and every later write, flush or sync for that disk fails, so
the filesystem reports the error to whoever writes next.

## What it measured

QEMU under KVM, image on a rotational host disk.

| | write syscalls | background | longest step |
| --- | --- | --- | --- |
| Doom's quit, IDE | 13 ms (was 293) | 308 ms in 46 steps | 35 ms |
| Doom's quit, AHCI | 9 ms | 166 ms in 46 steps | 24 ms |
| Doom's quit, virtio-blk | 8 ms (was 247) | 207 ms in 46 steps | 24 ms |

| `progman-smoke`'s 390 KB copy, as the program sees it | before | after |
| --- | --- | --- |
| IDE | 4.7 s | 289 ms |
| AHCI | 6.1 s | 148 ms |

The longest step is a device flush: the system still stops for that long, once
per step, but no longer for the whole operation.

## Tests

`tests/host/block_writeback_test.cpp` runs `kernel/block.cpp` against fake disks
that log every command: deferral and read-through, order and flushes at ordering
points, merging and splitting, durability of `write`, the budget, a failed write
stopping the queue, write-through for memory and partitions, and the age-based
drain. The SxFS power-cut cases in `sxfs_volume_test` cover the ordering points
the filesystem places.
