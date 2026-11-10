# SxFS — reliable v2 and the stable v3 format

> **Status: format v2 is current. The v2 reliability and file-contract work
> below is open; v3 is planned, not implemented or specified in full.** v2
> preserves existing images while closing consistency, recovery and API gaps.
> v3 changes the storage structures that limit capacity, scale and extensibility.
> Finishing the old v2 performance backlog alone does not meet the production
> acceptance gates on this page.

The goal is a filesystem suitable for the native OS and its current and planned
subsystems, with explicit durability guarantees, recovery tools and measured
limits. ext4 and NTFS are references for that level of reliability and utility;
matching every feature of either is not the acceptance condition.

Shared file mechanisms and their driver conformance gates are tracked in
[`VFS_ROADMAP.md`](VFS_ROADMAP.md). Its phases are independent of disk-format
versions: consume the relevant contracts during v2, and coordinate v3 design
with them rather than duplicating VFS/VM work inside the SxFS driver.

## Current baseline and the version boundary

Implemented: v2 geometry, sparse metadata journaling, persistent uid/gid/mode,
deferred block writeback, mount validation, read-only consistency reporting via
[`fscheck`](../../fs/SXFS_CHECK.md), and host image updates/compaction using
validated candidates that preserve installed content. The measurements later on
this page are historical baselines, not a current inventory or a release gate.

| Limit | Current implementation |
| --- | --- |
| Volume capacity | 1 GiB; geometry checked against compiled constants |
| Inode table | 4096 entries, fixed at format time by those constants |
| Extents | At most 8 per inode; fragmentation can prevent allocation despite free space |
| Names / relative paths | 63 / 255 bytes |
| Published tree | All entries loaded at attach; 2048 VFS nodes shared by every volume, devices and initramfs |
| Mounted SxFS volumes | 3, each with several MiB of statically reserved state |
| Recovery tools | Journal replay in the kernel; `fscheck` reports but does not repair; host tools refuse pending journals |

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

**Open, first priority.** The current journal covers allocation bitmaps and the
inode table. Directory entries are written through the data path before the
metadata commit; they are not part of its redo payload, and restoring a metadata
snapshot does not undo them. The existing create/write power-cut tests do not
establish atomicity for every namespace operation.

A local audit on 2026-11-08 compiled the real driver with the host stubs and
temporarily extended the power-cut harness. The existing suite passed all 122
checks. Starting with `/docs/hello.txt`, cutting at each sector written by an
unlink left an orphan at 3 of 14 cut positions. Renaming it to `/moved.txt` left
an orphan with neither name at 3 of 11 positions. These are deterministic cut
positions, not failure probabilities. The additional cases are not yet committed
regression tests; reproducing and adding them is the first implementation task.

Design an atomic update of directory contents, inode references and allocation.
Copying changed directories into newly allocated blocks, then publishing their
extents through the existing metadata transaction, is a v2 candidate; it is not
yet a selected or proven implementation. The journal's fixed metadata mirror
slots cannot simply accept arbitrary directory blocks. Another flush alone does
not make an in-place directory update transactional.

**Done when:** committed tests cut every mutation and its recovery at every
sector boundary, including create, mkdir, unlink, rmdir, same-directory and
cross-directory rename, replacement and block reuse. Each cut must recover a
consistent permitted before/after state without losing both rename names or
damaging unrelated files. Cover repeated cuts during recovery, low space,
fragmentation and I/O failures. An orphan report is not a substitute for
preserving a name during rename. Arbitrary file-content overwrites do not gain
whole-write atomicity merely because namespace metadata is recoverable.

## V2-B — file lifetime, errors and durability

**Open; depends on V2-A.** Complete the common file contract through Sx and the
VFS, then translate it at SUN/POSIX and future subsystem boundaries:

- Atomic rename with replacement, including type checks and failure rollback.
  Prepare the VFS update so disk success cannot leave the in-memory tree behind.
- Unlink removes a name; open handles and file-backed sections keep the object
  alive until their last reference is released. Define the persistent state and
  crash recovery of pending deletions, and distinguish it from unexplained
  unreachable inodes. Drivers need lifetime callbacks; freeing blocks at unlink
  or returning POSIX `EBUSY` is not the completed contract.
- Driver status results that distinguish `EIO`, `ENOSPC`, `EROFS`, invalid
  arguments and name collisions, rather than letting the VFS guess from a bool.
- Per-file and directory durability operations, write-through/open contracts as
  required by consumers, and delivery of deferred write errors to the caller.
  Global `sync` and writeback tickets already exist; write/close success must
  not be documented as durable storage by default.
- Stable identity and content-generation rules shared with file-backed sections
  and caches. Existing file-image sections are not a coherent writable page
  cache. VM/VFS must own mapping, truncation and invalidation rules together.

