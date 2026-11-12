# VFS — the shared filesystem foundation

> **Status: open.** The current VFS, mount registry, handles and drivers are
> implemented. No phase below is complete; SxFS v2 (closed) and the first SUN
> file calls landed the parts listed under "Where each phase stands".
> This roadmap is independent of on-disk format versions. Most groundwork can
> land on SxFS v2 and FAT; larger SxFS v3 volumes must not outgrow the common
> layer's ability to name, open, cache and release their objects.
>
> Last checked against the tree on 2026-11-11, after SxFS V2-D.

The goal is one filesystem foundation for SUN, current POSIX, planned Linux and
Win32, and future filesystem drivers. Keep the existing separation: `vfs::`
handles names and common file mechanisms, `fs::` manages drivers and mounts, and
each driver owns its format and recovery. The current architecture and limits
are described in [`FILESYSTEMS.md`](../../kernel/FILESYSTEMS.md); disk-format
work belongs in [`SXFS_ROADMAP.md`](SXFS_ROADMAP.md) and
[`FAT_ROADMAP.md`](FAT_ROADMAP.md).

Hardware identity, Plug and Play, DMA/request lifetime and the planned SUN Device
Manager are tracked in [`DRIVERS_ROADMAP.md`](DRIVERS_ROADMAP.md). VFS-B/E and
that roadmap share the open-reference and device-gone teardown contract; a
removed disk must not depend on a successful final write to disappear safely.

## What already exists and what still limits it

The root comes from a registered filesystem driver, with FAT at `/boot`, devices
under `/dev`, and the rescue initramfs under `/rescue`. Permissions are checked
centrally with caller credentials. Mount/unmount slots can be reused; unmount
checks open handles and file-backed sections before detaching a tree. Handles
carry granted rights, and the reference-counted `IoObject` already provides
shared state for duplicated handles. Preserve these mechanisms while extending
them rather than building a separate file implementation per subsystem.

The implementation baseline is `kernel/vfs.cpp`, `kernel/fs.cpp`, their headers,
`include/kernel/object.hpp`, and the file operations currently in
`subsystems/posix/kernel/posix_translation.cpp`:

