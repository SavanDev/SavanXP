# Linux subsystem — binary compatibility on Sx

> **Status: planned, not started (2026-11-08).** This is an implementation
> plan, not a claim that Linux binaries run today. The initial target is
> **x86-64 Linux ELF, starting with static executables**; the first application
> goal is **Xeyes**, displayed on the SavanXP desktop. The subsystem's installed
> runtime lives under **`/system/linux`**. Recheck the baseline below before
> implementation; no upstream dependency is pinned by this plan.

## Objective and order

Run programs built for Linux without changing their sources for SavanXP,
relinking them against the SavanXP SDK, stamping their ELF headers or rewriting
their syscall instructions. Building a test executable with a normal Linux
toolchain is allowed; the resulting file must also run on Linux unchanged.

The order within this roadmap is **shared Sx groundwork, then a bounded static
Linux milestone**. Linux is a useful foreign ABI to start with because ELF
loading, files, pipes and process mechanisms already exist. It and
[Win32](WIN32_SUBSYSTEM_ROADMAP.md) do not depend on each other: Win32's path
to its first application runs in a user-mode host and consumes none of the
slices below until its later profiles. Neither subsystem depends on finishing
audio or Azalia.

The first application target is **Xeyes (`xeyes`)**, running as an unmodified
Linux x86-64 executable, with eyes that follow the pointer on the SavanXP
desktop. Its actual Linux libc, X11 client libraries and display connection
are part of the target's dependency inventory. Drawing a native imitation or
recompiling it against SUN/POSIX does not satisfy this goal.

L4 remains an intermediate checkpoint with a documented set of static tools.
The small executables and tools before Xeyes are
engineering fixtures; they do not close the application goal. Dynamic loading
and X11 integration lead to Xeyes in L7. A general shell, distribution or
complete X11 desktop is not a prerequisite; take only the process/signal
features in L5 that the target actually needs.

## Baseline to recheck

| Available in SavanXP | Difference from Linux that remains |
| --- | --- |
| ELF64 `ET_EXEC`/`ET_DYN` loader in `kernel/elf.cpp` | Startup stack, environment, auxiliary vector and interpreter handoff use SavanXP conventions |
| Native and POSIX subsystem dispatch and private state | No Linux subsystem id, dispatcher or Linux launch selection exists |
| `int 0x80` entry in `arch/x86_64/context.S` | Linux x86-64 binaries normally use the CPU's `syscall` instruction |
| Files, directories, pipes, fork/exec and credentials | Linux syscall numbers, flags, structures and observable semantics are a separate contract |
| Threads, per-thread FS/GS bases and address waits | Linux `clone`, TLS setup, futex and thread-exit semantics still need translation |
| Sections, views, NX and SMP TLB coherence | General region reservation, commit/decommit, protection changes and queries are missing |
| User faults terminate the process after stack-growth handling | No general user exception delivery or signal frame/return exists |

The POSIX SDK is not a Linux ABI. For example, syscall 4 is `READDIR` in
`subsystems/posix/sdk/v1/include/savanxp/syscall.h` and `stat` in Linux x86-64.
The current POSIX `mmap` wrapper accepts a limited anonymous mapping subset;
its `sigaction` stores handlers without delivering kernel signals. Reusing
those entry points unchanged would not provide Linux semantics.

## Architecture and boundaries

- Follow [SYSTEM_LAYERING.md](../../userland/SYSTEM_LAYERING.md): Linux stands
  on Sx beside POSIX and SUN. Do not implement it by forwarding Linux numbers
  to the POSIX dispatcher or including POSIX SDK types in kernel core code.
- Reuse neutral VFS, process, object and VM mechanisms. Extract a missing
  neutral operation from the existing translation layer when needed, with
  its existing consumers preserved and tested.
- Keep Linux numbers, layouts, flag/error translation and private state in a
  proposed `subsystems/linux/` tree. Register process/thread state through
  `subsystem::`; the kernel must not interpret Linux signal or TLS structures.
- New SavanXP launchers and services are SUN programs. Foreign executables
  used as compatibility fixtures remain Linux binaries, not SUN applications.
  A SUN helper that needs a missing API supplies its consumer and `nativetest`
  assertion under [SUN.md](../../userland/SUN.md), rule 1.
- Preserve SUN and POSIX launch behavior. `EI_OSABI` alone cannot reliably
  distinguish Linux ELF from SavanXP's existing POSIX ELF. Start with an
  explicit launch ABI request, possibly exposed by a future SUN launcher;
  do not rewrite guest headers or guess from a file extension. That launch
  interface does not exist yet.