**Done when:** a temporary-file update followed by file synchronization, atomic
replacement and directory synchronization has documented, crash-tested outcomes.
Open handles retain the correct contents through rename/unlink, reclamation
happens once, and failures return precise errors. Existing SUN/POSIX permission
checks remain effective. Each exposed native API slice has its SUN consumer and
`nativetest` assertion as required by [`SUN.md`](../../userland/SUN.md).

## V2-C — recovery, inspection and migration safety

**Open; repair builds on V2-A.** Extend existing host tooling and define an
offline repair path. Keep `fscheck` read-only. Rescue readable orphan contents
before freeing allocations; reconcile claims only when ownership is known,
report ambiguous damage, and keep a backup before destructive repair. Host
tools need validated, idempotent journal recovery rather than requiring a guest
boot for every pending transaction.

Separate read-only recognition from journal recovery and writable attach.
Inspection must be able to leave the device untouched, while an explicit
read-only mount must enforce its policy across the VFS and driver. If recovery
needs writes, refuse or provide a defined in-memory view; never write silently.

**Done when:** malformed and interrupted images have deterministic reports,
repair preserves recoverable contents, and interrupted recovery/repair can be
resumed safely. Inspection is checked against unchanged image bytes. Existing
programs in `/bin`, game assets under `/games`, subsystem runtimes and user data
survive normal builds, maintenance and rollback.

## V2-D — performance and preparation for scale

**Open; independent groundwork can proceed alongside V2-A.** The detailed
backlog below remains applicable: coherent block-read caching, directory
validation without quadratic comparisons, direct record lookup, parent mtime
updates, optional nanoseconds/volume labels with reviewed feature semantics,
and less commit amplification. Improve allocation policy within the eight-extent
limit and measure low-space/fragmented workloads as well as empty images.

Start lookup and vnode publication on demand, bounded caches and identity-based
records in the VFS. Do not require the complete mounted tree or a full path per
inode to stay resident. Coordinate with FAT, whose driver shares these limits.
Non-polling I/O and kernel scheduling improvements belong to block drivers and
the kernel as well as this roadmap; writeback alone does not eliminate pauses.

**Done when:** correctness tests still pass, cache reads observe queued and
completed writes, and large directories/mounted trees have bounded memory and
measured lookup costs. Performance changes retain durability ordering. v2's
1 GiB geometry, inode table, name lengths and eight extents remain its limits.

## Closing v2 and consuming it from subsystems

v2 closes after V2-A through V2-D meet their gates: recoverable mutations, a
durable atomic-update protocol, conservative maintenance and preservation of
external installations. This is a reliable small-volume baseline, not a claim
of general Linux/Windows compatibility or production certification.

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
sector; directory mutation coverage and the consistency gap are in V2-A above.

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

## 2. `fscheck` reports, and nothing repairs

**What exists.** [`fscheck`](../../fs/SXFS_CHECK.md) reconciles the block bitmap against
the inodes in both directions and walks the tree for unreachable inodes, aliased
entries and duplicate names. It is read-only by construction and exits non-zero
on findings.

**What does not.** Freeing a leaked block, dropping an orphan, rebuilding the
claims bitmap.

**Why it fits in v2.** Repair need not change the layout. `kMaxVolumes` is now 3,
but an unused slot is not an offline repair guarantee: the target must be
quiescent and unavailable to ordinary writers. Extend the host tooling or define
a dedicated maintenance path, with the policy and acceptance in V2-C.

**What has to be decided first.** Freeing an allocation can discard recoverable
contents when its owner is unreachable. Rescue such contents before reclaiming
them and report ambiguity rather than assuming unreachable means disposable.
V2-B's pending deletions also need an explicit recovery policy.

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

## 7. Nothing caches reads

`block.cpp` caches writes (`block::write_cached`) and nothing caches reads:
every `read_file` becomes a device request, however small. A 64-byte read of an
ELF section header costs the same round trip as a full sector, and reading the
same sector twice pays twice. Under QEMU each request is a VM exit.

This is what made the Start menu take a second to open: building
progman's catalog read every binary in `/bin` through `sxe_read_section`, which
walked the section headers one 64-byte read at a time — about 7,700 requests,
541 ms in `progman-smoke`. It was fixed above the filesystem (`struct sxe_file`
reads the header table and `.shstrtab` once; the Start menu keeps its catalog
until `progman_registry_disk_signature()` changes), but every other reader of
small records — `file_assoc`, `appwiz`, the loader walking a library's headers —
still pays the full price.

