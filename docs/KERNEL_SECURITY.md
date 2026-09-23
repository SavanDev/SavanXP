# Kernel security model and hardening

Status: audited on 2026-09-23.

This document defines SavanXP's current kernel security boundary, records the
findings from the kernel audit, and distinguishes mitigations already in the
tree from risks that still require architectural work. It is not a claim that
SavanXP is a hardened multi-user operating system.

## Trust boundary

The kernel treats the following inputs as hostile unless a stronger statement
is made below:

- ELF files executed from `/bin` or `/disk/bin`;
- pointers, lengths and flags supplied through system calls;
- SxFS metadata and directory trees read from disk;
- the initramfs CPIO archive supplied by the boot configuration;
- packet lengths and used-ring entries reported by NIC hardware;
- ACPI, PCI and MMIO descriptions supplied by firmware or virtual hardware;
- resource exhaustion requests from otherwise valid processes.

The boot chain itself is not authenticated. Limine, the kernel, initramfs and
disk image are trusted at this boundary. Until verified boot or UEFI Secure Boot
is integrated, physical or boot-media modification is outside the mitigation
set below.

SavanXP also has no users, UIDs, credentials or general capability model yet.
Processes run with the same authority. Protecting init and classifying device
I/O access reduce accidental or confused-deputy behavior, but do not create
security isolation between applications.

## Existing structural defenses

The audit confirmed the following defenses were already present:

- each process has a separate address space and shares only the high half of
  the kernel page tables;
- user-copy helpers walk all four paging levels and require user permissions;
- user stack growth is bounded by a reserved region and a guard page;
- the system call gate accepts ring 3 only and the entry stubs clear `DF`;
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
- SavanXP now requires an NX-capable CPU and enables `EFER.NXE` during early
  CPU initialization. The static kernel image inherited from Limine is not yet
  repartitioned into executable and NX mappings, so this phase enforces W^X for
  user pages and NX for dynamically mapped kernel pages, not yet for all kernel
  data.
- ELF `PT_LOAD` segments marked both writable and executable are rejected.
  Executable segments are mapped without `PF_W`; non-executable segments,
  BSS, stacks and section views are mapped with NX.
- User stack and shared-section pages preserve the NX bit across fork.
- Kernel stacks grow from 16 KiB to 32 KiB. Canaries detect corruption before a
  normal return, although a dedicated non-present guard page is not implemented
  yet.

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
unbounded kernel stack use.

### Compiler hardening

- Kernel, uACPI, in-tree POSIX userland, external SDK applications, FFmpeg and
  the native SDK are compiled with `-fstack-protector-strong`. BusyBox is the
  explicit compatibility exception: its current port still needs the legacy
  no-canary build to boot, so it remains a tracked gap rather than silently
  claiming coverage.
- `crt0` installs a fresh nonzero canary from a kernel register before calling
  `main`. The value is not stored in the executable image; `fork` inherits it
  with the parent's context and address space, while `exec` receives a new one.
- `_start` seeds the kernel canary before the first protected frame and the
  kernel keeps it stable for the rest of boot; a detected failure panics.
- The native and POSIX runtimes supply the compiler runtime symbols required by
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
  non-overlapping, covered by the allocation bitmap and large enough for the
  inode size. Inode IDs, allocation bitmaps and inode types must agree.
- A journal containing invalid metadata is rejected before any of it is written
  to the home metadata area.
- Directory entries require valid terminated names, valid IDs, matching file or
  directory types and matching inode types.
- Recursive directory reconstruction carries a visited bitmap and a depth cap,
  preventing self-references and ancestor cycles from overflowing the kernel
  stack.
- The superblock's total sector count is checked against the real block device.
- The initramfs parser now requires strict hexadecimal fields, exactly one NUL
  at the end of each name, supported regular/directory file types, bounded
  lengths, checked padding, and a `TRAILER!!!` record. A truncated or malformed
  archive leaves the VFS unready instead of mounting partial data.

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

### Access and process lifecycle

- `ioctl` receives the opened descriptor's granted access. Mutating GPU,
  clipboard, input, network, speaker and power operations require write access;
  a read-only descriptor can no longer invoke them.
- Init and idle processes cannot be terminated through `kill`.
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
| Resource use | No process quotas or PMM ownership bitmap | High | Open |
| Authorization | No UID/GID/capabilities; power and diagnostics remain broadly available | High | Open |
| ACPI | Hand-written MADT/S5 parsing needs complete bounds/checksum validation | High | Open |
| PCI | No central resource manager for BAR/I/O ranges | High | Open |
| Networking | Incoming IP/TCP checksums and pending-socket tuple matching | High | Open |
| Kernel stacks | Canary and 32 KiB size, but no non-present guard page | Medium | Partial |
| Fault handling | No exception-table-based `copy_*_user` recovery | Medium | Open |
| CPU isolation | SMEP/SMAP/KPTI are not enabled | Medium | Open |
| Control flow | No CET shadow stacks, PIE, KASLR or compiler CFI | Medium | Open |
| SMP panic | Panic halts only the reporting core | Medium | Open |
| Boot trust | Kernel, initramfs and disk image are unauthenticated | High | Open |

## Required follow-up work

The next security phase should not add more ad-hoc device checks. It should add
a small process security context first:

1. UIDs/GIDs plus inheritable and permitted capability bitmaps.
2. A capability check for power, network fault injection, process inspection and
   protected storage mutation.
3. PID 1 as a real subreaper with a non-blocking reap loop.
4. Per-process memory, object, handle and I/O quotas.
5. `FD_CLOEXEC` and references for blocked-wait objects.

Memory safety should then proceed in this order:

1. a checked-arithmetic and user-mapping API shared by ELF, GPU, sections and
   filesystem code;
2. PMM ownership metadata and double-free detection;
3. a page-table VMM selftest and malformed-ELF corpus;
4. user stack ASLR, followed by PIE/ET_DYN and a load-bias implementation;
5. exception-table user-copy helpers before enabling SMAP;
6. per-stack non-present guard pages and SMP-wide panic stop.

Firmware and hardware inputs need their own parser tests: fuzz CPIO, ACPI/MADT,
SxFS and virtio rings; validate every PCI BAR against physical memory, reserved
regions and already assigned windows; and add finite waits to legacy I/O
paths such as UART and AC'97.

## Verification

At minimum, a security change is accepted only after:

```powershell
.\build.ps1 build
.\build.ps1 smoke -Smp 4
```

The loader/build flow must also preserve external applications and their data:

```powershell
.\sdk\doomgeneric\build.ps1
.\build.ps1 build
```

The smoke serial log must show `/disk/bin/doomgeneric` and the persistent Doom
WAD still exist. Changes to NX, page tables or SxFS are incomplete until this
regression is run.
