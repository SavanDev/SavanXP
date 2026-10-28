# SxFS — what is left, and why it all fits in version 2

> **Status: format v2 is current. Everything on this page is planned to land
> inside v2; none of it is scheduled for a v3.** This document is the leftover
> work: what the filesystem still does badly, what each item would cost, and for
> each one whether it is reachable without moving the on-disk format. Where an
> item is *not* reachable in v2, that is stated, because that is the boundary.
>
> The last line of defence for any of this is [`fscheck`](./SXFS_CHECK.md), which
> reports the volume's consistency from inside the system and is what makes a
> change to the format verifiable without a second machine.

The short version: **v2 removed the two ceilings that made the filesystem
unusable, and left one bill unpaid.** The inode table was the binding constraint
(256 inodes, 152 in use with one game installed) and the block bitmap capped the
volume at 64 MiB; both moved. What grew in their place is metadata, and the
journal used to copy all of it on every commit. Item 1 below paid that bill;
what a write still costs, and where, is in
[Where the remaining cost is](#where-the-remaining-cost-is).

## The baseline to compare against

Wall clock is not a usable measure here: the TDC loses 72–95% of real time under
TCG depending on what else the host is doing, so a smoke run's duration moves by
4× on an idle machine versus a loaded one. These are deterministic instead, and
they come out of `fscheck` on a stock 1 GiB volume after a core smoke:

```text
data_sectors          2094075
inodes_allocated      103          of 4096
data_used             59583
commits               19
bytes_per_commit      1575424      = 2 × 1537 sectors of metadata + 3 sectors
bytes_written         29933056     = 19 × 1575424, about 28.5 MiB
```

The single number that matters is `bytes_per_commit`. 1537 sectors of metadata
(512 block bitmap, 1 inode bitmap, 1024 inode table) written twice per commit,
because the journal holds a copy and the home location holds the other one. It
scales with **metadata size, not volume size**, so it is the thing that runs out
of headroom first, and it is independent of how much of the 1 GiB is used.

For reference, v1 spent 97 sectors per commit, so the same smoke wrote 1.8 MiB.
v2 wrote 16× that. With sparse journals (item 1) the same measurement gives
`bytes_per_commit 3872`.

## 1. Done: the journal logs deltas, not copies

A commit now logs and rewrites only the metadata sectors that changed since the
mutation's snapshot, a sparse journal described in
[`SXFS_JOURNAL.md`](SXFS_JOURNAL.md). `bytes_per_commit` went from 1 575 424 to
3 872 on the core smoke, and quitting Doom from its menu (22 commits then) went from a
70.7 s system freeze to 0.53 s on the default IDE disk. The format stayed v2:
the discriminator lives in what used to be a reserved header field, and old full
journals still replay. The crash path is tested at every sector of a transaction
(`sxfs_volume_test`).

## Where the remaining cost is

Measured on Doom's quit, which rewrites two small config files: QEMU under KVM,
`--smp 1`, disk image on a rotational host disk, time spent inside
`sxfs::write_file` as logged by the kernel. The full history is in
[`SXFS_JOURNAL.md`](SXFS_JOURNAL.md#what-it-costs).

| before block writeback | IDE (PIO) | virtio-blk |
| --- | --- | --- |
| Doom's quit, 4 writes, 4 commits | 293 ms | 247 ms |

The cost below is now paid in the background (see the last paragraph of this
section), but it is still the cost of the work.

**Every millisecond of it is a frozen system, not a slow program.** The syscall
gate is an interrupt gate and nothing re-enables interrupts in the syscall path
(see [`SMP_ROADMAP.md`](../kernel/SMP_ROADMAP.md)), and both block drivers poll
the device to completion inside the syscall. Until a write returns, no other
process runs and no timer is serviced, cursor included.

**Small writes are bound by flushes.** A commit has four ordering points and
QEMU turns each device flush into an `fsync` of the image file, so on this host a
commit costs about 60–75 ms whatever the device. What would lower it further is
fewer commits per user-visible operation, not fewer sectors: a truncate and the
rewrite that follows it are two commits today.

**The IDE flush never ran until the ordering points.** The driver wrote FLUSH
CACHE straight after a command's last sector, while the drive was still busy
with it, and a command written while BSY is set is dropped. That is why an
earlier measurement found the flush free, and why IDE looked several times faster
than virtio: with the flush issued after BSY clears, the same kernel took 582 ms
where it had taken 88. The flush is now issued only when the drive is idle.

**Large writes were bound by sectors on IDE.** The full-copy journal moved about
68 000 sectors in 70.7 s on Doom's quit, about 1 ms per sector: QEMU's IDE model
issues one backend request per sector in a PIO data phase. Earlier measurements
point the same way: going from 16-bit to 32-bit port writes bought 26% for half
the port accesses.

| change, `progman-smoke` copying 390 KB under TCG | ms |
| --- | --- |
| 16 bits per port access | 365930, 401218 |
| 32 bits per port access | 280703, 278922 |

The fix was DMA, and **not on that controller**: `isa-ide` is an ISA device and
has no bus master. The default QEMU disk is now on the `q35` board's AHCI
controller ([`AHCI.md`](../kernel/AHCI.md)), where a command moves up to 256
sectors. With host flushes ignored, `progman-smoke`'s 390 KB copy takes 0.33 s on
AHCI against 1.99 s on IDE; with flushes honoured both take about 5 s, so large
writes are now bound by flushes too.

**Writes reach SxFS in large pieces.** The write syscall used to copy user memory
through a 256-byte stack buffer and hand each piece to `vfs::write`, so every
256 bytes of file growth was a commit (a 3 KB file, 12). It now copies through a
heap buffer of up to 64 KiB (`kFileWriteChunk`), and the libc `FILE` buffer is
4 KiB (`SX_FILE_BUFFER_CAPACITY`), allocated on first use. Reads still go
through 256 bytes at a time, which costs sector reads, not commits.

**Writes no longer freeze the system for their whole length.** Physical disks
now queue their writes in the block layer and drain them in bounded steps while
the CPU is idle ([`BLOCK_WRITEBACK.md`](../kernel/BLOCK_WRITEBACK.md)): Doom's
quit spends 8–13 ms in write syscalls, and the longest single stop is one device
flush, 24–35 ms. Removing that last stop needs device I/O that does not poll
inside the kernel, which is the non-preemptible kernel's limit, not the
filesystem's.

## 2. `fscheck` reports, and nothing repairs

**What exists.** [`fscheck`](./SXFS_CHECK.md) reconciles the block bitmap against
the inodes in both directions and walks the tree for unreachable inodes, aliased
entries and duplicate names. It is read-only by construction and exits non-zero
on findings.

**What does not.** Freeing a leaked block, dropping an orphan, rebuilding the
claims bitmap.

**Why it fits in v2.** No layout change at all. It needs a mount slot, and
`kMaxVolumes` is 2, both already used by the installer (root and destination). A
repair path needs 3, or needs to be a host tool.

**What has to be decided first, and this is the reason it is not trivial.**
Clearing a bit in the allocation bitmap is the only operation in the whole
filesystem that can destroy data instead of revealing it. If a sector is marked
occupied because an inode exists that the tree walk could not reach, freeing it
discards that inode's contents. So the design has to answer what an
unreachable-but-present inode *means*, and that is a policy decision, not an
implementation detail. ext2's answer was to keep everything and tell the
administrator; a hobby OS with no `fsck -y` probably wants the same.

**Order.** It waited for item 1, which has landed: a repair path over a journal
that cost 1.5 MiB per commit was a repair path nobody would run.

## 3. A directory with many entries is scanned in O(n²)

**The cost.** `validate_directory` in `libsxfs/sxfs_core.c` and `walk_reachable`
in `kernel/sxfs.cpp` both detect a duplicate name by comparing each entry
against every earlier one. A directory with 4095 entries — the format's
maximum — is 8.4M comparisons, and the host's copy also re-reads the earlier
entries from the image rather than buffering them.

**Why it fits in v2.** Pure code. The on-disk layout is one array of fixed-size
entries and does not have to change.

**What it would take.** Either a sorted comparison or a small per-directory name
cache, and for the host path a buffer that is not the whole directory. Worth it
only if directories actually get big: `/bin` is the realistic case and it
holds about 100 entries today, where O(n²) is 10K comparisons and nobody can
tell.

**Note on the bound.** A directory cannot exceed `SXFS_MAX_RECORDS` (4095)
entries, and there are 4096 inodes total, so a single directory could in
principle hold every inode. That is the ceiling, not a typical shape.

## 4. The `FileRecord` cache is what breaks first, and it is not the inode table

**The cost.** `struct FileRecord` is 344 bytes and there is one per inode:
4095 × 344 = 1.4 MiB per volume, and `kMaxVolumes` is 2. With the block bitmaps
that is 3.4 MiB of static metadata per volume, 6.8 MiB for the pair. The
measured cost of that is small — the guest reports 938 MiB usable out of 1 GiB —
so this is a note, not a problem.

**Why it is on the list anyway.** It is the largest single term and it scales
linearly with `SXFS_MAX_INODES`, while the inode table itself (512 KiB) and the
inode bitmap (512 bytes) do not matter. If the inode count ever has to grow
again, this is the term that decides whether it is affordable, and it is the one
that has nothing to do with the on-disk format. `path[256]` and `name[64]` are
the bulk of it.

## 5. A v1 → v2 converter, for whoever wants one

**What exists.** Nothing. A v1 image is refused with `argumento/ruta invalida`,
deliberately: the superblock validator compares the on-disk geometry against the
compiled constants, so a v1 image would have its metadata read at offsets that
now mean something else. The documented path is to extract with a v1 `sxfs-cli`
and rebuild.

**Why it fits in v2.** It is tooling. `sxfs_walk` does not care about geometry,
so a v1 extractor is a build of the same code against the old constants.

**Whether it is worth it.** Only if anyone has a v1 image with contents worth
keeping. The v1 ceiling was 64 MiB and 255 files, so such an image is either
nearly empty or a collection of small things that are easy to reinstall. This
is the one item on the page that is not clearly worth doing, and it is listed so
that the absence is a decision rather than an oversight.

## 6. Above 1 GiB is a version 3, and this page does not propose it

The block bitmap is the address space: every bit is a sector, so the volume
ceiling is the bitmap's size in bits. `SXFS_MAX_TOTAL_SECTORS` is
`SXFS_BLOCK_BITMAP_SECTORS * 512 * 8`, and `sxfs_superblock_valid` compares the
on-disk `block_bitmap_sectors` against that compile-time constant. Raising the
constant to 1024 sectors would give 2 GiB and would invalidate every existing
image, because the validator is what refuses mismatches.

So the ceiling is not something that can be moved inside v2. It is the one thing
on this page that genuinely requires a new version, and the instruction for this
round is to stay in v2, which is fine: 1 GiB with 4096 inodes is not a limit
anything in the tree is close to. The volume is at 103 of 4096 inodes and 29 MiB
of 1 GiB used.

If 2 GiB is ever wanted, the honest options are the linear bitmap (double the
BSS for the three copies, double the journal payload, and the ATA chunking is
already there) or a two-level bitmap, whose cost is real indirection in the
allocator. The second is more work and buys a ceiling nothing needs yet.

## What is deliberately not broken

- **The kernel validates one direction at mount.** Every per-inode check runs as
  "an occupied sector has an owner". Leaks and unreachable inodes survive it on
  purpose: rejecting them at mount would mean refusing to mount a volume that is
  mostly fine and still readable, and the repair path in item 2 is where that
  decision belongs.
- **Ownership did not need a v3.** uid, gid and mode took bytes the inode had
  reserved, and a feature bit in the superblock's reserved tail says they are
  recorded; a volume without it is stamped on its first writable mount. See
  [`USERS_AND_PERMISSIONS.md`](../kernel/USERS_AND_PERMISSIONS.md#what-each-inode-stores).
- **`sxfs_walk` refuses to follow aliases.** SxFS has no hard links —
  `create_file` writes `link_count = 1` and nothing raises it — so a second path
  to one inode is a defect. The host validator fails the image on it; `fscheck`
  counts it.