**Reachable in v2:** a sector cache in the block layer, below SxFS, needs no
format change. The design questions are the size (a fixed pool in BSS, like the
rest of the kernel), eviction (LRU over a small hash), and coherence with the
write path: `write_cached` and the journal's ordered writes must update or
invalidate the cached sectors, or a read after a write returns the old bytes.
Metadata already lives in memory (the inode table, the bitmaps, the `FileRecord`
cache), so this is only for file data and directory blocks.

## 8. A directory's mtime does not move, and an mtime is whole seconds

`inode.mtime` is set when an inode is created and when its own contents are
written or truncated. **Creating, deleting or renaming an entry does not touch
the parent directory's mtime**, unlike POSIX. So "did anything in `/bin`
change?" cannot be answered by one `stat` of `/bin`: a caller has to list the
directory and stat every entry, which is what
`progman_registry_disk_signature()` does. It is cheap — no device reads, ~14 ms
for `/bin` — but it is a workaround for a missing guarantee.

And the field is whole seconds, so a file rewritten in place within the same
second at the same size is indistinguishable from the old one by metadata. The
Start menu's cache accepts that edge.

**Reachable in v2:** both.

- Bumping the parent's `mtime` belongs in `add_directory_entry` and
  `remove_directory_entry` (a rename does both), inside the same metadata
  commit, so it costs no extra journal transaction.
- Sub-second resolution fits in `reserved1`, the 4 spare bytes right before
  `mtime`, as nanoseconds. It needs a feature bit in the superblock so a reader
  knows the field is meaningful, the same way `SXFS_FEATURE_OWNERSHIP` made
  uid/gid/mode trustworthy without a v3. `struct savanxp_stat_ex` would gain an
  `st_mtime_nsec` at its end.

## What a second driver showed

Writing the FAT driver ([FAT_ROADMAP.md](FAT_ROADMAP.md)) was the first time
anything but SxFS sat under the VFS. What it showed about the VFS itself is in
[FILESYSTEMS.md](../../kernel/FILESYSTEMS.md#limits); these are SxFS's.

- **Mounting is not read-only.** `probe` replays the journal
  (`recover_journal`) before it validates the home metadata, and the replay
  writes the disk. So SxFS cannot be mounted just to look at a volume and let
  go: the search for the boot volume asks for the FAT driver by name
  (`fs::mount` with a driver name) precisely so that no SxFS volume on another
  disk is opened along the way. A probe that only reads, with the replay
  deferred to a writable attach, would remove the reason.
- **Few volumes, each expensive.** `kMaxVolumes` is 3 and each one costs about
  3.4 MiB of static metadata (item 4), reserved whether or not it is used. The
  root takes one, so two SxFS volumes can be mounted at `/mnt`, which is also
  all `fs::kMaxMounts` leaves once the root and `/boot` are in. Raising either
  means raising both, and item 4 is what makes the SxFS side cost.
- **Every operation on a vnode searches every record.** `file_from_vnode`
  walks the `FileRecord`s of every volume, up to 4095 × 3, comparing vnode
  pointers. For a directory there is no other way, since the VFS gives a
  directory vnode no data of the driver's. For a file there is: `mount_record`
  already hands the record to the VFS as `vnode.data`, and nothing reads it
  back. FAT reads it; SxFS could too, leaving the search to directories.
- **No volume label.** SxFS has none; Disk Management shows and sets the GPT
  partition name instead ([DISK_MANAGEMENT.md](../../userland/DISK_MANAGEMENT.md)),
  so a whole-disk SxFS volume has no name at all. A label in the superblock's
  reserved tail would not need a new version, as ownership did not.
- **`write(..., truncate)` ignores the offset.** With `truncate` the file is
  emptied and written from 0, whatever offset was passed; the VFS uses it for
  `O_TRUNC` with an empty write. FAT copies the behaviour so both drivers
  answer the same. It is an odd contract for a driver to learn, and a
  separate truncate-to-zero before the write would say the same thing.

## What is deliberately not broken

- **The kernel validates claims at mount.** Claimed sectors must be allocated,
  within bounds and owned by at most one inode. Leaks and unreachable inodes
  survive it on purpose: rejecting them at mount would mean refusing a volume that is
  mostly fine and still readable, and the repair path in item 2 is where that
  decision belongs.
- **Ownership did not need a v3.** uid, gid and mode took bytes the inode had
  reserved, and a feature bit in the superblock's reserved tail says they are
  recorded; a volume without it is stamped on its first writable mount. See
  [`USERS_AND_PERMISSIONS.md`](../../kernel/USERS_AND_PERMISSIONS.md#what-each-inode-stores).
- **`sxfs_walk` refuses to follow aliases.** SxFS has no hard links —
  `create_file` writes `link_count = 1` and nothing raises it — so a second path
  to one inode is a defect. The host validator fails the image on it; `fscheck`
  counts it.
