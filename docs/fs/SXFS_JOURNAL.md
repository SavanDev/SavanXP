# The SxFS journal

SxFS keeps its metadata — the block bitmap, the inode bitmap and the inode
table, 1537 sectors in all — consistent across a power cut with a redo journal.
Since sparse journals, a commit logs and rewrites **only the metadata sectors
that changed**, not the whole metadata. This page is the format, the write
order that makes it crash-safe, and the invariant the driver relies on to know
what changed.

The code is `commit_metadata()` and `recover_journal()` in
[`kernel/sxfs.cpp`](../../kernel/sxfs.cpp); the on-disk structures are in
[`include/sxfs/sxfs_format.h`](../../include/sxfs/sxfs_format.h).

## Layout

```text
journal_lba          header (1 sector)
journal_lba + 1 + i  metadata sector i, for i in [0, 1537)
```

Metadata sector *i* is the linear view `[block bitmap | inode bitmap | inode
table]`, which is also the home order on disk. Each metadata sector has a fixed
mirror slot in the journal, so a sparse journal needs no packing: it writes the
slots it uses and leaves the others stale.

The header:

| field | meaning |
| --- | --- |
| `magic` | `SXJNL`; a cleared header is all zeroes and has no magic |
| `checksum` | FNV-1a over the header with this field zeroed |
| `sequence` | superblock sequence this transaction commits |
| `pending` | 1 while the transaction may not be home yet |
| `metadata_sectors` | always `SXFS_JOURNAL_METADATA_SECTORS` |
| `kind` | `SXFS_JOURNAL_KIND_FULL` (0) or `SXFS_JOURNAL_KIND_SPARSE` (1) |
| `sector_map` | sparse only: bit *i* set means slot *i* belongs to this transaction |

`kind` used to be reserved and zero, so every journal written before sparse
journals reads as `FULL` and replays exactly as it always did. The kernel only
writes `SPARSE` now. A full commit (see below) is a sparse journal with every bit
set. `SXFS_VERSION` did not change: nothing in a clean image differs.

## Commit order

1. Write the changed sectors into their journal slots.
2. Write the header: `pending = 1`, `kind = SPARSE`, the map.
3. Write both superblocks with the new sequence and without `CLEAN`.
4. Write the changed sectors home.
5. Write both superblocks with `CLEAN`.
6. Zero the header.

A cut at any point remounts as the state before the transaction or the state
after it:

- before or during 2 (a torn header fails its checksum): no valid pending
  journal, home untouched, the old state;
- from 2 to 6: a valid pending journal. Recovery replays the mapped slots over
  whatever home holds, which is the new state. Replaying twice is harmless.

Every `block::write` waits for the device and flushes its cache before
returning, so each step is on the medium before the next one starts.

## Recovery

At mount the home metadata is read first, then the journal is replayed **on top
of it**: a sparse journal overwrites only the sectors it names. That is correct
because every sector it does not name was identical before and after the
transaction, so it does not matter whether the cut left the old or the new copy
at home for those. A full journal overwrites all of them.

The home metadata is validated only **after** the replay. A cut during step 4
leaves home torn (a bitmap updated, the inode table not), and validating before
replaying would refuse a volume the journal can repair. With no pending journal,
home is validated before anything is written to the volume.

The host tool (`sxfs-cli`, `libsxfs`) does not replay journals: it refuses an
image with a pending one, of either kind, so it never loses one.

## Knowing what changed

Every metadata mutation runs between `snapshot_metadata()` and either
`commit_metadata()` or `restore_metadata()`, under the volume's mutation lock.
At snapshot time memory equals what is home on disk, so the snapshot **is** the
on-disk state, and the dirty sectors are the ones that differ from it. The diff
is a `memcmp` of 1537 sectors per commit, in memory.

Two cases break that equality, and both fall back to a full commit:

- no snapshot was taken (`snapshot_valid` is false): a defensive default;
- the previous commit failed. The caller rolls memory back to the snapshot, but
  part of the transaction may already be on disk, so the next sparse diff would
  miss sectors that disk and memory disagree on. `full_commit_pending` makes the
  next commit write everything.

A commit with nothing changed writes nothing.

**Rule for future code:** do not touch `block_bitmap`, `inode_bitmap` or
`inodes` outside a snapshot→commit/restore window. A change made there is
invisible to the diff and never reaches the disk.

## What it costs

`fscheck` after the core smoke (`./build.sh smoke smoke`):

| | bytes per commit |
| --- | --- |
| full copy | 1 575 424 (2 × 1537 sectors + 3) |
| sparse | 3 872 (about 2 changed sectors, twice, + 3) |

Quitting Doom from its menu saves two config files, which is 11 commits. Time
from confirming the quit to the window disappearing, measured with QEMU under KVM,
`--smp 1`, the disk image on a rotational host disk:

| | IDE (PIO) | virtio-blk |
| --- | --- | --- |
| full copy | 70.7 s | 3.5 s |
| sparse | 0.53 s | 2.1 s |
| sparse, host flushes ignored (`cache=unsafe`) | 0.53 s | 0.10 s |

The kernel is non-preemptible and syscalls run with `IF=0`, so the whole system
is frozen for those seconds, cursor included. What is left after sparse
journals is a different cost per device; see
[the roadmap](SXFS_ROADMAP.md#where-the-remaining-cost-is).

## Tests

`tests/host/sxfs_volume_test.cpp` runs the real driver against an in-memory
device whose `block::write` can be cut after any number of sectors (a write
crossing the cut lands in part):

- the cost of a create and a write, in sectors;
- a power cut at every sector of a create-and-write transaction: each one
  remounts writable and clean, the existing file intact, the new file absent,
  empty or complete, never torn;
- a cut at every sector of a create followed by a successful write: disk and
  memory agree after every failed commit.

Each of these fails if the write order, the sparse replay or the full commit
after a failure is broken.
