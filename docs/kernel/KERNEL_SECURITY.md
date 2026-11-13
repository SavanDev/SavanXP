# Kernel security model and hardening

Status: audited on 2026-11-04 (third pass, against `bd08141`) and reconciled
with the tree on 2026-11-11 (fourth pass, against `cd58936`). The first audit
and its second-pass review date from 2026-09-23.

This document defines SavanXP's current kernel security boundary, records the
findings from the kernel audits, and distinguishes mitigations already in the
tree from risks that still require architectural work. It is not a claim that
SavanXP is a hardened multi-user operating system. The newest findings, and
what the third pass verified, are in
[Third-pass audit](#third-pass-audit-2026-11-04). What changed under the
document since, and which parts of the kernel no audit has read, is in
[Fourth-pass review](#fourth-pass-review-2026-11-11).

## Trust boundary

The kernel treats the following inputs as hostile unless a stronger statement
is made below:

- ELF files executed from any SxFS volume: `/bin`, `/system`, `/rescue/bin`,
  the user's profile, and a disk or USB stick mounted under `/mnt` (a FAT
  volume never gives a file the execute bit, so nothing runs from one);
- pointers, lengths and flags supplied through system calls;
- SxFS and FAT metadata and directory trees read from disk, and GPT and MBR
  partition tables;
- descriptors, reports and transfer lengths supplied by USB devices;
- the initramfs CPIO archive supplied by the boot configuration;
- packet lengths and used-ring entries reported by NIC hardware;
- ACPI, PCI and MMIO descriptions supplied by firmware or virtual hardware;
- resource exhaustion requests from otherwise valid processes;
- the user's processes as seen by the system's: port names, connections and
  handles passed over pipes, and every path or file a user process names.

The boot chain itself is not authenticated. Limine, the kernel, initramfs and
disk image are trusted at this boundary. Until verified boot or UEFI Secure Boot
is integrated, physical or boot-media modification is outside the mitigation
set below.

Every process carries credentials (uid, euid, gid) and the VFS checks them on
every path operation. The desktop and what it starts run as the user, and the
system's rights are reached only through the elevation prompt
([`USERS_AND_PERMISSIONS.md`](USERS_AND_PERMISSIONS.md)). That keeps the
user's programs out of the system; it does not keep them from each other,
since they are one person's, and it assumes the boot chain is intact.

## Existing structural defenses

The audit confirmed the following defenses were already present:

- each process has a separate address space and shares only the high half of
  the kernel page tables;
- user-copy helpers walk all four paging levels and require user permissions;
- user stack growth is bounded by a reserved region and a guard page;
- the system call gate accepts ring 3 only and the entry stubs clear `DF`;
  from the 32-bit user code segment (`0x2b`) it returns `ENOSYS` without
  reading any register, since the ABI is defined for 64-bit callers only
  ([ARCH.md](ARCH.md#segments));
- ring-3 code may run in 32-bit compatibility mode, flat and ring 3 like the
  64-bit segments, so it reaches nothing a 64-bit program could not; a fault
  there ends the process as any user exception does;
- a kernel lock serializes system calls, interrupts, scheduling and process
  teardown;
- file-descriptor access bits are checked by normal read and write operations;
- kernel object references are retained across pipes, fork and section views;
- SxFS has primary and secondary superblocks, checksums, journaling and
  read-only recovery;
- block requests are bounded by the backing device;
- process, handle, pipe, socket and section pools have fixed global limits.

## Hardening implemented in this audit

### User/kernel memory separation

- `map_page()` now rejects every non-canonical or kernel-half address. It is a
  user-page API; kernel mappings cannot be created through it.
- NX is a leaf-page permission. The loader, stack mappings and section views set
  bit 63 only on PTEs; the bit is removed before creating upper paging levels.
  It is a derived permission, not a constant: a section view maps without NX only
  when the view requests execution and the section grants it. See
  [`SECTIONS.md`](./SECTIONS.md).
- SavanXP now requires an NX-capable CPU and enables `EFER.NXE` on every logical
  processor during early CPU initialization. The static kernel image inherited
  from Limine is not yet repartitioned into executable and NX mappings, so this
  phase enforces W^X for user pages and NX for dynamically mapped kernel pages,
  not yet for all kernel data.
- When CPUID advertises SMEP, every logical processor also sets `CR4.SMEP` and
  `CR0.WP`. The baseline is the BSP's: on a CPU without SMEP every core runs
  without it, and an AP that cannot match a BSP that has it remains offline.
  (Until phase 4 of [SMP_ROADMAP.md](../roadmaps/open/SMP_ROADMAP.md) the APs demanded SMEP
  outright, so on such a CPU they all stayed offline.)
- SMAP follows the same rule: when CPUID advertises it, every core sets
  `CR4.SMAP` (or the boot says `cpu: SMAP unavailable on this processor`), and
  an AP that cannot match the BSP stays offline. See
  [User-access windows](#user-access-windows-smap).
- ELF `PT_LOAD` segments marked both writable and executable are rejected.
  Executable segments are mapped without `PF_W`; non-executable segments, BSS
  and stacks are mapped with NX. Section views no longer belong in that
  sentence: execution is now a permission they can hold, granted by the section,
  and what stays unconditional is that **no view is ever writable and executable
  at the same time** — refused in `vm::map_section_view`, so no caller including
  `fork` can produce one, and again in `map_view_handle`, where it is reported
  as `EINVAL` instead of the generic `ENOMEM`.
- `mmap` does not gain `PROT_EXEC`. It still returns `ENOSYS`, and `mmaptest`
  asserts it: an executable mapping is reachable only through the explicit
  `section_create` + `map_view` pair, where the grant and the W^X rule are
  enforced. Widening `mmap` would have made executable memory reachable from the
  path least able to say why it is allowed.
- Two `PT_LOAD` segments may share a page. This is not a relaxation of W^X: the
  page takes the **union** of both segments' permissions, and a union that is
  both writable and executable is still rejected. It had to change because every
  binary linked with an interpreter has a text segment that does not end on a
  page boundary, and the previous validation refused those images outright. The
  union is computed *before* mapping, because a page cannot lose permissions
  depending on order: map the executable segment first and a plain read-only
  segment arriving later would put `NX` back on the text.
- User stack and shared-section pages preserve the NX bit across fork. For a
  section view the flag is derived again on the clone from the view's own
  `access_mask`, so an executable view stays executable in the child and a
  non-executable one stays NX. A `share_on_fork` view is re-mapped onto the same
  section object and shares pages; a private one is re-mapped onto a full page
  copy made by `clone_section`, which is a copy at fork time and not
  copy-on-write.
- Kernel stacks grow from 16 KiB to 32 KiB. Canaries detect corruption before a
  normal return, although a dedicated non-present guard page is not implemented
  yet. The eight-page backing allocation is intentionally contiguous: it trades
  fragmentation resistance for a simple, bounded stack layout until the PMM can
  provide non-contiguous ownership and guard-page support.

### User-access windows (SMAP)

With SMAP on, a kernel load or store to a user page faults unless `RFLAGS.AC`
is set. The rules that keep that true:

- **Only the copy runs inside the window.** `arch::user_access_begin()` (`stac`)
  and `user_access_end()` (`clac`), or the scoped `arch::UserAccess`, wrap the
  access itself; validation (`vm::is_user_range_accessible`) stays outside.
  Both are no-ops on a CPU without SMAP, where the instructions are `#UD`.
- **The windows in the tree:** `sx::read_user_memory` and `write_user_memory`
  (every `copy_from_user`, `copy_to_user`, user string and path copy goes
  through them), `read_wait_word` for wait-on-address, and the three present
  paths whose backend reads the caller's pixels in place (`GPU_IOC_PRESENT`,
  `GPU_IOC_PRESENT_REGION`, `SXN_SYS_GFX_PRESENT`). New code copies through the
  helpers; a new in-place reader opens its own window and is listed here.
- **Every kernel entry closes the window.** Ring 3 may set `AC` itself with
  `popf`, and an interrupt gate does not clear it, so the `context.S` stubs and
  the `__attribute__((interrupt))` handlers run `clac` first. `iretq` restores
  the interrupted code's own `AC`, so an exception inside a window is safe.
- **A violation is a kernel bug and stops the system.** The `#PF` report adds
  `exception: SMAP violation, kernel access to user memory outside a
  user-access window` when a present user page was refused to a supervisor
  data access.

The windows do not depend on exception-table recovery: every access is
validated against the page tables first, under the kernel lock.

### ELF validation

The loader now rejects an image before exposing its entry point when it has:

- an invalid ELF version, header size, program-header table or excessive header
  count;
- `p_filesz > p_memsz`;
- an overflowing `p_offset + p_filesz` or `p_vaddr + p_memsz`;
- invalid load-segment alignment;
- an address outside the lower-half user range or overlapping the user stack;
- page overlap between load segments;
- writable and executable segments at the same time;
- an entry point outside an executable load segment.

The maximum is 64 program headers and 64 load segments. This is deliberately
smaller than the architectural ELF limit: the table is validated and held on
the fixed kernel stack, so an untrusted image cannot turn header count into
unbounded kernel stack use. SavanXP also deliberately rejects `p_memsz == 0`.
Two segments may share a page since the shared-library work, under the
permission-union rule described above, which never creates W+X; segments that
overlap by more than that shared page are still refused.

### Compiler hardening

- Kernel, uACPI, in-tree POSIX userland, external SDK applications, FFmpeg
  and the native SDK are compiled with `-fstack-protector-strong`.
- `crt0` installs a fresh nonzero canary from a kernel register before calling
  `main`. The value is not stored in the executable image; `fork` inherits it
  with the parent's context and address space, while `exec` receives a new one.
- `_start` seeds the kernel canary before the first protected frame and the
  kernel keeps it stable for the rest of boot; a detected failure panics.
- The POSIX runtime supplies the compiler runtime symbols required by
  Clang.

The TSC fallback is not cryptographic. The canaries improve corruption detection
and make prebuilt stack cookies less useful, but are not presented as an entropy
source for network keys, signatures or storage encryption.

### Mapping randomization

The start of the 16 GiB section-view window is selected per new process and
`exec` from RDRAND/TSC-derived kernel entropy. This randomizes dynamic malloc
arenas, GPU sections and other section-backed mappings. Fork preserves the
parent's mappings.

This is partial ASLR only. The executable image, BSS and user stack are still
at fixed addresses, and the kernel is not relocated.

### Storage and boot parsers

- SxFS now validates every inode before use and before journal replay:
  `extent_count` is bounded, extents are nonempty, contained in the data area,
  non-overlapping both within and across inodes, covered by the allocation bitmap
  and large enough for the inode size. Inode IDs, allocation bitmaps and inode
  types must agree.
- A journal containing invalid metadata is rejected before any of it is written
  to the home metadata area.
- Directory entries require valid terminated names, valid IDs, matching file or
  directory types and matching inode types.
- Recursive directory reconstruction carries a visited bitmap and a 32-level
  depth cap, preventing self-references and ancestor cycles while keeping the
  worst-case frames within the bounded 32 KiB kernel stack.
- The superblock's total sector count is checked against the real block device.
- The initramfs parser now requires strict hexadecimal fields, exactly one NUL
  at the end of each name, supported regular/directory file types, bounded
  lengths, checked padding, and a `TRAILER!!!` record. A truncated, malformed or
  over-capacity archive leaves the VFS unready instead of mounting partial data.

### Device and DMA boundaries

- Imported GPU surface geometry is calculated in 64 bits. A `pitch * height`
  overflow can no longer make a tiny buffer appear large enough for a scanout.
- GPU section import now requires read access rather than merely query access.
- virtio-net rejects used-element lengths above the RX buffer and frames above
  the Ethernet limit.
- RTL8139 bounds the complete hardware record, including its CRC header, against
  the allocated ring plus wrap tail. Invalid records resynchronize with `CBR`
  instead of leaving `CAPR` stuck on the same malformed packet.
- Shared sections are capped at 64 MiB and checked for alignment and allocation
  arithmetic overflow before any pages are requested.
- Received IPv4, ICMP, TCP and UDP checksums are verified and fragments are
  dropped; see [`NETWORKING.md`](../net/NETWORKING.md).

### Access and process lifecycle

- `ioctl` receives the opened descriptor's granted access. Mutating GPU,
  clipboard, input, network, speaker and power operations require write access;
  a read-only descriptor can no longer invoke them.
- Init and idle processes cannot be terminated through `kill`. Unsupported
  signals, including signal 0, are rejected with `EINVAL`; a normal process may
  still terminate itself by sending a supported terminating signal.
- Orphaned descendants are reparented to init and automatically reaped when
  they exit, preventing permanent zombie-slot exhaustion.

## Findings and disposition

| Area | Finding | Severity | Disposition |
| --- | --- | --- | --- |
| ELF/VMM | Untrusted ELF could reach unchecked address arithmetic and mappings | Critical | Fixed |
| GPU import | 32-bit `pitch * height` wrap enabled OOB reads | Critical | Fixed |
| SxFS | Unchecked extent count/ranges and directory cycles | Critical | Fixed |
| CPIO | Permissive hex, names and archive termination | High | Fixed |
| NIC DMA | Device-provided frame lengths were not bounded | High | Fixed |
| User memory | No NX/W^X, stack canaries or mapping randomization | High | Partially fixed |
| Devices | Mutating ioctls required only query access | High | Fixed |
| Processes | `kill(1, ...)` and orphan zombies | High | Fixed |
| Resource use | No process quotas or PMM ownership bitmap; only ports have a user share | High | Open |
| Authorization | UID/GID, VFS modes and elevation exist; capabilities beyond the elevation rule do not | High | Partially fixed |
| ACPI | Hand-written MADT/S5 parsing needs complete bounds/checksum validation | High | Fixed: bounds enforced, checksums reported |
| PCI | No central resource manager for BAR/I/O ranges | High | Open |
| Networking | Incoming IP/TCP checksums and pending-socket tuple matching | High | Fixed |
| Kernel stacks | Canary and 32 KiB size, but no non-present guard page | Medium | Partial |
| Fault handling | No exception-table-based `copy_*_user` recovery | Medium | Open |
| CPU isolation | SMEP and SMAP are enabled when available; KPTI is not | Medium | Partial |
| Control flow | No CET shadow stacks, PIE, KASLR or compiler CFI | Medium | Open |
| SMP panic | Panic halts only the reporting core | Medium | Fixed: `smp::stop_other_cores` |
| Boot trust | Kernel, initramfs and disk image are unauthenticated | High | Open |
| VFS | POSIX `open` with no access mode skipped the permission check and granted read (E1) | High | Fixed |
| Ports | A port sent over its own pending connection leaked with its name (E2) | Medium | Fixed |
| Ports | Global 16-slot namespace with no ACL on service names (E3) | Medium | Fixed |
| VFS | Vnode slots are never reclaimed; create/unlink exhausts the table (E4) | Medium | Fixed |
| Sections | File-section cache is never invalidated by writes (E5) | Low | Fixed |
| Sections | An anonymous section may grant write and execute to separate views (E6) | Low | Accepted |
| ELF | ET_DYN wrap check uses the unbiased `p_vaddr` (E7) | Low | Fixed |

## Second-pass audit disposition

The second review of `be3622c` confirmed the following outcomes. The A3 policy
was checked against the current executable corpus and kept as a deliberate
loader restriction; it is not a reproduced runtime failure.

| ID | Disposition |
| --- | --- |
| A1 | Fixed: RTL8139 bounds CRC-inclusive records, uses the wrap tail and resynchronizes invalid records with `CBR`. |
| A2 | Fixed: the keyboard-layout popup opens `/dev/input0` read/write and reports setter failures. |
| A3 | Policy retained then, since relaxed for shared pages only (see ELF validation): 1,065 ELF artifacts of the time had no shared `PT_LOAD` pages, zero `p_memsz`, RWX or low vaddr cases. |
| A4 | Fixed: every application processor establishes its own NX baseline. |
| A5 | Fixed: the kernel canary hook uses the compiler builtin return address. |
| B1 | Fixed: kernel boot and each new process receive a nonconstant canary; `fork` inherits it. |
| B2 | Fixed: BusyBox built and ran with the normal canary runtime. The port has since been removed. |
| B3 | Fixed: SxFS validates a global per-volume extent-claim bitmap. |
| B4 | Fixed for the current stack budget: directory recursion is capped at 32 levels. |
| B5 | Fixed: over-capacity CPIO archives leave the VFS unready. |
| B6 | Fixed: supported processors enable SMEP and `CR0.WP` per core. |
| B7 | Documented open trade-off: eight contiguous pages remain until non-contiguous stacks and guard pages exist. |
| C1 | Documentation corrected: canaries are no longer described as build-time constants. |
| C2 | NX comments corrected without changing page-table behavior. |
| C3 | Refuted as a vulnerability: signal 0 is rejected as unsupported; normal self-termination remains valid. |
| C4 | Redundancy documented and retained as defense in depth. |

The D1-D3 design decisions remain unimplemented by design.

## Third-pass audit (2026-11-04)

Scope: what entered the kernel between the second pass (`8625174`) and
`bd08141` — the native dispatcher (`native_dispatch.cpp`, `sx.cpp`), ports and
handle passing, threads and wait-on-address, executable, fixed-address and
file-backed section views, `ET_DYN` and `PT_INTERP` in the loader, the POSIX
translation in `subsystems/posix/kernel/`, credentials, the VFS permission
checks, the elevation path (`SPAWN_EX` with an identity, input claims), and the
`/dev` nodes with their ioctls. The AHCI driver, block writeback and the SxFS
journal changes were read only where they meet a system call; they were not
audited as parsers of hostile disks.

E1 and E2 were fixed right after the audit, with tests in `elevtest`,
`handletest` and `nativetest`. The other findings below are open until a commit
fixes them and marks them in the table above.

### Verified

- Every system call holds the kernel lock from entry to return
  (`CONTEXT_ENTRY` in `context.S`), so a sibling thread cannot unmap a page
  between `is_user_range_accessible` and the copy. A call that blocks
  revalidates when it completes: `write_to_process_memory` checks the range
  again before writing into the parked buffer.
- System calls enter through `int $0x80` and leave through `iretq`. There is no
  `SYSRET`, so a non-canonical return address cannot fault in ring 0.
  `create_thread` requires a mapped user entry point and a writable stack slot;
  `SET_THREAD_BASE` rejects non-canonical FS/GS bases before `wrmsr`.
- `WAIT_ADDRESS` requires a natural size and alignment and reads the word only
  in the caller's address space, under the lock.
- `SPAWN_EX` with `SXN_SPAWN_FLAG_IDENTITY` is refused unless the caller acts
  as the system. Spawn and exec check `may_execute` with the spawner's
  credentials, and a new process gets its token before its first thread exists.
- `kill` and `TERMINATE` apply POSIX's rule (caller's uid or euid against the
  target's uid) and never reach init or idle. An elevated process keeps the
  user's uid, so the user may end it, as POSIX allows; nothing else reaches into
  it.
- `INPUT_IOC_CLAIM`, `POWER_IOC_PANIC`, `SET_REALTIME` and `chown` require the
  system; `chmod` requires the owner; the sticky bit holds for unlink and
  rename.
- `MAP_VIEW_AT` requires page alignment and a free, canonical range below the
  stack guard; W^X is refused per view in both places `SECTIONS.md` names.
- `PT_INTERP` is read through the bounds-checked image reader and must be a
  terminated absolute path no longer than 512 bytes.
- Panic now stops every other core (`smp::stop_other_cores`).

### Findings

**E1 — High, fixed. `open` without an access mode read any reachable file.**
`vfs::open` builds the permission it checks from `SAVANXP_OPEN_READ` and
`SAVANXP_OPEN_WRITE`. With neither, it asks `permits(who, node, 0)`, which is
true for everyone. `open_node` then treats a handle with no mode as read-only
and grants `access_read`. A user process can therefore read a `0600` file of
the system or of another user, list a directory it has no read permission on
(`readdir` only checks the handle), and build a file section from it, as long
as the ancestors are searchable. The check was right and the grant was wrong:
to the VFS, no mode is a query open, which NT's `CreateFile` with no desired
access needs and a future SUN or Win32 file API will use. Fixed in the POSIX
layer: `open_node` refuses an open with no mode (`EINVAL`), and `vfs::open`
keeps the query semantics, documented in
[`USERS_AND_PERMISSIONS.md`](USERS_AND_PERMISSIONS.md#what-the-check-covers).
Any other subsystem must grant such a handle query access only. `elevtest`
checks the refusal on `/dev/input0` (`0600`) and on a readable file.

**E2 — Medium, fixed. A port could keep itself alive forever.** A port handle may be sent
over a pipe (`PIPE_SEND_HANDLE` rejects only `io` objects). If the process
connects to its own port and sends the port over the request pipe of that
connection, the port's pending queue holds the pipe's reader and the pipe's
handle queue holds the port. Queued handles are released only when the reader
count drops to zero, so neither ever does: after the process exits, the slot
(one of 16 system-wide) and the name stay taken until reboot. The comment on
`drain_pipe_handles` also assumes a queued object never releases an `IoObject`;
a port does, through `destroy_port`. Fixed: both `pipe_send_handle` paths
refuse ports with `EINVAL`, which also makes that comment true again. Nothing
in the tree sent ports; only sections and events travel. Allowing them again
needs cycle collection or a port that cannot outlive its creator.

**E3 — Medium, fixed. Port names were first come, first served.**
`PORT_CREATE` took any free name from any process, and the table holds 16
ports for the whole system. A user process could fill the remaining slots, or
claim `SAVANXP_WM_PORT_NAME` while `windowd` was not running; an elevated
program that then opened a window would have read input from a user-owned
server. Only `elevationd` checked who created the port it connects to
(`CONNECT_EX` on the consent port). Fixed in the kernel rather than in every
client: `object::port_name_is_reserved` names the services (`windowd`,
`windowd.consent`, `compositord`, `elevation`, `audiod`, `effects-settings`,
`cursor-settings`) and the prefix `sys.`, and `PORT_CREATE` answers `EPERM` to
a creator that does not act as the system, before it looks the name up. Ports
created by anyone else share `kMaxUserPortObjects` (8) of the 16 slots, so the
other half is always the system's. **A new system service takes a name under
`sys.`**; the bare names stay only because programs already installed on a
volume connect to them. The limit is per identity and not per process, which
is what the fix first proposed: a per-process limit is defeated by spawning.
`elevtest` checks the refusals and the limit as the user.

**E4 — Medium, fixed. The VFS node table only grew.** `add_node` appended to
a 768-slot table; `unlink` and `rmdir` marked a slot invalid and nothing reused
it, so a user process that created and deleted files in its own profile
exhausted the table for the whole system until reboot. Fixed by the storage
work that followed the audit, together with E5 as the audit asked: the table
holds 2048 slots, and `retire_node` and `detach_subtree` return a slot to a
free list (`g_free_nodes` in `vfs.cpp`) once nothing holds its node. The
table is still global: a user can fill it with files that exist, which is the
quota problem below and not this one.

**E5 — Low, fixed. Cached file sections went stale.** A file-backed section
was looked up by `(vnode, offset, length)` and filled once, so a library
rewritten in place at the same size kept serving the old pages. The key now
includes `Vnode::content_generation`, which every write, truncate and slot
reuse moves on ([`SECTIONS.md`](./SECTIONS.md)), and a section holds its
file (`vfs::retain`), so unmount counts it. One part remains and
fails closed: a cached section keeps the access mask of its first opener, so a
later opener that asks for more gets a handle whose `map_view` is refused.

**E6 — Low, accepted. W^X is per view, not per section.** An anonymous
section may be created with read, write and execute granted; one writable view
and one executable view of it give memory that is written and then run, with no
`mprotect` involved. [`SECTIONS.md`](./SECTIONS.md) records this as the design.
If no JIT appears, refusing write and execute together in an anonymous
section's grant closes it without touching file-backed images.

**E7 — Low, fixed. The `ET_DYN` wrap check used the unbiased address.** The
loader checked `image_end >= p_vaddr` where `image_end` already includes the
load bias, so a segment whose end wrapped by less than the bias passed
validation. The image could only overwrite its own pages and the copy failed
on unmapped ones, so this crossed no boundary. The check now compares the
biased start against `p_vaddr` and the end against the biased start. No test
tells the two apart from user space: the wrapped image was already refused
one step later, by the copy.

**Closed from the earlier passes (2026-11-11).** `/dev/gpu0` stays `0666`, for
queries and per-process 3D contexts, but `GPU_IOC_ACQUIRE` requires the
system: the display session went to whoever asked first, so a user process
could take the screen whenever `compositord` had released it. `elevtest`
checks the refusal. `/dev/net0` stays `0666`, but
`NET_IOC_SET_TCP_FAULT` now requires the system. Incoming IPv4, ICMP, TCP and
UDP checksums are verified, fragments are dropped, and a segment reaches a
connecting socket only when the whole tuple matches; the fallback that matched
on ports alone and then adopted the sender's address is gone. See
[`NETWORKING.md`](../net/NETWORKING.md#testing-what-only-a-bad-network-exercises).
 Thread, port, section and node tables are global with no per-process
quota.

## Fourth-pass review (2026-11-11)

Not a new line-by-line audit: a comparison of this document with the tree at
`cd58936`, 78 commits after the third pass. Each
claim below was checked in the source on that date. The "Verified" list of the
third pass was not walked again.

### What the document had wrong

- **E4 and E5 were fixed and still listed as open.** The storage and SxFS v2
  commits landed the free list and the generation key without marking them.
- **The node table is 2048 slots, not 768.**
- **The ELF section contradicted itself**: it described the shared-page union
  rule and, two sections later, said shared pages are rejected.
- **The trust boundary named `/bin` and `/rescue/bin` only.** Programs now run
  from `/system` and from SxFS volumes mounted under `/mnt`, and FAT and
  partition metadata and USB devices are hostile inputs it did not list.
- **"Protected storage mutation" was listed as needing a system check.** It
  has one: raw I/O, rescan, mount and unmount (`SXN_SYS_STORAGE_*`) all answer
  `EPERM` unless the caller acts as the system.

### Still true, checked again

- SMEP and `CR0.WP` only; no SMAP, KPTI, CET, KASLR or CFI. (SMAP since
  closed: see [User-access windows](#user-access-windows-smap).)
- Kernel stacks are eight contiguous pages with a canary and no guard page.
- The ELF loader holds at most 64 program headers; the `ET_DYN` load bias is
  the constant `kUserBase`; only the section-view window is randomized.
- Sections are capped at 64 MiB. `mmap` refuses `PROT_EXEC` (`mmaptest`).
- Kernel, userland and the external SDK build with `-fstack-protector-strong`.
- `/dev/input0` and `/dev/mouse0` are `0600`; ports do not travel over pipes
  on either dispatcher; a spawn with an identity requires the system.
- Process, thread, section and node tables have no per-process or per-user
  share. Ports are the first table that does.

### Closed in this pass

E3, E7, the received-checksum and TCP-tuple findings, the open fault
injector, the display session and the ACPI table walk. Each is marked where it was recorded above.

### ACPI table walk

The hand-written parser (`acpi.cpp` for the FADT and `\_S5_`, `ioapic.cpp`
for the MADT) followed whatever firmware wrote: the root table's length was
subtracted from without a check, every entry was dereferenced as a table, the
MADT walk trusted each entry's length, and the `_S5_` decoder advanced a
pointer past the end of the DSDT. The two files also carried a copy each of
the walk. uACPI, which runs later, was never part of this.

All of it now goes through `acpi_tables` (`include/kernel/acpi_tables.hpp`),
which has no access to the machine and so runs in the host tests:

- **Every address goes through a `Reader`** that returns nullptr for a range
  it will not read. The kernel's refuses zero and anything at or above the top
  of the boot memory map (never below 4 GiB, where legacy firmware keeps
  tables in ranges the map does not always list). That bounds what firmware
  can make the kernel dereference; it does not prove the page is mapped. The
  FADT's memory reset register passes the same bound before `reboot` writes
  to it.
- **A table is mapped twice**: its header, then its whole declared length,
  which must be at least a header and at most 16 MiB (64 KiB for the RSDT or
  XSDT, whose signature is also checked). Bad entries are skipped.
- **The XSDT is used only when the RSDP is long enough to have one**, and a
  wrong extended checksum falls back to the RSDT.
- **MADT entries** are returned only when their whole length is inside the
  table; `ioapic.cpp` also requires the length its entry type needs.
- **The FADT must reach the end of the ACPI 1.0 structure**, which covers
  every field read without a length check of its own.
- **Checksums are reported, not enforced**, except the RSDP's. Firmware that
  ships a table with a wrong checksum exists, refusing its FADT would cost
  that machine its power button and shutdown, and a checksum is not a defense:
  whoever can write the table can write the sum. The boot log says
  `acpi: XXXX has a wrong checksum, using it anyway`.

`acpi-tables-test` builds its tables in memory that ends at a page with no
access, so a read past a bad length is a crash, not a pass. Checked only in
QEMU with OVMF (the XSDT path); the RSDT path and wrong checksums are covered
by the host test alone, and no real machine has booted this yet.

### Never audited

These entered the kernel after the third pass, or were out of its scope, and
all of them parse input this document calls hostile. None has been read as an
attacker would read it, and no finding above speaks for them:

| Code | Hostile input |
| --- | --- |
| `kernel/usb/` (xHCI, hubs, HID, mass storage) | Descriptors, HID report descriptors and transfer lengths from any device plugged in |
| `kernel/fat.cpp` and the vendored FatFs | The metadata of a FAT volume on a stick (its files cannot be executed) |
| `kernel/partition.cpp` | GPT and MBR tables of any disk |
| `kernel/virtio_gpu.cpp`, the `GPU_IOC_3D_*` ioctls | virgl command streams from any process, since `/dev/gpu0` is `0666`; the host renderer is the parser |
| `kernel/net.cpp`, native sockets (`SXN_SYS_SOCKET_*`) | Reviewed here only for checksums and tuple matching |
| `kernel/azalia/` | Codec responses; read against the OpenBSD source on 2026-11-11 for fidelity, not for hostile hardware |
| `kernel/sxfs.cpp` after v2 | Link counts and copy-on-write directories read from a hostile disk |
| `kernel/ahci.cpp`, block writeback | Carried over from the third pass, which read them only at the system-call edge |

## Required follow-up work

The third-pass findings are all closed or accepted. What comes first now is
the list in [Fourth-pass review](#fourth-pass-review-2026-11-11): a first
audit of the drivers that parse
hostile input and have never had one.

The next security phase should not add more ad-hoc device checks. The process
security context it called for now exists in part: every process carries a
uid, euid and gid ([`USERS_AND_PERMISSIONS.md`](USERS_AND_PERMISSIONS.md)).
What remains:

1. Capability bitmaps, or at least the elevation rule, beyond file modes.
2. Done: the display session, network fault injection and storage mutation
   (`SXN_SYS_STORAGE_*`) require the system. Shutdown stays unelevated by
   design.
3. PID 1 as a real subreaper with a non-blocking reap loop.
4. Per-process memory, object, handle and I/O quotas.
5. `FD_CLOEXEC` and references for blocked-wait objects.

Memory safety should then proceed in this order:

1. a checked-arithmetic and user-mapping API shared by ELF, GPU, sections and
   filesystem code;
2. PMM ownership metadata and double-free detection;
3. a page-table VMM selftest and malformed-ELF corpus;
4. user stack ASLR, followed by PIE/ET_DYN and a load-bias implementation;
   the second half is now partly done — `elf.cpp` accepts `ET_DYN` and relocates
   it onto a base — but the base is still a constant, and no userland image is
   linked as PIE yet;
5. exception-table user-copy helpers (SMAP is on without them: every copy is
   validated first, see [User-access windows](#user-access-windows-smap));
6. per-stack non-present guard pages (the SMP-wide panic stop is done).
7. for the shared-library loader: `mprotect` with per-page copy-on-write, and
   RELRO. Dropping write on a view another process still holds writable would
   change that process's mapping without its consent, so the first `mprotect`
   cannot be a plain flag rewrite. `BIND_NOW` does not need either; lazy PLT
   binding does. See [`SECTIONS.md`](./SECTIONS.md).

Firmware and hardware inputs need their own parser tests: fuzz CPIO, ACPI/MADT,
SxFS, FAT, partition tables, USB descriptors and virtio rings; validate every PCI BAR against physical memory, reserved
regions and already assigned windows; and add finite waits to legacy I/O
paths such as UART and AC'97.

## Verification

At minimum, a security change is accepted only after:

```bash
./build.sh build
./build.sh smoke smoke --smp 4
```

The loader/build flow must also preserve external applications and their data:

```bash
./ports/doomgeneric/build.sh --wad ports/doomgeneric/wad/doom1.wad
./build.sh build
```

The smoke serial log must show `/bin/doomgeneric` and the persistent Doom
WAD still exist. Changes to NX, page tables or SxFS are incomplete until this
regression is run.
