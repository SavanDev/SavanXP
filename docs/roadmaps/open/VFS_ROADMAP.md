# VFS — the shared filesystem foundation

> **Status: open.** The current VFS, mount registry, handles and drivers are
> implemented; VFS-A through VFS-E below are planned work, not completed APIs.
> This roadmap is independent of on-disk format versions. Most groundwork can
> land on SxFS v2 and FAT; larger SxFS v3 volumes must not outgrow the common
> layer's ability to name, open, cache and release their objects.

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
| Resident tree | 2048 nodes shared by volumes, devices and initramfs; drivers publish the whole tree at attach |
| Reclamation | Unlink invalidates a node but does not reuse its slot during the mount; create/delete churn can exhaust the pool independently of live file count |
| Object identity | `Vnode` includes one name and parent, coupling the file with its location; file-section cache identity relies on vnode pointers not being reused after unlink |
| Open lifetime | `IoObject` has references, offset and flags, but drivers do not receive a full open/close/vnode-release lifecycle |
| Errors | Many driver operations return bool; VFS guesses write errors and converts a failed driver read to zero bytes, which the POSIX read path treats as EOF |
| Paths | 63-byte names, 255-byte paths, lexical normalization and linear child lookup; no driver lookup on demand or symbolic-link traversal |
| Directories | Volume directories are published as memory vnodes; enumeration indexes the resident child list |
| Mounts | Four slots, 32-byte mount-point buffers and no general read-only mount flag |
| File semantics | Rename rejects existing destinations; POSIX refuses unlink of open files; append initially positions at EOF rather than enforcing append on each write |
| Large I/O | Some translation results, including the seek position, pass through int even when internal offsets are size_t |
| Native surface | SUN has no regular-file API yet; common file operations remain partly inside POSIX translation |

These are code/documentation observations, not claims that all missing cases
have committed regression tests. Establish the failing cases as each phase
starts. A driver test that stubs the VFS does not qualify the real VFS path.

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

**Open, first contract work.** Move neutral file operations behind Sx, with
SUN/POSIX translating their userland conventions. Linux and Win32 must not call
POSIX translation internals to reach a file. Preserve handle rights and
credential checks, including query-only opens that must never grant read access.

Define results that carry status and transferred length, including partial I/O,
EOF, cancellation/interruption as supported, and late device failures. Replace
the global last-error dependency as callers move to explicit results. Keep
offsets, sizes and returned positions 64-bit through every boundary, with
overflow/range checks and explicit limitations of each backing driver.

Grow the native API with a real SUN consumer and `nativetest` assertions, as
required by [`SUN.md`](../../userland/SUN.md). The planned first consumers are
the existing `cat` for open/read/close and `cp` for create/write: migrate each
separately, preserving its command name and behavior, rather than adding a new
POSIX utility to exercise a missing native slice. Do not claim that these two
consumers alone qualify all file operations.

**Done when:** real VFS tests inject read/write errors, partial transfers and
EOF without confusing them; seek beyond the signed 32-bit range round-trips
through the API on a suitable test backend. Shared handles retain correct
offsets and rights. Native consumers and existing POSIX file tests pass, and
unsupported operations return documented failure rather than success stubs.

## VFS-B — identity, references and safe reclamation

**Open; builds on VFS-A's interface work.** Separate entries from file identity
and extend the existing open descriptions. Count references from openings,
current-directory objects, directory cursors, caches and mapped sections as
appropriate. Give drivers open/close and final-release notification, so FAT can
retain per-open state and all drivers can defer reclamation correctly.

Unlink removes an entry. Where the advertised contract supports unlink while
open, existing handles keep the file accessible until its final reference goes.
Win32 may impose additional sharing/deletion rules through common mechanisms;
do not globally impose its policy on SUN/POSIX. Coordinate the persistent state
and recovery of pending deletions with each filesystem driver.

