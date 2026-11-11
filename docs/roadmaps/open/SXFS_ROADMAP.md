# SxFS — reliable v2 and the stable v3 format

> **Status: format v2 is current and its reliability work (V2-A to V2-D) is
> done. v3 is planned, not implemented or specified in full.** v2 kept every
> existing image while closing its consistency, recovery and file-contract
> gaps. What v2 cannot do is listed under "Closing v2" with who owns it: the
> VFS roadmap for tree publication and caches, v3 for the storage structures
> that limit capacity, scale and extensibility.

The goal is a filesystem suitable for the native OS and its current and planned
subsystems, with explicit durability guarantees, recovery tools and measured
limits. ext4 and NTFS are references for that level of reliability and utility;
matching every feature of either is not the acceptance condition.

Shared file mechanisms and their driver conformance gates are tracked in
[`VFS_ROADMAP.md`](VFS_ROADMAP.md). Its phases are independent of disk-format
versions: consume the relevant contracts during v2, and coordinate v3 design
with them rather than duplicating VFS/VM work inside the SxFS driver.

## Current baseline and the version boundary

Implemented: v2 geometry, sparse metadata journaling, atomic namespace
mutations ([`SXFS_NAMESPACE.md`](../../fs/SXFS_NAMESPACE.md)), persistent
uid/gid/mode, deferred block writeback, mount validation, read-only consistency
reporting via
[`fscheck`](../../fs/SXFS_CHECK.md), and host image updates/compaction using
validated candidates that preserve installed content. The measurements later on
this page are historical baselines, not a current inventory or a release gate.

| Limit | Current implementation |
| --- | --- |
| Volume capacity | 1 GiB; geometry checked against compiled constants |
| Inode table | 4096 entries, fixed at format time by those constants |
| Extents | At most 8 per inode. A file out of extents is moved to one contiguous run; only a volume with no run large enough refuses with room left |
| Names / relative paths | 63 / 255 bytes |
| File size | 32 bits on disk: under 4 GiB, and under the 1 GiB volume in practice |
| Published tree | All entries loaded at attach; 2048 VFS nodes shared by every volume, devices and initramfs |
| Mounted SxFS volumes | 3, each with about 3.7 MiB of statically reserved state |
| Recovery tools | Journal replay in the kernel and in `libsxfs`; `fscheck` reports; `sxfs-cli recover` and `repair` work offline on the host |