| Area | Current limit |
| --- | --- |
| Resident tree | 2048 nodes shared by volumes, devices and initramfs; FAT and SxFS publish a directory when it is first looked into (`populate`), attach publishing only the mount point; published on-demand directories are given back, least recently looked into first, when free slots drop below 256 (`vfs::trim_if_low`, at system-call entry) |
| Reclamation | Node slots are reused once a node is gone and released; a held node keeps its slot until its last `vfs::release` |
| Object identity | `Vnode` includes one name and parent, coupling the file with its location; caches key on the vnode plus `content_generation`, which never repeats across slot reuse |
| Open lifetime | `IoObject` has references, offset and flags; drivers get `open`/`close` at a vnode's first and last holder (FAT keeps a FatFs file open between them) and `release` for the last holder of an unlinked file, but nothing per opening |
| Holders | Counted on the vnode (`Vnode::references`, `vfs::retain`/`release`/`is_held`) by open handles, file-backed sections and POSIX's current directory (a retained vnode, whose path is rebuilt with `vfs::path_of` and follows renames). Directory cursors are held through their handle; there are no name caches yet |
| Errors | Driver operations return `fs::Status` and a failed read is an error, not EOF; handle I/O returns `sx::IoResult` (status and length), but path operations and `vfs::read`/`vfs::write` themselves still report through the global `vfs::last_error()` |
| Paths | 255-byte names (up to 47 bytes in the node's slot, longer on the heap), 1023-byte paths of up to 64 components (`vfs::PathBuffer` keeps them off the kernel stack), lexical normalization and linear child lookup; no symbolic-link traversal |
| Name comparison | By the directory's driver (`fs::Driver::same_name`), byte for byte without one; FAT folds case as FatFs does. No 8.3 short-name aliases |
| Directories | Volume directories are published as memory vnodes; a directory handle lists through a VFS cursor (`vfs::DirCursor`) that changes to the directory move instead of break |
| Mounts | Four slots and 32-byte mount-point buffers; a read-only mount is a flag of `fs::attach` the VFS enforces, with no remount and no per-mount policy beyond it |
| File semantics | Rename replaces and unlink keeps open files alive where the driver supports it (SxFS; FAT answers `EEXIST`/`EBUSY`); append initially positions at EOF rather than enforcing append on each write; no exclusive-create flag and no positioned I/O |
| Mutation order | A path mutation reaches the driver first and the resident tree second; a tree update that failed after the driver succeeded would leave the two disagreeing |
| Durability | `fsync` and `sync` are one whole-system barrier; a deferred write error is the `EIO` of whoever synchronizes next, not of the handle that wrote |
| Mappings | File-backed sections are private copies of one content generation; no shared writable mapping and no page cache coherent with writes |
| Large I/O | Internal offsets are `size_t`, but the seek position and transfer counts return through `int`, and the stat ABI carries a 32-bit size |
| Native surface | SUN opens and reads an existing regular file (`sxn_open_file`, `sxn_read`), saves a whole file of up to 4096 bytes (`sxn_write_file`), queries a path (`sxn_query_path`), lists a directory (`sxn_open_directory`, `sxn_read_directory`) and streams a new file (`sxn_create_file`, `sxn_write`, `sxn_flush_file`); no seek and no path operations. Native dispatch and POSIX translation open, read, write, seek and close through `sx::` (`kernel/sx_file.cpp`) |

The contract these rows describe is
[`FILESYSTEMS.md`](../../kernel/FILESYSTEMS.md#the-file-contract), landed with
SxFS V2-B and tested through the real VFS by `sxfs-vfs-test`. The phases below
keep what it did not do.

These are code/documentation observations, not claims that all missing cases
have committed regression tests. Establish the failing cases as each phase
starts. A driver test that stubs the VFS does not qualify the real VFS path.

## Where each phase stands

| Phase | Landed | Still open |
| --- | --- | --- |
| VFS-A | `fs::Status` from every driver operation; read errors distinct from EOF; `sxn_open_file`/`sxn_read` and `sxn_write_file` with their consumers and `nativetest` assertions; one open/read/write/seek/close path in `sx::` (`kernel/sx_file.cpp`) with `sx::IoResult`, called by native dispatch and POSIX translation (step 1) | Path operations (stat, unlink, rename, mkdir...) still return through `vfs::last_error()`; 64-bit positions and sizes; the rest of the native surface |
| VFS-B | Unlink while open; `release` for the last holder; generation-checked slot reuse, held over 5000 create/delete rounds by `sxfs-vfs-test`; references counted on the vnode by handles and file-backed sections, replacing the holder probe; driver `open`/`close` at the first and last holder, with FAT keeping its file open; the POSIX current directory held | Entries apart from file identity; driver state per opening; directory cursors |
| VFS-C | Slice 1: directories published on demand (`vfs::install_volume_directory`, `fs::Driver::populate`/`forget`), FAT first. Slice 2: the bounded cache (`vfs::trim`). Slice 3: SxFS on demand; an SxFS tree larger than the pool is read whole in `sxfs-vfs-test`. Slice 4: name comparison by the driver, FAT case-insensitive; 1023-byte paths and 255-byte names (`fat-vfs-test`); `ENAMETOOLONG`. Slice 5a: enumeration cursors | Directory-relative resolution (5b) |
| VFS-D | Replacing rename, `EXDEV`, `fsync` as the global barrier, the durable-update protocol cut at every sector on SxFS | Exclusive create, append per write, positioned I/O, per-file barriers, errors delivered to the writer, validation before every driver mutation |
| VFS-E | Read-only mounts from attach; a `probe` that writes nothing; root eligibility (`can_be_root`); absent write callbacks answered with `EROFS`; unmount that refuses held nodes | Remount and policy on open handles; device removal; coherent mappings; lock order and I/O outside the global lock |

## The object and driver contracts to establish

Separate these responsibilities; exact type and callback names are not frozen
by this roadmap:

| Object | Owns |
| --- | --- |
| Mount instance | Driver/volume association, identity, policy, supported capabilities and lifetime |
| File object / vnode | Identity independent of names, type, metadata, driver state and references held by opens, caches and mappings |
| Directory entry | One name-to-object relation under a parent; several entries may refer to one file when supported |
| Open description | Per-open driver state, offset, status flags, enumeration cursor and deferred-error observation; duplicated handles share it where the subsystem contract requires |
| Handle | Granted rights and handle-local flags; closing one duplicate does not close the shared opening prematurely |

Use identity scoped to the mount/volume with a reuse generation. Distinguish
identity generation from content generation: preventing a reused slot from
aliasing an old object does not invalidate cached bytes after an in-place write.
The final reference releases an object; a removed name alone does not.

The driver interface must describe supported operations and return explicit
status/results. It needs object-relative lookup, metadata and directory
enumeration, open/close/release hooks, I/O and synchronization. Directories must
carry their driver/volume identity just as files do. Optional operations must be
checked before invocation and fail explicitly when unsupported. A format without
links, persistent owners or atomic replacement must not advertise them or be
silently given a lossy imitation. Preserve the existing explicit owner policy
for FAT rather than claiming that it stores native ownership.

## VFS-A — common I/O results and the native file surface

**Open, first contract work.** Driver status results and read errors distinct
from EOF are in place, and SUN has its first two file calls. Move neutral file
operations behind Sx, with SUN/POSIX translating their userland conventions.
Both `native_open_file` and the POSIX open now build the `IoObject` with
`sx::open_file`, so a third subsystem does not copy it a third time.
Linux and Win32 must not call POSIX translation internals to reach a file.
Preserve handle rights and credential checks, including query-only opens that
must never grant read access.

Define results that carry status and transferred length, including partial I/O,
EOF, cancellation/interruption as supported, and late device failures. Replace
the global last-error dependency as callers move to explicit results. Keep
offsets, sizes and returned positions 64-bit through every boundary, with
overflow/range checks and explicit limitations of each backing driver.

Grow the native API with a real SUN consumer and `nativetest` assertions, as
required by [`SUN.md`](../../userland/SUN.md#files-and-small-profile-saves).
Open/read landed with compositord's cursor catalog as its consumer and the
whole-file save with the cursor profile; the Win32 plan stores its per-user
settings through the same save. Metadata queries, streaming create/write and
directory enumeration landed together with the SUN file dialog and Log
Viewer's Save Log... as their consumers. What is missing, each slice with a
consumer of its own: seek, 64-bit positions through the handles, and the path
operations (mkdir, rename, unlink) a New Folder button would need. The existing
`cat` is still a planned consumer of sequential reads and `cp` of streaming
writes: migrate each separately, preserving its command name and
behavior, rather than adding a new POSIX utility to exercise a missing native
slice. Do not claim that these consumers alone qualify all file operations.

Suggested order, each slice shippable alone:

1. One neutral open/read/write/seek/close path in `sx::` that returns status
   and length, with POSIX translation and native dispatch as its two callers.
   No new API; existing tests are the regression. **Done:** `sx::open_file`
   (what it accepts besides a regular file is the caller's `sx::OpenAccept`),
   `read_file`, `write_file`, `seek_file` and `close_handle` return
   `sx::IoResult` or `sx_status`; `sx::io_result_value` turns one into a
   syscall's `int`. Positions are already 64-bit inside `seek_file`; the
   `int` the syscalls return is step 2's.
2. 64-bit positions and sizes through seek and stat, then the native size and
   seek calls.
3. Native streaming create/write, then enumeration. **Done:**
   `SXN_SYS_QUERY_PATH`, `OPEN_DIRECTORY`, `READ_DIRECTORY` (over the
   VFS-C enumeration cursors), `CREATE_FILE` and `FLUSH_FILE`, with the file
   dialog as their consumer.

**Done when:** real VFS tests inject read/write errors, partial transfers and
EOF without confusing them; seek beyond the signed 32-bit range round-trips
through the API on a suitable test backend. Shared handles retain correct
offsets and rights. Native consumers and existing POSIX file tests pass, and
unsupported operations return documented failure rather than success stubs.

## VFS-B — identity, references and safe reclamation

**Open; builds on VFS-A's interface work.** Unlink while open, final-release
notification and generation-checked slot reuse are in place for one name per
file. Separate entries from file identity and extend the existing open
descriptions. References are counted on the object (`Vnode::references`):
open handles take one in `sx::open_file`, file-backed sections one for their
lifetime; this replaced a holder probe that scanned every I/O object per
question. POSIX's current directory is a retained vnode too: it follows a
rename of itself, and keeps its directory from being removed or unmounted.
Directory cursors and caches take theirs when they exist. Drivers get `open`/`close` next to `release`, at a vnode's
first and last holder: FAT keeps one FatFs file open between them instead of
reopening the file and walking its cluster chain on every read. That is state
per file; state per opening (directory cursors, locks) waits for the open
descriptions above.

Unlink removes an entry. Where the advertised contract supports unlink while
open, existing handles keep the file accessible until its final reference goes.
Win32 may impose additional sharing/deletion rules through common mechanisms;
do not globally impose its policy on SUN/POSIX. Coordinate the persistent state
and recovery of pending deletions with each filesystem driver.

Node slots are already reclaimed only once a node is gone and released, and
the section cache keys on the vnode plus a `content_generation` that survives
reuse ([`SECTIONS.md`](../../kernel/SECTIONS.md)). Keep that property when
identity stops being a slot address: introduce the identity generation
separately, so that reuse and an in-place write stay distinguishable.

**Done when:** sustained create/unlink/close cycles exceed the old pool's total
allocation count without exhaustion or monotonic growth (met today for SxFS
with one name per file; repeat it with open handles, FAT and the new identity).
Duplicated/inherited
handles, rename, open-file unlink and mapped sections retain the correct object;
the last reference reclaims once. Reuse never exposes a different file through
an old handle/cache entry, and failures leave reference counts balanced.

## VFS-C — component resolution and bounded directory caches

**Open; requires VFS-B identity/lifetime.** Replace mandatory tree publication
with driver lookup and enumeration on demand. Cache names and objects within
explicit budgets, including invalidation of negative lookups after mutations.
Mounting and accessing a small working set must not require memory proportional
to every entry on every volume. Avoid repeated linear scans for large directory
lookups and index-based enumeration from the start on every call.

Resolve components relative to a directory object, with explicit follow/no-follow
rules, mount crossings, limits and errors. Check directory-search permissions
along the actual walk. `.` and `..`, trailing separators and later symbolic links
need traversal semantics; lexical removal of components is not a replacement.
Introduce directory-relative operations with stable directory references rather
than reconstructing absolute strings. File formats retain their own component
limits, while the VFS must not truncate names silently.

Define filesystem-native name comparison as a driver/mount capability, preserving
FAT's comparison rules without changing SxFS globally. Subsystem lookup views
such as Win32 case behavior and Linux runtime roots remain explicit policies;
a guest root mapping is not an authorization boundary. Preserve `/disk` aliases
used by installed binaries until a separately validated compatibility transition.

Directory enumeration needs driver-owned cookies/cursors with a defined response
to concurrent changes, buffer exhaustion and cursor restart. Hard/symbolic links
are available only when the backing format and common layer support them;
SxFS's complete link model remains coordinated with its v3 plan.

Slices, each shippable alone:

1. **Directories published on demand. Done for FAT.** A volume's directory vnode carries
   its driver and whether its entries are published. The first lookup or
   enumeration of an unpublished one asks the driver (`populate`) for that
   directory's entries only; attach publishes nothing below the mount point.
   A mount costs one directory, and a volume larger than the pool mounts:
   only what is visited is resident. FAT first, the format of the sticks
   that ran into the ceiling.
2. **A bounded cache. Done.** Published directories nothing holds and nobody
   visited lately are given back (their entries retired, the driver told to
   `forget` them) to keep free slots above a low-water mark. Eviction runs
   only at a point where no unretained vnode pointer can exist: the entry of
   a system call, under the kernel lock. Splitting the VFS off that lock
   (SMP phase 5.6) has to revisit this rule.

   As built: `vfs::trim_if_low()` runs at the top of `handle_syscall`. Below
   `kMaxNodes / 8` free slots it calls `vfs::trim(kMaxNodes / 4)`, which
   takes the on-demand directory looked into longest ago (`Vnode::last_used`,
   stamped on every look) and gives back its whole subtree, when nothing in
   it is held or unlinked, all of it belongs to that directory's driver, and
   no other volume is mounted inside. The directory itself stays, marked
   unpublished; a held directory (an open listing, a current directory) keeps
   its own entries. A trim that cannot reach the low-water mark is not
   retried until 64 more directory looks have happened, so a tree full of
   held or always-resident nodes is not searched on every call.

   **The rule this sets for all kernel code:** a `Vnode*` kept from one
   system call to the next is retained (`vfs::retain`), or it may point at a
   slot given to another node. Inside one call, an unretained pointer is
   safe as long as the call does not sleep in `smp::wait_for_interrupt()`
   between finding it and using it; the sleeping calls today (tty, network,
   PC speaker, the idle yield) hold none. Driver records point at their
   vnodes and are dropped by `forget`.
3. **SxFS on demand. Done.** With the same two hooks. SxFS already keeps a
   record per inode in memory, so `populate` finds the directory by its path
   and publishes the records whose parent it is, and `forget` only drops the
   record's vnode pointer. A directory the tree already has with no driver
   (`/dev`, `/rescue`) is merged, one another volume publishes (a mount
   point) is left to it (`vfs::publish_volume_directory`). Republishing a
   record clears any other record still pointing at that slot. The record
   table (4095 per volume) stays resident: it is SxFS's format limit, not the
   VFS's.
4. **Names**: comparison as a mount capability (FAT case-insensitive), and
   names past 63 bytes.

   Comparison is done (4a). It is a driver hook, `same_name`, that the
   child lookup uses in the directories a driver publishes; FAT's compares
   code points through `ff_wtoupper`, the table FatFs compares long names
   with, so the VFS finds exactly what `f_open` would. Creating a name that
   exists in another case opens the existing entry, and a rename to another
   case of the same name renames instead of being a no-op. Because a caller
   may spell the directories above a new name differently from the disk,
   create, mkdir and rename hand the driver a path rebuilt from the tree
   (`spelled_path`), which keeps drivers that store paths byte-exact.
   Names past 63 bytes are 4b. Its first step is done: paths went from 255
   to 1023 bytes (a 255-byte name did not fit in a path), held on the heap
   (`vfs::PathBuffer`) in the VFS and both dispatchers, FAT's records keep a
   path sized to fit and reserve the room a rename needs before touching the
   disk, and `normalize_path` no longer copies components onto the stack.
   The second step is done too: names reach 255 bytes. A slot keeps up to 47
   bytes of name in place (it held 64, so the pool shrank) and a longer name
   is a heap string of its size; a rename reserves the new name's storage
   before the driver changes the disk, so the tree cannot be left behind.
5. **Directory-relative resolution and enumeration cursors**, the rest of
   the paragraphs above.

   Enumeration cursors are done (5a). A directory handle carries a
   `vfs::DirCursor`: the slot of the next entry, on a list the VFS keeps, so
   `detach_node` moves every cursor about to return a node it takes out of
   a directory on to the entry after it. Nothing is skipped when an entry
   is deleted right after being read (`rm -r`) or right before; entries
   added are appended and listed if the cursor has not reached the end; an
   entry renamed inside the directory goes to the end and may be listed
   again under its new name. Each step is O(1) where `child_at` walked from
   the start every call, and restart is `rewind_cursor` (the SDK's
   `rewinddir` reopens). The cursor is the VFS's, not a driver cookie: the
   VFS holds the directory's entries while it is published, and a listed
   directory is held, so the bounded cache never trims it under a cursor.
   Driver cookies matter only once a directory is too large to publish.

   Directory-relative resolution is still open (5b).

**Done when:** the real VFS uses a mounted tree beyond the old 2048-node ceiling
through a bounded cache; SxFS/FAT retain their actual format limits. Test cache
eviction, lookup misses followed by create, permissions, long names, mount
crossings and directory mutation during enumeration. Supported link traversal
has loop bounds and no-follow coverage; unsupported links fail explicitly.

## VFS-D — mutation semantics and durability

**Open; requires VFS-A/B, coordinated with VFS-C.** Atomic replacement, the
cross-volume error (`EXDEV`) and `fsync` as the global barrier are in place,
with crash outcomes documented and tested on SxFS
([the file contract](../../kernel/FILESYSTEMS.md#the-file-contract)). Add
exclusive creation, which has no open flag today, and state the unlink/rmdir
and rename outcomes for formats that cannot replace or keep an open file.
Reserve/validate resources before a driver mutation so a successful disk update
cannot be followed by an in-memory tree update failure: rename still calls the
driver first. A copy-and-delete utility across volumes is a separate operation,
not atomic rename.

Enforce append at the write operation, not just at open. Define its atomicity
unit with concurrent writers and user-buffer chunking. Add positioned I/O that
does not change a shared opening's offset, and handle-based metadata/truncation
operations where consumers need them. Define how open-file offsets and directory
cursors behave across dup, inheritance, seek, replacement and truncation.

Expose per-file/directory synchronization cheaper than the global barrier, and
deliver a deferred write error to the opening that wrote the bytes as well as
to the next synchronizer. Specify which data, metadata and namespace changes a
successful checkpoint makes durable. Global sync/writeback tickets already
exist; closing an opening is not by itself a durability promise. A filesystem
that cannot provide a requested guarantee must report that limitation.

**Done when:** concurrent append, exclusive-create races, replacement with open
destinations and partial/error I/O have executable tests. A temporary-file write,
file sync, atomic replacement and directory sync has documented crash outcomes
(met on SxFS by `sxfs-vfs-test`; FAT documents that it cannot replace). Exercise
the real driver as well as the VFS: the disk guarantee comes from the driver, as
SxFS V2-A's namespace transactions did, and VFS success cannot manufacture it.

## VFS-E — mounts, coherent mappings and scalable I/O

**Open; builds on references and explicit I/O results.** The groundwork is in:
a read-only mount is chosen at attach and refused with `EROFS` before any
driver is called, `probe` recognises and recovers in memory without writing,
only a writable attach persists a recovery, `can_be_root` restricts root
eligibility and a driver without write support leaves those callbacks null.
Give each mount enforceable policy and capability state beyond that flag:
the read-only check runs where an operation starts from a path, so a write
through a handle opened earlier reaches the driver, which is what a remount
must close first. Manage mount resources with explicit budgets rather than
increasing fixed arrays without measuring their cost.

Unmount must block new operations, handle existing references consistently,
synchronize, and release caches/driver state in a documented order. Failed attach
and unmount must roll back without publishing half a tree. Define behavior for
device removal and failed backing I/O; do not dereference released driver state
or pretend lost dirty data was saved.

Coordinate VM, file sections, page/block caches and drivers on content generation,
dirty tracking, read/write coherence, truncation, writeback and mapping lifetime.
Define private/shared mappings, explicit flush behavior and faults after shrink
as supported. Do not treat current read-only file-image sections as a writable
coherent page cache. Preserve mapping rights and the platform's W^X policy.

Before relaxing the kernel's coarse serialization, document ownership and lock
order for mounts, directories, file objects, openings and cache/writeback state.
Support waiting for device I/O without holding a global filesystem lock through
the wait; cancellation, failure and completion must retain references. Regular
files' readiness, notifications and advisory/range locks need explicit contracts
when a consumer requires them, not assumed support from a nonblocking flag.

**Done when:** read-only mounts refuse mutations even for the system caller,
on open handles and after a remount as well as at attach;
unmount with live references is safe and predictable; failed attach/removal
leaks no objects. Supported mappings observe the specified write/truncate/flush
results. Concurrent lookup, rename, I/O and unmount on multiple CPUs pass lock
and lifetime tests, and slow-device I/O does not monopolize the global kernel
critical section for its entire duration. Introduce these as separate slices;
no single change is expected to complete VFS-E.

## Coordination and the baseline for future drivers

| Consumer / roadmap | Dependency |
| --- | --- |
| SxFS v2 reliability | Done: it landed the driver status results, unlink while open, replacing rename and `fsync` it needed, in this layer |
| SxFS v2 scale | Done: on-demand publication and the bounded cache (VFS-C slices 1-3); format ceilings remain unchanged |
| SxFS v3 | Agree VFS/driver contracts during format design; qualify VFS-C/E before claiming large-tree or coherent-mapping support |
| [FAT](FAT_ROADMAP.md) and future formats | Implement the common contract and advertise actual capabilities; exercise format-specific names, ownership, I/O and failure behavior. FAT waits on VFS-B for per-open state |
| [Linux](LINUX_SUBSYSTEM_ROADMAP.md) | Consume measured neutral operations for L3/L4; private file mappings arrive with its S2b slice at L4; add directory-relative I/O, links and locking with the profiles that require them |
| [Win32](WIN32_SUBSYSTEM_ROADMAP.md) | Its first profile stores settings with `sxn_write_file` and installs names lower-cased until a native enumeration call exists. Later: common identity, rights, sharing/deletion mechanisms and mappings; paths, dispositions, flags and errors are translated in Win32 |
| [Drivers D5](DRIVERS_ROADMAP.md#d5--general-plug-and-play-and-safe-removal) | VFS-B references and VFS-E teardown define what a removed disk leaves behind; USB storage is the first consumer |

The sequence is VFS-A, then VFS-B, with VFS-C/D developed against those shared
contracts; mount policy groundwork can start early, while VFS-E's VM/concurrency
work depends on stable lifetimes. Initial foreign executable milestones need
only their measured subset, not the completion of this entire roadmap.

Qualify changes with a reusable conformance harness linking the **real VFS** to
fault-injecting drivers, plus integration with SxFS, FAT, memory/rescue and
device paths. Cover unsupported capabilities, resource exhaustion, repeated
create/delete/mount/unmount, errors before and after partial progress, credential
denials, shared handles and stale-cache prevention. Keep filesystem power-cut
tests separate from VFS lifetime tests, and run both for durability claims.

Existing driver/registry host tests and native/POSIX smokes remain regressions,
not substitutes for the shared harness. New native API slices need their SUN
consumer and `nativetest` assertions. Implementation changes must preserve boot,
rescue, installed programs, ownership and persistent data; use
`./tools/verify_doom_persistence.sh` for persistent-image changes and
`./build.sh smoke sxgui-smoke` when the loader or drawing programs are touched.

The baseline is met when advertised operations pass common conformance tests
and a new filesystem can attach through the driver contract without adding its
format name to VFS dispatch. Not every format must implement every capability.
Mount namespaces for containers, overlays, network filesystems and additional
notification/locking profiles are later consumer-driven work, not prerequisites
for the first Linux/Win32 applications. Record supported profiles, scale and
remaining limitations before calling the common layer production-ready.