Reclaim node slots only after references are gone, invalidating cache entries
and checking reuse generations. Simply putting invalidated nodes on a free list
would break the section cache's current pointer-identity assumption.

**Done when:** sustained create/unlink/close cycles exceed the old pool's total
allocation count without exhaustion or monotonic growth. Duplicated/inherited
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

**Done when:** the real VFS uses a mounted tree beyond the old 2048-node ceiling
through a bounded cache; SxFS/FAT retain their actual format limits. Test cache
eviction, lookup misses followed by create, permissions, long names, mount
crossings and directory mutation during enumeration. Supported link traversal
has loop bounds and no-follow coverage; unsupported links fail explicitly.

## VFS-D — mutation semantics and durability

**Open; requires VFS-A/B, coordinated with VFS-C.** Define atomic replacement,
exclusive creation, unlink/rmdir and rename outcomes. Reserve/validate resources
before a driver mutation so a successful disk update cannot be followed by an
in-memory tree update failure. Cross-volume rename must return its distinct
error; a copy-and-delete utility is a separate operation, not atomic rename.

Enforce append at the write operation, not just at open. Define its atomicity
unit with concurrent writers and user-buffer chunking. Add positioned I/O that
does not change a shared opening's offset, and handle-based metadata/truncation
operations where consumers need them. Define how open-file offsets and directory
cursors behave across dup, inheritance, seek, replacement and truncation.

Expose per-file/directory synchronization and propagate deferred errors through
the shared contract. Specify which data, metadata and namespace changes a
successful checkpoint makes durable. Global sync/writeback tickets already
exist; closing an opening is not by itself a durability promise. A filesystem
that cannot provide a requested guarantee must report that limitation.

**Done when:** concurrent append, exclusive-create races, replacement with open
destinations and partial/error I/O have executable tests. A temporary-file write,
file sync, atomic replacement and directory sync has documented crash outcomes.
Exercise the real driver as well as the VFS: SxFS V2-A must fix directory
transaction consistency; VFS success cannot manufacture that disk guarantee.

## VFS-E — mounts, coherent mappings and scalable I/O

**Open; builds on references and explicit I/O results.** Give each mount
enforceable policy and capability state, including read-only protection before
every mutating operation. Separate format recognition from recovery and writable
attach. Drivers may restrict root eligibility or lack write support without
needing dummy write callbacks. Manage mount resources with explicit budgets
rather than increasing fixed arrays without measuring their cost.

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

**Done when:** read-only mounts refuse mutations even for the system caller;
unmount with live references is safe and predictable; failed attach/removal
leaks no objects. Supported mappings observe the specified write/truncate/flush
results. Concurrent lookup, rename, I/O and unmount on multiple CPUs pass lock
and lifetime tests, and slow-device I/O does not monopolize the global kernel
critical section for its entire duration. Introduce these as separate slices;
no single change is expected to complete VFS-E.

## Coordination and the baseline for future drivers

| Consumer / roadmap | Dependency |
| --- | --- |
| SxFS v2 reliability | VFS-A/B/D for errors, lifetime and durable replacement; its journal fix is independent driver work and must not wait for all VFS phases |
| SxFS v2 scale preparation | VFS-C for on-demand publication and bounded caches; format ceilings remain unchanged |
| SxFS v3 | Agree VFS/driver contracts during format design; qualify VFS-C/E before claiming large-tree or coherent-mapping support |
| FAT and future formats | Implement the common contract and advertise actual capabilities; exercise format-specific names, ownership, I/O and failure behavior |
| [Linux](LINUX_SUBSYSTEM_ROADMAP.md) | Consume measured neutral operations for L3/L4; add directory-relative I/O, links, mappings and locking with the profiles that require them |
| [Win32](WIN32_SUBSYSTEM_ROADMAP.md) | Consume common identity, rights, sharing/deletion mechanisms and mappings; translate paths, dispositions, flags and errors in Win32 |

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