The VFS node pool can run out before the SxFS inode table does. Raising format
constants without changing tree publication and caches does not solve scale.
See [`FILESYSTEMS.md`](../../kernel/FILESYSTEMS.md#limits).

**v2 keeps its geometry and existing image contents.** API, VFS, allocation
policy, caching and recovery changes can land without moving its disk regions.
Reserved-field extensions need defined feature semantics and coordinated kernel,
host-tool and validator support. Unchanged structure sizes alone do not prove
that an older reader or writer is safe. Do not silently introduce a new journal
interpretation or inode type under a feature older tools ignore.

**v3 is an explicit format transition.** Its stable baseline must describe
geometry and extensible structures on disk, rather than require a larger set of
compiled arrays. "Stable v3" means an evolution contract and tested mandatory
core, not a promise that filesystem development ends there.

## V2-A — directory consistency before further optimization

**Done.** The journal covers the allocation bitmaps and the inode table;
directory entries are file data and were written in place before the commit,
so a cut could leave an inode nothing named or a rename with neither name. An
audit on 2026-11-08 found it at 3 of 14 cut positions of an unlink and 3 of 11
of a rename.

Directories are now copied on every change and published through the existing
metadata transaction, and a transaction never writes into blocks it frees. The
rules, the fallback that lets a volume with no free sector still delete, and
the `SXFS_FEATURE_LINK_COUNT` bit it relies on are in
[`SXFS_NAMESPACE.md`](../../fs/SXFS_NAMESPACE.md). The journal format and the
geometry did not change.

**Gate, met by `sxfs-namespace-test`:** create, mkdir, unlink, rmdir,
same-directory and cross-directory rename, rename of a directory, replacement
and block reuse are each cut at every sector they write, and the recovery is
cut again. Every cut remounts writable with a clean `fscheck` and the tree
exactly before or exactly after, on a mostly empty volume, a full one, one with
no free sector and a fragmented one; I/O errors at every write are covered the
same way. Arbitrary overwrites inside a file's current size keep no such
guarantee; a truncating rewrite does, while there is room beside the old
contents.

## V2-B — file lifetime, errors and durability

**Done.** The contract is
[`FILESYSTEMS.md`](../../kernel/FILESYSTEMS.md#the-file-contract); the SxFS
side of an unlinked file is in
[`SXFS_NAMESPACE.md`](../../fs/SXFS_NAMESPACE.md#a-file-unlinked-while-open).

- Rename replaces an existing file or empty directory atomically, with type
  checks, and the VFS validates everything it can before the driver commits.
- Unlink removes the name; handles and sections keep the object until the last
  one is released. On disk the object is a doomed inode that keeps its blocks,
  which a mount after a power cut frees, and which `fscheck` reports as a
  pending deletion and not as a finding.
- Driver operations return `fs::Status`; `EIO`, `ENOSPC`, `EROFS`, `EEXIST`,
  `EXDEV` and invalid arguments reach the caller as themselves, and a failed
  read is an error and not an end of file.
- `fsync` on a file or a directory, as the same barrier as `sync`. A deferred
  write error is the `EIO` of the next synchronization.
- `Vnode::content_generation` is the identity file-backed sections and caches
  share; VFS node slots are reused without it ever repeating.

`sxfs-vfs-test` runs the real VFS over the real driver, including the
temporary-file, `fsync`, rename, `fsync` update with a power cut at every
sector; `smoke` repeats the protocol and the unlink of an open file in QEMU.
No native API was added, so there is no new `nativetest` assertion.

Left to the VFS roadmap, not to SxFS: a per-volume barrier cheaper than the
global one, errors delivered to the handle that wrote, open/close callbacks,
and shared writable mappings.

## V2-C — recovery, inspection and migration safety

**Done.** Described in [`SXFS_CHECK.md`](../../fs/SXFS_CHECK.md).

- `probe` writes nothing and recovers in memory; a writable `attach` persists
  the recovery; an explicit read-only mount is enforced by the VFS (`EROFS`)
  and by the driver, and is reachable from Disk Management.
- `libsxfs` replays a pending journal as the kernel does. `sxfs-cli check`
  inspects without writing, `recover` persists idempotently, and `apply`/`rm`
  recover first instead of refusing the image.
- `sxfs-cli repair` works on a copy and keeps the original: it rescues
  unreachable files and directories under `/lost+found` before reclaiming
  anything, rebuilds the block bitmap from the inodes' claims, and reports
  damage with no identifiable owner without touching it. `fscheck` stays
  read-only.

`sxfs-recovery-test` checks all of it against unchanged image bytes, with a
power cut at every sector and recovery interrupted at every write.
`verify_doom_persistence.sh` still covers `/bin` and `/games` across builds.

## V2-D — performance and preparation for scale

**Done, for what is the driver's and the block layer's.**

- **Reads are cached below SxFS**, coherently with the writeback queue
  ([`BLOCK_WRITEBACK.md`](../../kernel/BLOCK_WRITEBACK.md#the-read-cache)): a
  cached read sees every write issued, queued or landed, and write ordering is
  untouched.
- **A repeated name is found in one pass** by `fscheck`, the host walk and
  repair (`sxfs_name_set`), instead of 8.4 million comparisons at 4095 entries.
- **A file's record is read from its vnode**, not searched for.
- **Allocation within eight extents**: growth extends the last extent when it
  can, and a file that runs out of extents with room on the volume is
  consolidated into one run
  ([`SXFS_NAMESPACE.md`](../../fs/SXFS_NAMESPACE.md#growing-a-file)). Measured
  on two files growing in turns: 4 MiB each before, then "no space"; no limit
  but free space after.
- **Commit amplification**: a truncating rewrite is one commit (V2-A), a
  directory's mtime moves in the commit that changes it.

Tested by `block-writeback-test`, `sxfs-vfs-test` and `sxfs-recovery-test`;
the namespace matrix already runs on full and fragmented volumes.

**Not done here, and why.** Lookup and vnode publication on demand, bounded
caches and records without a full path are a change to how every driver
attaches to the VFS, not to SxFS: they are
[VFS-C](VFS_ROADMAP.md#vfs-c--component-resolution-and-bounded-directory-caches),
and SxFS is its first consumer. Until then a mounted tree is resident and the
2048-node pool is the practical ceiling (item 4 below). Nanosecond mtimes and a
volume label were optional and have no consumer; they go to v3's metadata
contract instead of taking a feature bit each (items 8 and "What a second
driver showed"). The read cache's effect is measured in the host test, not yet
on hardware.

## Closing v2 and consuming it from subsystems

**v2 is closed** as a reliable small-volume baseline: recoverable mutations
(V2-A), a durable atomic-update protocol with open-file lifetime and precise
errors (V2-B), inspection that does not write, recovery and conservative
repair on the host (V2-C), and the driver-side costs (V2-D). It is not a claim
of general Linux/Windows compatibility or production certification.

What remains is owned elsewhere:

| Open | Owner |
| --- | --- |
| The whole tree resident at attach; 2048 VFS nodes; a 344-byte record per inode | [VFS-C](VFS_ROADMAP.md#vfs-c--component-resolution-and-bounded-directory-caches) |
| A durability barrier cheaper than the global one; write errors delivered to the writing handle; open/close callbacks | [VFS-A/B/D](VFS_ROADMAP.md) |
| Shared writable mappings; a page cache coherent with file writes | [VFS-E](VFS_ROADMAP.md#vfs-e--mounts-coherent-mappings-and-scalable-io) |
| Overwrites inside a file's size are not atomic (metadata journal only) | v3 journal |
| 1 GiB, 4096 inodes, 8 extents, 63-byte names, no links, whole-second mtimes, no label | v3 format (V3-A) |
| Repair of cross-linked sectors and damaged inodes; repair from inside the guest | consumer-driven; `sxfs-cli repair` reports them |

| Consumer | Storage responsibility and acceptance |
| --- | --- |
| SUN and current POSIX | Common lifetime, permissions, error, replacement and durability contracts; regression coverage for current programs |
| [Linux L2-L3](LINUX_SUBSYSTEM_ROADMAP.md#l2--static-c-programs) | Pinned static musl/tool profiles may consume v2; demonstrate measured file operations, errors and restart/build persistence |
| Linux dynamic and broader profiles | File-backed mappings, links, directory-relative operations and locking as measured; VM/VFS work is not supplied by changing the disk format |
| [Win32 W2](WIN32_SUBSYSTEM_ROADMAP.md#w2--base-runtime-for-the-measured-closure) | Keep drive paths, case rules, sharing, creation dispositions and deletion policy in Win32; retain native access checks |
| Broader Win32 profiles | Add security metadata, streams/reparse behavior or notifications with named consumers; do not change all native names to Windows semantics |

Initial bounded profiles need not wait for all of v3. Neither installing a
Linux runtime nor loading a PE image establishes the broader filesystem
contract. Hard links can use existing fields in principle but require changes
to records, tree validation and lifetime semantics; the complete link model is
scheduled with v3. New inode types such as symbolic links require an explicit
format/feature decision rather than an unannounced v2 reinterpretation.

## V3-A — specify the stable storage contract

**Planned; specify before implementing a writer.** Freeze mandatory structures,
byte order, checksums, address/size bounds, recovery rules and feature discovery.
Use explicit compatible, read-only-compatible and incompatible feature classes.
Unknown incompatible features must fail safely; unsupported read-only-compatible
ones must prohibit writes. Specify safe old-tool behavior and test it.

| Structure | Required v3 capability |
| --- | --- |
| Superblock and geometry | On-disk geometry, volume identity and documented redundant recovery records; no exact match to compiled v2 layout constants |
| Addresses and file sizes | 64-bit representation throughout format, VFS, APIs and tools, with checked arithmetic |
| Block allocation | Groups/regions and free-space summaries; locality and fragmentation policy without scanning one whole-volume bitmap per allocation |
| Inode storage | Configurable capacity and on-demand access; defined exhaustion/growth policy without a resident table per volume |
| File extents | Extensible indirect/tree representation beyond eight inline extents; validated bounds and ownership |
| Directories | Longer names, extensible records and scalable indexing; component-wise path resolution without a full path in each record |
| Journal | All affected namespace and auxiliary metadata in recoverable transactions, checksummed payloads and explicit ordering/replay rules |
| Integrity | Metadata checksums, block identity/generation validation and a documented corruption response; detection is not automatic repair |
| Links and identity | Persistent identity independent of names, hard links, symbolic links and recovery of unlinked-but-open objects |
| Sparse storage | Explicit holes, zero-read semantics and preallocation without writing every intervening byte |
| Extensible metadata | Complete timestamp contract, subsecond fields, extended attributes and room for ACL/compatibility metadata |

**Done when:** a written format specification and independent reader/validator
fixtures agree, including invalid structures and unknown features. Define
supported capacities and scale targets before making support claims; representable
64-bit values do not certify that every such volume is supported.

## V3-B — implement storage and complete shared contracts

**Planned; depends on V3-A.** Build the formatter, driver, journal/recovery,
allocation and directory structures, and teach the existing maintenance tools
the format. Carry forward v2's reliability gates. Finish VFS/VM support for
on-demand objects, mapping coherence, file identity and reclamation; filesystem
capacity must not outgrow what the VFS can mount and manipulate.

**Done when:** kernel and host tools agree on new volumes, and operations across
allocation groups, indirect extents, directory splits, sparse files and metadata
extensions survive failures and recovery. Exercise files beyond 4 GiB, volumes
beyond 1 GiB and trees beyond both current inode/vnode ceilings. Pin larger
workload sizes and resource budgets in the tests before declaring scale support.

## V3-C — migrate without resetting persistent installations

**Planned; required before adoption.** Use an explicit offline migration into a
new candidate volume. Inventory and copy all reachable contents and metadata,
validate the result, and retain the source for rollback. Do not silently migrate
an image with unresolved consistency findings. v2's lack of some metadata must
have documented defaults, not fabricated preservation claims.

**Done when:** migration and interrupted installation tests preserve external
executables, `/games` assets, `/system/linux`, `/system/win`, ownership, times
and user data. `./build.sh build` preserves a valid existing image and never
implicitly formats or destructively upgrades it. Run the existing
`./tools/verify_doom_persistence.sh` regression as well as subsystem fixtures;
rollback restores a usable original image. v1 conversion remains separate and
optional, not an excuse to omit the v2 migration.

## V3-D — qualify the production baseline

**Planned; required before calling v3 stable.** Run sustained create/delete/
rewrite workloads, concurrent readers/writers, low-space and fragmented images,
device errors, corrupted metadata/journal payloads and repeated recovery cuts.
Validate permissions, information exposure through reused blocks, and resource
cleanup on failure. Compare measured Linux/Win32 operations with their reference
profiles and keep native SUN/POSIX regressions passing. Report throughput,
latency, memory budgets, workload sizes and remaining limits.

**Done when:** reproducible automated gates cover these workloads and recovery
tools ship with the format, migration and rollback. The stable declaration names
the tested hardware/device contracts and supported scale; passing a few QEMU
smokes is not sufficient evidence.

The core need not initially implement compression, encryption, quotas, named
streams or every NTFS/ext4 extension. Add optional features through the reviewed
extension contract and concrete consumers. If snapshots are a future objective,
decide their allocation/ownership implications during V3-A; do not assume a
feature bit can retrofit them onto any design.

## Detailed v2 backlog and historical measurements

v2 removed the original 256-inode and 64 MiB ceilings. The larger metadata then
made full-copy journal commits expensive; sparse journaling paid that cost.
The sections below retain the measurements and specific implementation work,
with V2-A through V2-D setting priority and acceptance.

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
[`SXFS_JOURNAL.md`](../../fs/SXFS_JOURNAL.md). `bytes_per_commit` went from 1 575 424 to
3 872 on the core smoke, and quitting Doom from its menu (22 commits then) went from a
70.7 s system freeze to 0.53 s on the default IDE disk. The format stayed v2:
the discriminator lives in what used to be a reserved header field, and old full
journals still replay. `sxfs_volume_test` cuts a create/write sequence at every
sector; every namespace mutation is cut the same way by `sxfs-namespace-test`
(V2-A above).

## Where the remaining cost is

Measured on Doom's quit, which rewrites two small config files: QEMU under KVM,
`--smp 1`, disk image on a rotational host disk, time spent inside
`sxfs::write_file` as logged by the kernel. The full history is in
[`SXFS_JOURNAL.md`](../../fs/SXFS_JOURNAL.md#what-it-costs).

| before block writeback | IDE (PIO) | virtio-blk |
| --- | --- | --- |
| Doom's quit, 4 writes, 4 commits | 293 ms | 247 ms |

The cost below is now paid in the background (see the last paragraph of this
section), but it is still the cost of the work.

**Every millisecond of it is a frozen system, not a slow program.** The syscall
gate is an interrupt gate and nothing re-enables interrupts in the syscall path
(see [`SMP_ROADMAP.md`](../done/SMP_ROADMAP.md)), and both block drivers poll
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
controller ([`AHCI.md`](../../kernel/AHCI.md)), where a command moves up to 256
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
the CPU is idle ([`BLOCK_WRITEBACK.md`](../../kernel/BLOCK_WRITEBACK.md)): Doom's
quit spends 8–13 ms in write syscalls, and the longest single stop is one device
flush, 24–35 ms. Removing that last stop needs device I/O that does not poll
inside the kernel, which is the non-preemptible kernel's limit, not the
filesystem's.

## 2. Done: `fscheck` reports, `sxfs-cli repair` repairs

[`fscheck`](../../fs/SXFS_CHECK.md) reconciles the block bitmap against the
inodes in both directions and walks the tree for unreachable inodes, aliased
entries and duplicate names. It is read-only by construction and stays so.

Repair is offline, on the host, on a copy
([`SXFS_CHECK.md`](../../fs/SXFS_CHECK.md#offline-repair)). The decision this
item waited for is made there: an unreachable inode is rescued under
`/lost+found`, never freed; a marked sector nothing claims is freed only when
every inode could be read; damage with no identifiable owner is reported and
left. Committed deletions (V2-B's unlinked files) are finished, by a mount or
by repair.

## 3. Done: a repeated name is found in one pass

`validate_directory` in `libsxfs/sxfs_core.c` and `walk_reachable` in
`kernel/sxfs.cpp` compared each entry against every earlier one: 8.4 million
comparisons for a directory at the format's maximum of 4095 entries, and the
kernel's copy re-read the earlier entries from the device to do it. All three
readers (the third is repair) now use `sxfs_name_set`, a 16 KiB hash set over
the directory held in a buffer. The on-disk layout did not change.

Looking a name up while creating or renaming is still a linear scan of that
one directory. An index is a format matter (V3-A, "Directories").

## 4. The `FileRecord` cache scales poorly, and the VFS can fill first

**The cost.** `struct FileRecord` is 344 bytes and there is one per inode:
4095 × 344 = 1.4 MiB per volume, and `kMaxVolumes` is 3. With the block bitmaps
that is 3.4 MiB of static metadata per volume, about 10 MiB for all three. The
measured cost of that is small — the guest reports 938 MiB usable out of 1 GiB —
but it is not a design that can simply scale to millions of inodes. The shared
2048-node VFS pool can already fill before one v2 volume exhausts its inodes.

**Why it is on the list anyway.** It is the largest single term and it scales
linearly with `SXFS_MAX_INODES`, while the inode table itself (512 KiB) and the
inode bitmap (512 bytes) do not matter. If the inode count ever has to grow
again, this is the term that decides whether it is affordable, and it is the one
that has nothing to do with the on-disk format. `path[256]` and `name[64]` are
the bulk of it.

## 5. A v1 → v2 converter, for whoever wants one

**What exists.** Nothing. A v1 image is refused as "not a valid SxFS image",
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

## 6. Above 1 GiB belongs to v3

The block bitmap is the address space: every bit is a sector, so the volume
ceiling is the bitmap's size in bits. `SXFS_MAX_TOTAL_SECTORS` is
`SXFS_BLOCK_BITMAP_SECTORS * 512 * 8`, and `sxfs_superblock_valid` compares the
on-disk `block_bitmap_sectors` against that compile-time constant. Raising the
constant to 1024 sectors would give 2 GiB and would invalidate every existing
image, because the validator is what refuses mismatches.

Do not raise the constant and silently call the result v2. V3-A replaces fixed
geometry with an explicit format and grouped allocation; V3-B must demonstrate
that the driver, VFS and tools can use its larger capacities. The small stock
volume in the historical baseline is not evidence about future subsystem needs.

## 7. Done: small reads are cached

`block.cpp` cached writes and nothing cached reads: every `read_file` became a
device request, however small. A 64-byte read of an ELF section header cost
the same round trip as a full sector, and reading the same sector twice paid
twice. Under QEMU each request is a VM exit.

This is what made the Start menu take a second to open: building progman's
catalog read every binary in `/bin` through `sxe_read_section`, which walked
the section headers one 64-byte read at a time, about 7,700 requests and
541 ms in `progman-smoke`. That was fixed above the filesystem (`struct
sxe_file` reads the header table and `.shstrtab` once), but every other reader
of small records still paid the full price.

The block layer now keeps a 1 MiB sector cache for small reads of physical
disks ([`BLOCK_WRITEBACK.md`](../../kernel/BLOCK_WRITEBACK.md#the-read-cache)).
The design questions this item listed were answered as: a fixed pool,
direct-mapped rather than LRU (a collision costs one device read), and
coherence by having every write refresh the slots it covers. Metadata already
lives in memory, so it serves file data and directory blocks.

## 8. An mtime is whole seconds

`inode.mtime` is set when an inode is created and when its own contents are
written or truncated. Creating, deleting or renaming an entry used not to touch
the parent directory's mtime, so "did anything in `/bin` change?" could not be
answered by one `stat` of `/bin`.

The field is whole seconds, so a file rewritten in place within the same
second at the same size is indistinguishable from the old one by metadata. The
Start menu's cache accepts that edge.

**Done:** the parent's `mtime` moves with its entries, in the commit that
changes them ([`SXFS_NAMESPACE.md`](../../fs/SXFS_NAMESPACE.md#rule-1-a-directory-is-never-changed-in-place)).
`progman_registry_disk_signature()` still lists and stats, which stays correct.

**Left to v3:** sub-second resolution would fit in `reserved1`, the 4 spare
bytes right before `mtime`, behind a feature bit. Nothing asks for it: the one
consumer of mtimes compares listings, and the kernel has no sub-second wall
clock to fill it from. It is part of V3-A's "complete timestamp contract"
rather than a v2 feature bit of its own.

## What a second driver showed

Writing the FAT driver ([FAT_ROADMAP.md](FAT_ROADMAP.md)) was the first time
anything but SxFS sat under the VFS. What it showed about the VFS itself is in
[FILESYSTEMS.md](../../kernel/FILESYSTEMS.md#limits); these are SxFS's.

- **Fixed: mounting was not read-only.** `probe` replayed the journal onto the
  disk before validating, so SxFS could not be mounted just to look at a
  volume. `probe` now only reads and a writable attach persists the recovery
  (V2-C). The search for the boot volume still asks for the FAT driver by
  name; it no longer has to for safety, only to skip a pointless probe.
- **Few volumes, each expensive.** `kMaxVolumes` is 3 and each one costs about
  3.4 MiB of static metadata (item 4), reserved whether or not it is used. The
  root takes one, so two SxFS volumes can be mounted at `/mnt`, which is also
  all `fs::kMaxMounts` leaves once the root and `/boot` are in. Raising either
  means raising both, and item 4 is what makes the SxFS side cost.
- **Fixed: every operation on a file's vnode searched every record.**
  `file_from_vnode` now reads the record `mount_record` put in `vnode.data`,
  checked against the table. The search remains for directories, which the
  VFS gives nothing of the driver's.
- **No volume label.** SxFS has none; Disk Management shows and sets the GPT
  partition name instead ([DISK_MANAGEMENT.md](../../userland/DISK_MANAGEMENT.md)),
  so a whole-disk SxFS volume has no name at all. A label in the superblock's
  reserved tail would fit, but it also needs a way to set it and a field in
  the storage snapshot; it goes with v3's "volume identity".
- **`write(..., truncate)` ignores the offset.** With `truncate` the file is
  emptied and written from 0, whatever offset was passed; the VFS uses it for
  `O_TRUNC` with an empty write. FAT copies the behaviour so both drivers
  answer the same. It is an odd contract for a driver to learn, and a
  separate truncate-to-zero before the write would say the same thing.

## What is deliberately not broken

- **The kernel validates claims at mount.** Claimed sectors must be allocated,
  within bounds and owned by at most one inode. Leaks and unreachable inodes
  survive it on purpose: rejecting them at mount would mean refusing a volume that is
  mostly fine and still readable. `sxfs-cli repair` (item 2) is where they are
  dealt with.
- **Ownership did not need a v3.** uid, gid and mode took bytes the inode had
  reserved, and a feature bit in the superblock's reserved tail says they are
  recorded; a volume without it is stamped on its first writable mount. See
  [`USERS_AND_PERMISSIONS.md`](../../kernel/USERS_AND_PERMISSIONS.md#what-each-inode-stores).
- **`sxfs_walk` refuses to follow aliases.** SxFS has no hard links —
  `create_file` writes `link_count = 1` and nothing raises it — so a second path
  to one inode is a defect. The host validator fails the image on it; `fscheck`
  counts it.
