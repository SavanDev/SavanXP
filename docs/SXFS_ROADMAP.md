# SxFS — what is left, and why it all fits in version 2

> **Status: format v2 is current. Everything on this page is planned to land
> inside v2; none of it is scheduled for a v3.** This document is the leftover
> work: what the filesystem still does badly, what each item would cost, and for
> each one whether it is reachable without moving the on-disk format. Where an
> item is *not* reachable in v2, that is stated, because that is the boundary.
>
> The last line of defence for any of this is [`fscheck`](SXFS_CHECK.md), which
> reports the volume's consistency from inside the system and is what makes a
> change to the format verifiable without a second machine.

The short version: **v2 removed the two ceilings that made the filesystem
unusable, and left one bill unpaid.** The inode table was the binding constraint
(256 inodes, 152 in use with one game installed) and the block bitmap capped the
volume at 64 MiB; both moved. What grew in their place is metadata, and the
journal copies all of it on every commit. That is now the most expensive thing
the filesystem does, and it is the first item below.

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
v2 writes 16× that.

## 1. The journal should log deltas, not copies

**The bill.** 28.5 MiB of PIO writes per smoke, 1.5 MiB per metadata mutation,
and it happens twice per commit because the journal and the home copy are the
same bytes written to two places.

**What one write of a real file costs.** `progman-smoke` copies a whole stamped
program to `/disk/bin` to get a fixture, and that copy is the most expensive
single operation anywhere in the smoke suite: **365930 ms for 390664 bytes**, about
1 KB/s, on the same host and with the same caveats as the numbers above (wall clock
under TCG moves by 4× with host load). Everything else in that smoke — two catalog
scans, the rebuild, the `unlink` and the rescan — adds up to about 3 s.

The arithmetic matches the bill exactly. Growing a new file 390 KB means five
metadata commits (64 → 128 → 256 → 512 → 1024 sectors), each writing 1537 sectors
twice, which is 15 370 sectors of metadata for 768 sectors of actual data — a 20×
write amplification on top of a transfer that is PIO word-at-a-time, 256 `outw` per
sector, plus a cache flush per command.

So the ledger is: **a single guest-side file write can cost six minutes**, and the
first program to notice was a smoke that had been failing for 180 s and reading as
"the launcher is broken". It was not. Its timeout was raised to 900 s and the cost
is now printed by the smoke itself, because a red test that says nothing is worse
than a slow one that says why.

**Where the six minutes are not.** Three hypotheses, measured, and two of them wrong:

| change | ms for the same 390 KB |
| --- | --- |
| as it was, 16 bits per port access | 365930, 401218 |
| no cache flush — only the wait kept | 379989 |
| 32 bits per port access | 280703, 278922 |

The cache flush is free: ~143 of them per file, one per transfer command, and
removing every one changed nothing. It stays, because it is also what waits for the
device to go idle.

Halving the port writes bought 26%, not 50%, and **that ratio is the finding**. If
the cost were the number of `outw`, doubling their width would halve the time. It
does not, so most of it is a fixed per-sector cost on the device side — QEMU's IDE
model issues one backend request per sector in a PIO data phase. Nothing the guest
does in that loop can remove it.

What removes it is **bus-master DMA**, which `isa-ide` offers and the kernel does
not use: one backend request for a run of sectors instead of one per sector. It
needs a descriptor table in kernel memory, the BM registers, and a way to know the
device finished — a real feature, not a flag. That is the fix, and it is worth
roughly an order of magnitude on every metadata mutation. Until then the honest
summary is that the journal's cost per commit is the right thing to attack first,
and this is second.

**What it would be.** ext3's answer: log *which blocks changed* instead of
copying the whole metadata. A metadata commit currently touches a handful of
sectors of the 1537; the delta journal would write those handfuls.

**Why it fits in v2.** The journal is transient — `clear_journal()` zeroes the
header after every successful commit — so nothing about it survives in a clean
image. `struct sxfs_journal_header` has 122 reserved bytes
([`sxfs_format.h`](../include/sxfs/sxfs_format.h)), which is where a
discriminator goes: a v2 reader sees a header with no magic as "cleared", and one
with magic and the full-copy discriminator as the format it already knows. A
volume that crashed mid-commit can carry either shape and both must replay
correctly, and that is the only compatibility case there is. No persistent
structure changes, so `SXFS_VERSION` does not move.

**What has to be decided first.** Whether the log records block ranges, inode
deltas, or a redo intent per operation, and whether replay can be done without
consulting the stale copy. The second question is the real one: the current
recovery reads the journal payload *into* the live metadata, so a delta format has
to be applied rather than read.

**Risk.** This is the one item here that touches on-disk state, and the one where
a mistake loses data rather than reporting it. It wants the crash path tested the
way the compaction test tests the sync path, not just a happy-path commit.

## 2. `fscheck` reports, and nothing repairs

**What exists.** [`fscheck`](SXFS_CHECK.md) reconciles the block bitmap against
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

**Order.** Not before item 1: a repair path that runs over a journal whose cost
is 1.5 MiB per commit is a repair path nobody will run.

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
only if directories actually get big: `/disk/bin` is the realistic case and it
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
- **`sxfs_walk` refuses to follow aliases.** SxFS has no hard links —
  `create_file` writes `link_count = 1` and nothing raises it — so a second path
  to one inode is a defect. The host validator fails the image on it; `fscheck`
  counts it.
- **The journal stays whole until item 1.** A delta journal is the change with
  the worst failure mode in this list, and doing it while the metadata is still
  small keeps the regression surface as small as it can be.