## Runtime layout

The installed Linux subsystem lives in **`/system/linux`**, alongside the
other subsystem runtimes under `/system`, as specified in
[SYSTEM_LAYERING.md](../../userland/SYSTEM_LAYERING.md#planned-subsystem-runtime-layout).
This is a path inside the running OS; the proposed source tree remains
`subsystems/linux/`.

Place foreign executables and their Linux runtime files beneath that root,
using `bin/`, `lib/`, `lib64/`, `usr/` and `etc/` as required by the pinned
profile. Define how guest paths such as `/usr/bin/xeyes` or the ELF interpreter
path resolve into this tree; `PT_INTERP` must not accidentally load a native
SavanXP library. Define access to native files and special paths such as
`/dev`, `/proc` and `/tmp` explicitly, with normal SavanXP credentials. The
directory prefix is an installation boundary, not a sandbox by itself.

Launch selection, library/configuration lookup and build/install manifests
must use this root consistently. Preserve externally installed executables,
libraries and assets beneath it across normal builds and runtime updates;
sync owned files rather than recreating the tree or the persistent image.

## Implementation reference: FreeBSD Linuxulator

Use **FreeBSD's Linuxulator as the primary implementation reference** for
the x86-64 subsystem. Its maintained Linux compatibility layer supports
unmodified 64-bit Linux binaries, matching this roadmap's architecture more
closely than OpenBSD's historical implementation. The
[FreeBSD Handbook](https://docs.freebsd.org/en/books/handbook/linuxemu/)
describes the supported architectures, static binaries and separate Linux
userland installation.

The intended path is **Linux ELF / syscall ABI → Linux translation subsystem
→ Sx → kernel mechanisms**. Linux remains the behavioral authority; FreeBSD
provides examples of implementing that contract on another kernel. A
Linuxulator limitation or FreeBSD-specific behavior is not automatically the
behavior to reproduce in SavanXP.

Study these parts first, pinning their upstream revision during L0:

| FreeBSD source | Application to SavanXP |
| --- | --- |
| [`sys/amd64/linux/linux_sysvec.c`](https://github.com/freebsd/freebsd-src/blob/main/sys/amd64/linux/linux_sysvec.c) | Register/argument handling, syscall results, initial registers and signal context; inform L1/L2 and later L5 |
| [`sys/amd64/linux/syscalls.master`](https://github.com/freebsd/freebsd-src/blob/main/sys/amd64/linux/syscalls.master) | Declarative dispatch/argument inventory; consider generating Linux-owned declarations, checked against the Linux syscall table |
| [`sys/compat/linux/linux_file.c`](https://github.com/freebsd/freebsd-src/blob/main/sys/compat/linux/linux_file.c) | Flag, descriptor and error translation around host file operations; inform L3/L4 over neutral Sx operations |
| [`sys/compat/linux/`](https://github.com/freebsd/freebsd-src/tree/main/sys/compat/linux) | Later process, VM, signal and synchronization translations; inspect only what the selected fixture requires |

These are references, not a ready-made subsystem to compile against Sx.
FreeBSD's code depends on its process/thread structures, VFS, VM and locking.
For example, its file translation calls `kern_openat`, and its amd64 argument
fetching consumes a trap frame already rearranged by FreeBSD's syscall entry.
Implement the corresponding Sx operation and SavanXP entry contract explicitly;
do not import a BSD kernel interface layer merely to satisfy those dependencies.
Keep Linux descriptors, flags, errors and signal state in the Linux subsystem.
Missing neutral mechanisms extend Sx with their native consumer and assertions
under the shared groundwork rules below.

[NetBSD's Linux compatibility](https://www.netbsd.org/docs/compat.html) is a
complementary reference for alternative amd64 implementations. OpenBSD is a
historical design reference: its
[`compat_linux`](https://man.openbsd.org/OpenBSD-5.9/compat_linux) supported
i386, and Linux emulation was
[removed in OpenBSD 6.0](https://www.openbsd.org/60.html). Neither replaces
Linux reference runs and ABI tests.

## Shared Sx groundwork

This section is the shared inventory of kernel work both roadmaps name. Linux
needs it from L2 on; Win32 defers all three slices past its first application
([Win32 roadmap](WIN32_SUBSYSTEM_ROADMAP.md#deferred-s1-s2-and-s3)). These
are kernel/platform slices, not Linux features. Introduce each missing
capability with an actual SavanXP consumer and `nativetest` assertions. The
native heap using sparse arenas and a SUN fault-reporting utility are possible
consumers; select and document the real consumer before implementing a slice.
Do not require every slice before the libc-free executable in L2 can run.

| Slice | Scope | Acceptance |
| --- | --- | --- |
| S1 — launch contract | Separate image recognition, launch ABI and startup preparation; carry the selected subsystem through spawn/exec and handle inheritance | Native/POSIX behavior is preserved; explicit foreign selection is checked; failed loading leaks no process, handle or mapping and leaves the old exec image usable |
| S2 — virtual memory | Track reserved ranges separately from backing; support commit/decommit, release, protection and queries, fixed mappings and collision checks | Native consumer and assertions cover zeroing, overlap/overflow, inaccessible reserved/decommitted pages, protection changes, rollback and SMP TLB coherence |
| S3 — user exceptions | Deliver fault information and saved registers to a user handler and restore a validated user context | Native consumer handles a fault/breakpoint; invalid stacks/contexts cannot return into kernel space; unhandled/recursive faults terminate predictably; sibling threads and other processes survive |

S1 must define child/exec ABI selection, standard handles, environment and
cleanup across subsystem changes. Never carry opaque state into a different
subsystem merely because both registered the same byte size. Keep Linux runtime
files under `/system/linux` and Win32 runtime files under `/system/win`, with
their respective lookup policies; do not overwrite SUN/POSIX libraries or
interpret a guest root as a replacement for access checks.

S2 must address partial ranges and mapping lifetime, not merely increase
`kMaxSectionViews`. Keep the existing permission policy explicit: foreign RWX
requests do not silently relax the platform's W^X policy. Record unsupported
cases until a separate policy decision and its tests exist. S3 supplies the
mechanism; Linux signal numbers, masks, frames and `rt_sigreturn`, and Windows
SEH/unwind rules, belong to their respective subsystems.

## Phases and acceptance

All phases below are pending. Each completion record must name the tested
binary, build configuration, hash, reference environment and retained results.

### L0 — freeze the first compatibility profile

Choose the first Linux ABI profile as x86-64 static non-PIE `ET_EXEC`, without an
interpreter, and pin its toolchain and fixtures. State that i386, x32, dynamic
ELF, desktop applications, containers and distribution boot are outside this
profile. Inventory the fixture's syscalls on Linux and record required layouts
from Linux UAPI. Define resource limits, a separate guest runtime layout and
diagnostic output. No compatibility profile requires replacing the system's
init or desktop.

Also identify the Xeyes application profile: pin its Linux executable/package,
architecture, libc/interpreter, transitive shared libraries, resource/config
files and launch options. Record hashes and reference pointer/repaint behavior
on Linux. Inventory its X11 requests/extensions and transport requirements;
these later dependencies do not need implementation before L1-L4. Dynamic ELF
and X11 are outside the first static profile, but inside the Xeyes goal.

Pin the FreeBSD reference revision and record a per-operation map from its
translation to an existing or missing Sx primitive. Record which references
were studied and audit each candidate's license before copying code; choosing
Linuxulator as a reference does not authorize importing the whole component.

**Done when:** the fixtures run on a recorded Linux reference, their hashes
and expected stdout/stderr/exit results are retained, and a dependency matrix
identifies which Sx primitives each one consumes. The record distinguishes
implemented, unsupported and deliberately deferred behavior.

### L1 — Linux syscall entry and dispatch

Implement the actual x86-64 `syscall` entry: per-CPU MSR setup, safe kernel
stack selection, user RSP preservation, interrupt/flag handling and a validated
return path. Unlike an interrupt gate, `syscall` does not switch to a kernel
stack automatically. Preserve the current `int 0x80` ABI and FS/GS ownership;
do not assume GS is available for kernel CPU state. Linux arguments use RAX
for the number and RDI, RSI, RDX, R10, R8, R9 for arguments.

Add a Linux dispatcher with explicit translation and `-ENOSYS` for unknown
calls. Do not reserve an arbitrary part of the Linux number space for native
calls; any bridge must be deliberate and unambiguous.

**Done when:** a controlled foreign-ABI probe exercises entry/return and six
arguments on 1, 2 and 4 vCPUs, including scheduling, register preservation and
invalid return state. Native/POSIX `int 0x80` callers still pass. No Linux
number can accidentally invoke an unrelated SavanXP operation.

### L2 — the first unmodified Linux executable

Use S1 to launch the L0 ELF explicitly as Linux. Build the Linux initial stack
with `argc`, `argv`, `envp` and a valid, explicitly supported `auxv`; preserve
stack alignment and correct ELF/program-header information. Reject unsupported
interpreters and image variants rather than using SavanXP's runtime startup.
Implement `write` and `exit` for the first libc-free fixture, with explicit
standard handles and observable child termination.

**Done when:** the same ELF hash runs on Linux and SavanXP, prints the exact
expected bytes and returns the expected exit code. Bad headers, pointers,
truncation, unsupported architecture and missing handles fail without a kernel
panic or resource leak. Neither a source port nor a patched instruction counts.

### L3 — static musl programs and essential file I/O

Pin a musl build and run a normal static C executable, still non-PIE initially.
Implement its traced requirements: environment/auxv, TLS and `arch_prctl`,
memory allocation, clocks and file operations such as `openat`, `read`,
`writev`, seek, stat and close. This is an inventory to measure, not a promise
that every listed call is required or sufficient. Expose remaining neutral
operations through Sx instead of depending on POSIX translation internals.

Consume S2 as real allocation/protection needs appear. Translate Linux layouts,
open flags, descriptor flags and errors explicitly. Unsupported operations
return their documented failure; successful no-op stubs do not close a gate.

**Done when:** pinned static musl fixtures check arguments/environment, TLS,
heap use and file create/read/seek/stat/close against Linux, including short
I/O and errors. File results survive a guest restart; existing SUN/POSIX
credentials and access checks still apply.

### L4 — a useful bounded tool set

Pin a static BusyBox configuration, or an equally small external tool set,
with explicitly selected commands such as `echo`, `cat`, `ls` and `pwd`.
Keep the Linux sources unmodified for SavanXP; configuration choices may
reduce scope. List exact supported invocations and required syscalls. Add
directory, cwd and other operations only as those commands require them.

**Done when:** those commands run unchanged on deterministic directory/file
fixtures and match Linux results after documented normalization of unstable
fields. Bad paths and access denial are covered. The executables and their
data remain installed after `./build.sh build`.

**This closes the static checkpoint.** The Xeyes application goal remains open
through L7. Reassess priorities here against the independent
[Win32 phases](WIN32_SUBSYSTEM_ROADMAP.md#phases).

### L5 — signals, process creation and a small shell

Use S3 for real Linux signal delivery: masks, pending state, user frames,
`rt_sigaction`, return validation and interruption/restart behavior. Implement
the measured process-creation subset (`fork`/`clone` flags as needed), `execve`,
wait status, descriptor inheritance, pipes and `exit` versus `exit_group`.
Add Linux futex/thread teardown semantics with a multithreaded consumer before
claiming pthread compatibility; native address waits alone do not supply it.

**Done when:** a pinned static shell runs a script that launches children,
pipes data, redirects I/O, observes statuses and handles a signal. Thread
fixtures verify TLS, join/wake and process cleanup on several vCPUs. Unsupported
clone/futex modes fail explicitly. Interactive job control needs its own gate.

### L6 — dynamic binaries and later consumers

Load the guest's `PT_INTERP` and hand off using the Linux entry contract,
including accurate auxiliary-vector addresses for the executable and loader.
Start with pinned musl and later evaluate glibc as a separate compatibility
profile, driven by the libc used by the selected Xeyes binary. SavanXP's
existing dynamic linker is not a replacement for a Linux interpreter, even
if its path looks like a Linux loader path. Resolve guest
runtime files under `/system/linux` rather than replacing the native runtime.
Add file-backed mappings, relocation/TLS needs and interfaces such
as `/proc`, epoll or sockets only with named consumers and tests.

**Done when:** one pinned dynamic application and its recorded library set run
unchanged with correct results, including missing-interpreter/library failures.
This does not certify arbitrary glibc binaries, a distribution, X11/Wayland,
systemd, namespaces, containers or Linux drivers. Running Wine's Linux build
is a separate future assessment, not this roadmap's acceptance condition.

### L7 — Xeyes on the SavanXP desktop

Run the pinned Linux Xeyes binary with its actual client-library closure,
including the X11/Xt/Xext/Xmu and other dependencies found at L0. Implement
the measured socket/connection, I/O multiplexing, timer and file operations
through the Linux-to-Sx boundary. Retain the unchanged ELF and Linux libraries
under `/system/linux`; use normal launch options rather than replacing their
code with a SavanXP-specific graphics library.

Provide a local X11 protocol endpoint integrated with `windowd`, composition
and input. New native display services are SUN programs, with missing Sx/SUN
capabilities supplied under the usual consumer/assertion rule. Freeze the
server/bridge approach before implementing it, including any upstream reuse.
A remote host X server can help diagnose the Linux client, but the application
gate requires rendering and interaction inside SavanXP's own desktop.

Implement the requests needed for connection/setup, resources, windows,
graphics contexts, drawing, pointer queries/events, repaint and close. Record
the target's extension requirements: SHAPE, RENDER and newer features depend
on the selected version/options. A documented bounded profile may disable
optional extensions using upstream-supported launch/build options; test that
same profile on Linux. Unimplemented extensions must not advertise support.

**Done when:** the same pinned Linux executable opens a visible Xeyes window
on SavanXP, draws both eyes, and changes pupil position correctly for several
known pointer positions. Cover motion outside the client area where required
by the reference, expose/occlusion, moving/resizing the window, closing and
relaunching without leaked processes, connections or surfaces. Retain input
sequences, logs and visual assertions; a startup log or static screenshot is
insufficient. Native/POSIX desktop behavior still passes, and the executable,
its libraries and resources survive a normal build and guest restart.

## Verification and integration rules

- Linux is the behavioral reference. Use `strace` on the host and optionally
  [sogen](https://github.com/momo5502/sogen) to inspect supported cases. An
  emulator's result is secondary evidence, not proof of Linux behavior.
- Keep exact fixture hashes, toolchain/configuration pins, syscall inventory,
  reference kernel/libc versions and logs. Normalize addresses, PIDs and
  timing only where appropriate; preserve statuses, byte counts and ordering.
- Start with small ABI tests, then selected relevant cases from the
  [Linux Test Project](https://github.com/linux-test-project/ltp). Tests may
  require much more infrastructure than the operation they assert. Track
  pass, fail, skip, timeout and unsupported setup separately.
- Add proposed guest smoke scenarios through the existing catalog when the
  implementation lands; no Linux build target or smoke command exists yet.
  Test actual rebuilt/extracted image binaries, not stale payloads.
- Kernel/loader/build changes require appropriate host tests, `native-test`,
  POSIX/thread regressions and `./build.sh smoke sxgui-smoke` for loader or
  drawing changes. Follow [AGENTS.md](../../../AGENTS.md) for the Doom
  persistence check, including both `/bin/doomgeneric` and its WAD under
  `/games`, plus the installed Linux payload under `/system/linux`; never
  recreate a valid `build/disk.img` as a normal build step.
- Discover any new host tool in CMake and document dependencies in
  [BUILD_CMAKE.md](../../build/BUILD_CMAKE.md). Keep optional foreign toolchains
  out of the normal build until explicitly required. Behavior changes get an
  `[Unreleased]` changelog entry in their implementation commit.

## Source policy and references

Follow [THIRD_PARTY_ADOPTION.md](../../third-party/THIRD_PARTY_ADOPTION.md).
Pin and inspect licenses before adopting musl, BusyBox, LTP, headers or sogen
code; preserve notices and record every distributed component in
[THIRD_PARTY_PROVENANCE.md](../../third-party/THIRD_PARTY_PROVENANCE.md).
External host tools need not become kernel dependencies. In particular, using
sogen for traces does not require copying its GPL code into the kernel. This
plan introduces no vendored source, guest binaries or proprietary assets.

Linuxulator is `Reference` by default. A small, self-contained conversion may
be a `Selective port` after its exact revision, file-level license, notices
and Sx dependencies are reviewed and recorded in the provenance registry.
Do not assume a single license covers the FreeBSD, NetBSD or historical
OpenBSD trees. Record reference-derived implementation provenance when the
implementation lands, even if no upstream code is copied.

Primary references for implementation, to pin at L0:

- [Linux x86-64 syscall table](https://github.com/torvalds/linux/blob/master/arch/x86/entry/syscalls/syscall_64.tbl)
- [Linux x86 UAPI](https://github.com/torvalds/linux/tree/master/arch/x86/include/uapi/asm)
- [musl syscall entry](https://git.musl-libc.org/cgit/musl/tree/arch/x86_64/syscall_arch.h)
  and [libc startup](https://git.musl-libc.org/cgit/musl/tree/src/env/__libc_start_main.c)
- [musl interpreter startup](https://git.musl-libc.org/cgit/musl/tree/ldso/dlstart.c)
- [Xeyes upstream](https://gitlab.freedesktop.org/xorg/app/xeyes) and
  [X.Org module descriptions](https://www.x.org/ModuleDescriptions/) for the
  X11 client/server components; pin the actual source/package and dependencies
  at L0 rather than assuming every release has the same extension needs
- [Thread groundwork](../done/THREADS_ROADMAP.md),
  [Sx API](../../kernel/KERNEL_NATIVE_API.md) and
  [sections/views](../../kernel/SECTIONS.md)
