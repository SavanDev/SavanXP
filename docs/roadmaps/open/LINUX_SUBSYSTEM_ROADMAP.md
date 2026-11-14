# SSL, the Savan Subsystem for Linux — binary compatibility on Sx

> **Status: L0, E1 and L1 closed (2026-11-14): a static, libc-free Linux
> executable runs unchanged.** What is built is described in
> [LINUX.md](../../userland/LINUX.md). The subsystem is
> named **SSL (Savan Subsystem for Linux)**, as the Win32 one is SSW. Its
> measured first profile is
> [LINUX_XEYES_PROFILE.md](../../userland/LINUX_XEYES_PROFILE.md). The first
> application goal is unchanged: an unmodified Linux x86-64 **Xeyes** whose
> eyes follow the pointer on the SavanXP desktop. The design was settled on
> 2026-11-10, replacing the plan of 2026-11-08. It keeps that plan's direction
> (a translation layer in the kernel, over Sx), makes the decisions it left
> open, shrinks the first kernel step, and orders the phases by what Xeyes
> needs. `subsystems/linux/` holds the L0 fixtures and profile tooling and
> the kernel half's entry points, and the installed runtime root
> `/system/linux` is not created by any build.

## Objective

Run programs built for Linux without changing their sources for SavanXP,
relinking them against the SavanXP SDK, stamping their ELF headers or
rewriting their system call instructions. Building a test executable with a
normal Linux toolchain is allowed; the same file must run on Linux unchanged.

The first application is **Xeyes (`xeyes`)** as a Linux x86-64 executable
with its real libc and X11 client libraries, displayed in a window of the
SavanXP desktop. A native imitation, or Xeyes recompiled against SUN or
POSIX, does not satisfy the goal.

Static executables and a small tool set are the engineering gates on the
way. A shell, a distribution, a complete X11 desktop, containers and Linux
drivers are not part of it; each later consumer is its own named profile
([L7](#l7--later-profiles)).

## The decision

```text
  xeyes, its libc and X11 libraries     unmodified Linux ELF files, under
                                        /system/linux
  ---------------------------- the `syscall` instruction -------------------
  subsystems/linux/kernel/              Linux numbers, layouts, flags, errors
                                        and descriptors, translated onto the
                                        kernel's neutral mechanisms
  kernel                                VFS, processes, threads, memory
                                        regions, pipes, named ports
  ---------------------------- a named port, two pipes per connection ------
  x11d                                  SUN program: a small X11 server, one
                                        desktop window per top-level X window
  windowd, compositord                  the existing desktop
```

1. **The translation layer is a kernel half of a new subsystem**, with its
   own `subsystem::Id`, dispatcher and private per-process state, like POSIX
   has. A system call ABI can only be received by the kernel, and process
   creation, descriptors and memory are kernel objects.
2. **The first entry path is the trap that already exists.** With
   `EFER.SCE` clear, which is how the system runs today, the `syscall`
   instruction raises an invalid-opcode exception through the interrupt
   table. The handler recognizes it for a Linux process and dispatches. A
   Linux call then costs what a native `int 0x80` costs. The MSR-based fast
   entry is a later optimization with a measured consumer.
3. **A program is run as Linux because it was asked for**, through a SUN
   launcher, `linux`. `EI_OSABI` cannot tell a Linux image from a SavanXP
   POSIX one, and nothing is guessed from a path or a header.
4. **Linux memory semantics get a real mechanism: page-granular memory
   regions in the kernel.** Section views cannot carry them. This is the
   largest kernel piece and it arrives in two steps, each with its consumer.
5. **The libc of the first profiles is musl**, from the static fixtures to
   the Xeyes binary. glibc is a later profile with its own inventory.
6. **X11 is served by a program written here**, `x11d`: core protocol, no
   extension advertised until one is implemented. A Linux client reaches it
   through an `AF_UNIX` stream socket that the subsystem carries over a native
   named port.
7. **The scope is measured.** A system call or an X request is implemented
   when a pinned fixture issues it. An unimplemented call fails with
   `-ENOSYS` and one log line; it never returns success without doing the
   work.

### It is a subsystem with a kernel half

This is the model [SYSTEM_LAYERING.md](../../userland/SYSTEM_LAYERING.md#planned-binary-compatibility-subsystems)
declares, in the form POSIX already uses: a dispatcher registered under an id,
state the kernel allocates without reading, and translation code in
`subsystems/linux/kernel/` that calls the neutral kernel and never the POSIX
dispatcher. [Win32](WIN32_SUBSYSTEM_ROADMAP.md#it-is-a-subsystem-and-it-does-not-live-inside-sun)
is the opposite case and has no kernel half, because its contract is a set of
libraries. The two do not depend on each other.

The programs written here for the subsystem, `linux` and `x11d`, are SUN
programs. Whatever they lack in SUN arrives as a slice with its `nativetest`
assertion ([SUN.md](../../userland/SUN.md), rule 1).

## What was measured

Read from the tree and the running system on 2026-11-10. Recheck before
relying on a row.

| Fact | Where | Consequence |
| --- | --- | --- |
| `EFER` is `0xd00` on every virtual CPU: long mode and NX, `SCE` clear. The kernel only ORs the NX bit into what the boot loader left (until E1, which clears `SCE` itself) | QEMU monitor on the booted image; `configure_efer` (was `enable_nx`) in `arch/x86_64/cpu_init.cpp` | `syscall` raises #UD today. The kernel must state `SCE` itself rather than inherit it: with `SCE` set and no entry point, ring 3 could crash the kernel |
| A ring-3 exception ends the process (`user: exception #N killed pid=`) after the stack-growth check. Every entry saves all registers and returns with `iretq` | `kernel/process.cpp`, `arch/x86_64/context.S` | **Corrected at E1:** only the `int 0x80`, timer and IPI entries save a full `SavedContext`. Exceptions use compiler-generated `__attribute__((interrupt))` stubs that cannot hand back another context, so #UD got its own small stub |
| There is no `swapgs`; the kernel finds its CPU through TR, and `IA32_FS_BASE`/`IA32_GS_BASE` belong to the user thread | `include/arch/x86_64/cpu.hpp` | Linux TLS (`arch_prctl`) has its mechanism. A fast `syscall` entry would need per-CPU entry stubs or `swapgs` everywhere, which is why it is deferred |
| `CR4` is `0x100620`: `OSXSAVE` is clear, and thread state is the 512-byte `fxsave64` area | QEMU monitor; `cpu.hpp` | Programs see no usable AVX and must not be given any. A libc that selects routines by CPUID stays on SSE2 |
| Subsystems are `posix` and `native`. An id selects a dispatcher, sizes per-process and per-thread private state, and receives a wait-recheck hook | `include/kernel/subsystem.hpp` | A third id is what the interface was built for |
| The subsystem of a process is chosen from `EI_OSABI` in two places, spawn and exec: the native value, or else POSIX | `kernel/process.cpp` | A Linux image is taken for a POSIX one today and dies on its first `syscall`. Selection must be explicit |
| The ELF loader accepts `ET_EXEC` and `ET_DYN`, maps `PT_LOAD` segments page by page with W^X, refuses addresses below `kUserBase` (`0x400000`), and builds SavanXP's own start-up stack. It passes the interpreter path to user mode instead of loading it. User address validation uses the same floor | `kernel/elf.cpp`, `kernel/vmm.cpp` | Mapping is reusable. The start-up stack, the floor and interpreter loading are not |
| User memory outside the image and stack is section views: physically contiguous sections, at most 128 views per process and 512 sections in the system, unmapped whole, access fixed when mapped. POSIX `mmap` is an anonymous-only wrapper over them | `include/kernel/vmm.hpp`, `subsystems/posix/sdk/v1/runtime/posix.c` | Linux `mmap`, `munmap` of part of a range and `mprotect` cannot be built on views |
| `fork` copies the address space; there is no copy-on-write | `kernel/vmm.cpp` | Correct, and slow for a large process. Not on the path to Xeyes |
| The kernel has no signals: `kill` ends the target for three signal numbers, and nothing is delivered to user mode | `subsystems/posix/kernel/posix_signals.hpp` | Signal delivery needs S3. Nothing before L7 may depend on it |
| Sockets are `AF_INET` only, and no loopback path was found. Named ports give each connection two pipes and are waitable | `kernel/net.cpp`, `include/abi/savanxp_native_abi.h` | The local X11 connection travels over a port, not over TCP |
| The file system has no symbolic links | `include/kernel/vfs.hpp` | The guest tree is installed with links resolved to files |
| A `windowd` client has no call to ask where the pointer is outside its window; capture reports relative motion only | `include/sxn/sxnwm.h` | Xeyes needs exactly that. A platform slice, P1 |
| The painter fills rectangles and ellipses | `include/sxgfx/gfx2d.h` | What Xeyes draws without extensions is already there |

On the build host:

- **A Linux ELF needs no extra toolchain.** The project's clang and lld link
  a libc-free static executable that makes three system calls (`execve`,
  `write`, `exit`). Its `EI_OSABI` is System V and its first segment is at
  `0x200000`, below the loader's floor. A static glibc program is stamped
  GNU; neither value is the native one.
- **What a real dynamic X client asks for.** The host's `xprop` (glibc,
  libX11, libxcb), traced from `execve` to exit, made 150 calls of 28 kinds.
  Loading five libraries took 25 `mmap` calls, five per library: the whole
  file read-only and private, then fixed mappings over it for the read-only,
  executable and writable parts, and a fixed anonymous one for the zeroed
  tail. Seven `mprotect` calls made relocated data read-only. The connection
  was `socket`, `connect`, `getpeername`, `setsockopt`, `fcntl`, `uname`, then
  `poll`, `writev`, `recvmsg` and `recvfrom`.
- **A core-only X server is enough for Xlib to work.** That `xprop` ran to
  completion against a test server of about 200 lines of Python, which
  answered the connection set-up and the five request types the client sent.
  Xlib asked for the `BIG-REQUESTS` and `XKEYBOARD` extensions, was told both
  are absent, and carried on.
- **Xeyes has a fallback for every extension it can use.** Read from the
  1.3.1 source: without XInput 2 it polls `XQueryPointer` on a timer, without
  RENDER it draws with `XFillArc`, without SHAPE the window is rectangular,
  and Present is optional. Built on the host against Arch Linux's libraries
  with RENDER and Present enabled, it links 18 shared objects besides the
  interpreter. Running that build under the tracer is left to L0.
- **A musl distribution ships the whole profile.** Alpine Linux's
  `latest-stable` index of 2026-11-10 lists `xeyes` 1.3.1-r0 (MIT, 32 KiB
  installed) against `musl` 1.2.6-r3, with libX11 1.8.14, libxcb 1.17.0 and
  libXt 1.3.1. By the index's dependency lists its closure is 18 shared
  objects: the eleven it names (libX11, libX11-xcb, libXext, libXi, libXmu,
  libXrender, libXt, libxcb and its damage, present and xfixes parts) and
  seven beneath them (libXau, libXdmcp, libbsd, libmd, libICE, libSM,
  libuuid), over a libc that is also the interpreter. The same index carries
  `busybox-static` 1.37.0-r31.

Not measured yet, and the work of L0: the system calls and X requests of
Xeyes itself and of any musl binary. Only the package index was read, not the
files in the packages.

## Paths considered

| | Path | Verdict |
| --- | --- | --- |
| A | Translation layer in the kernel, entered through the real `syscall` MSRs (FreeBSD's Linuxulator, WSL 1) | The first plan. The layer is right. Starting with the entry is not: it needs per-CPU state the kernel does not keep, and the `sysret` return has well-known hazards, all before one fixture runs |
| **B** | The same layer, entered through the #UD trap while `SCE` stays clear | **Chosen.** The trap, its stack and its register frame exist. The fast entry replaces it later without the guest noticing |
| C | A user-mode host that loads the ELF and receives its system calls reflected by the kernel, as Win32's host does for DLL calls | Rejected. Every call would cross the kernel at least three times. `fork`, `exec` and signals need the kernel anyway. The guest owns its whole address space and its `%fs`, so a host cannot hide inside it |
| D | Rewriting `syscall` sites in the image | Excluded by the objective, and unsound for static binaries |
| E | Emulating the CPU in a SUN process (the qemu-user, box64 and sogen shape) | Rejected. An x86-64 interpreter with SSE is more work than the kernel entry and is slower for ever |
| F | A real Linux kernel in a virtual machine (WSL 2) | Rejected. It needs a hypervisor in the kernel and virtual devices, and it still needs the X11 work to show a window on this desktop |
| G | Porting the programs to the POSIX SDK | That is what the POSIX subsystem is for. It is not binary compatibility |

For the display, with the client side fixed by the goal:

| | Path | Verdict |
| --- | --- | --- |
| **X1** | An X11 server written here as a SUN program, rootless, core protocol first | **Chosen.** The request set of one client is small and measurable, and each top-level window becomes a real desktop window |
| X2 | Porting an existing X server (Xvfb, kdrive) to the POSIX SDK | Rejected. It needs far more POSIX than the SDK has, and it shows a desktop inside a window instead of integrating with `windowd` |
| X3 | Running a Linux X server binary under this subsystem | Rejected. A server needs much more of Linux than its clients do: shared memory, signals, timers, listening sockets, font and keyboard data |
| X4 | Sending the client to an X server on another machine | A diagnostic aid. It does not close the goal |

## What changed from the first plan

- **The entry is no longer the first obstacle.** The old L1 wrote the
  `syscall` entry, with its stack switch and return path, before any Linux
  code ran. The trap makes that step small, and L1 now ends with a running
  executable.
- **S1 shrank to what is needed.** "Image recognition, launch ABI and start-up
  preparation" becomes one explicit selector in the native spawn call,
  inheritance across Linux `fork` and `execve`, and a Linux start-up stack.
- **S2 is named for what it is.** The old plan asked for reserve, commit,
  protect and query with a native consumer. The consumer is the Linux layer
  itself, inside the kernel, and the mechanism is a region list per process.
  A SUN call for it is added only when a native program needs one, which is
  SUN's own rule. Inventing a native consumer to justify a kernel mechanism
  inverted that rule.
- **Signals, process creation and a shell left the path.** The old L5 sat
  between the static tools and Xeyes, and Xeyes uses none of it. It moved to
  L7, and S3 with it.
- **The libc is decided.** The old plan left "musl, later glibc, driven by the
  selected binary" open. The first Xeyes profile is a musl one.
- **The X11 approach is decided.** The old L7 said "freeze the server/bridge
  approach before implementing it". It is frozen here, with the transport.
- **Two gates were added before Xeyes**: a dynamically linked program with no
  display, and a small X client. Each isolates one large piece of work.
- **The evidence is in the document.** The old baseline listed differences
  from Linux. This one records what the tree does and what real binaries asked
  for on the build host.

## Architecture

### Entry and dispatch

- `subsystem::Id::linux`, registered by `subsystems/linux/kernel/` with its
  dispatchers and state sizes. The `syscall` path is a *trap dispatcher*
  (`subsystem::register_trap_dispatcher`), separate from the one `int 0x80`
  reaches, so the neutral kernel never names Linux.
- The kernel clears `EFER.SCE` on every processor during start-up, so the
  behaviour no longer depends on the boot loader.
- On #UD from ring 3 in a Linux process, the handler reads two bytes at the
  saved RIP through the checked user copy. If they are `0f 05`, it advances
  RIP by two, sets RCX to the new RIP and R11 to the saved flags as the
  instruction would, and dispatches: number in RAX, arguments in RDI, RSI,
  RDX, R10, R8, R9, result in RAX. Anything else is a real invalid opcode and
  ends the process.
- `int 0x80` from a Linux process returns `-ENOSYS`. On Linux that vector is
  the 32-bit ABI, which this subsystem does not offer. No Linux number can
  reach the native or POSIX tables, because the dispatcher is chosen by the
  process and not by the number.
- A process of another subsystem that executes `syscall` dies as it does
  today.
- An unknown or unimplemented call returns `-ENOSYS` and logs
  `linux: pid=N unimplemented syscall M` once per number per process. A trace
  switch logs every call with its result, in a form that can be compared with
  a host trace.

### Launch and start-up

`linux <guest path> [arguments]` is the only entry from outside. It spawns
through `SXN_SYS_SPAWN_EX` with an explicit Linux selector and an environment
block; the kernel validates the image for that ABI or fails the spawn. A
Linux process that calls `execve` or `fork` produces Linux processes. The
Start menu and Files reach a Linux program through a shortcut that runs the
launcher.

The loader keeps its segment mapping and gains a second start-up builder. A
Linux process starts with RSP at `argc`, then `argv`, `envp` and the
auxiliary vector: `AT_PHDR`, `AT_PHENT`, `AT_PHNUM`, `AT_PAGESZ`, `AT_BASE`,
`AT_ENTRY`, the four identity entries, `AT_SECURE`, `AT_RANDOM`, `AT_EXECFN`
and `AT_NULL`. There is no vDSO, so `AT_SYSINFO_EHDR` is absent and the libc
makes real calls for the clock. Every other register is zero.

The user address floor drops from `0x400000` to `0x10000` for every
subsystem, because a Linux `ET_EXEC` is linked wherever its linker chose. The
kernel loads a `PT_INTERP` interpreter itself for a Linux image, from the
guest root, and enters at the interpreter's entry point.

### Memory regions

A sorted list of regions per process, kept beside the section views and not
instead of them. A region has a range, an access, and either nothing behind
it or private pages. Its pages are allocated one at a time on first touch and
zeroed. The operations are the ones Linux programs perform:

- map anonymous memory at a hint, or at a fixed address replacing what was
  there;
- unmap any page range, splitting regions;
- change the access of any page range, splitting and merging;
- map part of a file privately, which reads the bytes into pages when mapped
  (there is no page cache to share);
- grow and shrink the program break, which is one more region.

W^X holds: a request for writable and executable memory at once is refused
with the Linux error, and recorded as a difference. Shared file mappings are
refused until a consumer needs them. `fork` copies regions as it copies the
rest. The list logic (split, merge, find a gap) is plain code with host
tests; the page work reuses what the stack and the loader already do.

### Descriptors, files and the guest root

The Linux descriptor table is the subsystem's own, in its per-process state.
An entry names one or two kernel handles and carries the Linux flags, so
descriptor numbering, `dup`, close-on-exec and sockets follow Linux rules
without changing the kernel's handle table.

Every path a Linux process names resolves under `/system/linux`. The
exceptions are a short explicit table, pinned as fixtures need them:
`/dev/null`, `/dev/zero`, `/dev/urandom`, `/dev/tty`, `/proc/self/exe`, and
one prefix that reaches the native tree. SavanXP credentials and permission
checks apply underneath; the prefix is an installation boundary, not a
sandbox.

The guest tree is installed from a pinned distribution by a script in the
style of the ports: it verifies hashes, resolves symbolic links into files
under each name the closure uses, and syncs only the files it owns. Nothing
of it is committed.

### Signals, threads and what is absent

`rt_sigaction` and `rt_sigprocmask` keep real state and return the previous
values, but nothing is delivered: a fault or a `kill` ends the process, and
the parent sees the Linux wait status for that signal. `clone` for threads
and `futex` return `-ENOSYS`. Both are stated limits of the profiles up to
L6, and no fixture before L7 may rely on them.

### The X11 connection

`connect` on an `AF_UNIX` stream socket looks its address up in a table the
subsystem owns and opens the native port bound to it. The first entry maps
display 0, in both the abstract and the path form libxcb tries, to the port
`x11d` creates. The socket descriptor wraps the connection's two pipes;
`poll`, `writev`, `recvmsg` and the rest act on them. Listening sockets, file
descriptor passing and other address families are not in the profile.

### `x11d`

A SUN program started on demand by the launcher when the program's
environment names a display. It is not on the boot path.

- **Protocol**: X11 core, little-endian clients, no authorization. It
  advertises no extension until one is implemented; `QueryExtension` answers
  "absent".
- **Windows**: a mapped child of the root is a `windowd` window, framed by the
  window manager, titled from `WM_NAME`. Closing it sends `WM_DELETE_WINDOW`
  when the client asked for that protocol and ends the connection otherwise.
  Windows below a top-level one exist only inside `x11d`, which keeps the
  tree, the clipping and the Expose events.
- **Drawing**: graphics contexts and the fill requests the inventory lists,
  rasterized into the window's surface with the platform painter.
- **Input**: pointer and key events from `windowd`, and `QueryPointer` for the
  whole desktop through P1.
- **Shape**: the protocol engine is portable C with host tests. On the build
  host it also listens on TCP with an offscreen surface, so real host clients
  run against it under CTest.

## Kernel and platform slices

### E1 — the `syscall` trap

Size S. `EFER.SCE` cleared explicitly on the boot processor and on each
application processor; the Linux id; the #UD recognition described above.

**Done when:** `nativetest` starts a child that executes `syscall` and sees
it die with the invalid-opcode status, on 1, 2 and 4 vCPUs. The Linux half is
proven by L1's fixture.

**Closed on 2026-11-14** ([LINUX.md](../../userland/LINUX.md)). One thing
differed from the plan: the #UD vector needed a `CONTEXT_ENTRY` stub of its
own, because exception entries did not save a context a dispatcher can
use. The Linux dispatchers exist and answer `-ENOSYS` to everything; L1
fills them in.

### Shared Sx groundwork

The three slices both compatibility roadmaps name. Win32 defers all of them
past its first application
([Win32 roadmap](WIN32_SUBSYSTEM_ROADMAP.md#deferred-s1-s2-and-s3)).

| Slice | Scope for Linux | Size | First needed |
| --- | --- | --- | --- |
| S1 — launch selection | An explicit ABI selector and an environment block in the native spawn request; inheritance across Linux `fork` and `execve`; the Linux start-up stack; the lower address floor | M | L1 |
| S2a — anonymous regions | The region list, map and unmap of page ranges, `mremap` with move, first-touch zeroed pages, the program break | M | L2 |
| S2b — protection and files | Access changes on ranges, fixed mappings that replace, private file mappings, the interpreter load | M | L4 |
| S3 — user exceptions | Fault information and saved registers delivered to a user handler, and a validated return | L | L7 |

Rules that hold for all of them:

- S1 never carries opaque subsystem state into a process of another
  subsystem. A failed load leaves no process, handle or mapping behind, and a
  failed `execve` leaves the old image running.
- S2 is not "more views". Raising `kMaxSectionViews` does not provide partial
  unmapping or protection changes.
- The platform's W^X policy is not relaxed for foreign programs. Unsupported
  requests fail and are recorded.
- S3 supplies a mechanism. Linux signal numbers, masks, frames and
  `rt_sigreturn`, and Windows structured exception handling, belong to their
  subsystems.
- Each slice lands with its consumer. For S1 that is the `linux` launcher,
  with `nativetest` assertions for selection and refusal. For S2 it is the
  Linux layer, with host tests of the region list and guest fixtures. A SUN
  call for memory regions is not added until a native program needs one.

### P1 — the pointer position for a desktop client

Size S, in `windowd` and `sxnwm.h`, not in the kernel. A client may ask for
the pointer position in desktop coordinates while the pointer is elsewhere.
`x11d` is the consumer and `nativetest` asserts it. Whether this is a query
or a subscription is decided there, together with who may use it.

## Phases

```text
L0 ─► L1 ─► L2 ─► L3 ─► L4 ─► L5 ─► L6
      E1    S2a         S2b   P1
      S1
```

L0, the host tests of the region list and the protocol engine of `x11d` need
no kernel change and can start now.

### L0 — profiles and inventory

Size S. Nothing of the distribution is committed: only the fixtures'
sources, the tooling that traces them, hashes, lists and reference outputs.

- Pin the distribution: an Alpine Linux release, by version and by the hash
  of every package taken from it. It supplies `xeyes` and its closure, the
  musl runtime, and `busybox-static` for L3, all as unmodified files.
- Pin the fixture toolchain: the project's clang and lld for libc-free
  programs. For C fixtures, try the same clang and lld against the pinned
  `musl-dev` files before adding a musl toolchain to the host tools; either
  way it is found by CMake and optional for a normal build.
- For each fixture and for Xeyes: hashes, the interpreter, the transitive
  libraries, and a system call trace on Linux from start to exit with
  arguments (`strace -f`).
- For Xeyes and the L5 fixture: the X request trace, taken by running them
  against a recording server, with the extensions disabled the same way the
  profile will disable them, and reference screenshots for known pointer
  positions.
- A per-call map from the inventory to an existing or missing kernel
  primitive, with the FreeBSD revision studied.

**Done when:** the profile is recorded in `docs/userland/` with hashes, the
system call and request matrices marked as required at start-up, required in
use, or reachable only from a feature declared out of scope, and the
reference outputs are kept.

**Closed on 2026-11-14** (L0) in
[LINUX_XEYES_PROFILE.md](../../userland/LINUX_XEYES_PROFILE.md), reproducible
with `subsystems/linux/profile/profile.sh`. No musl toolchain was needed. What
it added to the plan: `mremap` in S2a, `clock_gettime` in L2 (hidden behind
the vDSO on the host), `sendfile`, `getdents64`, `umask` and the working
directory in L3, and `getrandom` in L4. No run used threads, signals or
process creation.

### L1 — entry, launch and the first executable

Size M. E1, S1, the dispatcher, and `write`, `exit` and `exit_group`.

**Done when:** the same libc-free ELF, identified by hash, prints the same
bytes and returns the same status on Linux and on SavanXP, started by `linux`
from a SUN parent that captures its output. A second fixture checks six
arguments, register preservation across calls and under preemption on 1, 2
and 4 vCPUs, the start-up stack and the auxiliary vector. Bad headers,
truncated images, bad pointers, an unsupported machine and a missing handle
fail without a kernel panic or a leak. `int 0x80` from the fixture returns
`-ENOSYS`. Native and POSIX launches are unchanged. `linux-hello` is a
registered smoke scenario.

**Closed on 2026-11-14** ([LINUX.md](../../userland/LINUX.md)), with the floor
change landed alone before it. What the plan did not say: the selector is
`SXN_SPAWN_FLAG_SUBSYSTEM` with a block in RDX, and its `path` is a guest
path the kernel maps under `/system/linux`; an image with `PT_INTERP` is
refused until L4; the bad-pointer and missing-handle cases are a third
fixture, `errors`, whose output was taken on Linux too. S1's inheritance
across `execve` waits for that call (L7): no Linux program can reach exec
before it.

### L2 — static C programs

Size M to L. S2a, thread-local storage through `arch_prctl`, clocks
(`clock_gettime`, `nanosleep`), and the file calls of the inventory: open,
read, write, vectored write, seek, status, close, unlink, terminal queries.

**Done when:** pinned static musl fixtures check arguments and environment,
thread-local data, heap use from small to several megabytes with memory
returned, and file create, read, seek, status and close, against Linux,
including short reads and errors. A file written by a fixture is read back
after a guest restart. The region list has host tests for splitting, merging,
overlap and overflow.

### L3 — a bounded static tool set

Size M. The distribution's static BusyBox, with a stated list of applets such
as `echo`, `cat`, `ls`, `pwd`, `wc` and `mkdir`: directory reading, the
working directory, path status, and what else those applets issue. Its shell
is not in the list.

**Done when:** those commands run unchanged on prepared directory trees and
match Linux after documented normalization of unstable fields. Bad paths and
denied access are covered. The executables and their data survive
`./build.sh build`. `linux-tools` is a smoke scenario.

**This closes the static checkpoint.**

### L4 — dynamic programs

Size L. S2b, the interpreter load, guest library lookup, and `getrandom`
over `arch::random_u64` (RDRAND, or a TSC mix without it).

**Done when:** a pinned dynamically linked program with no display, using
several shared libraries from the pinned distribution, runs unchanged with
the same output as on Linux. Thread-local data in a library, a missing
library and a missing interpreter are covered, each failing with the
reference's message. Mapping counts well beyond the old view budget are
exercised. SavanXP's own dynamic loader and libraries are never used for a
Linux image.

### L5 — a window from a Linux client

Size L. Local stream sockets over ports, `poll`, P1, and `x11d` with the
requests of a first, small X client: one written here against the pinned
distribution's Xlib, which opens a window, fills shapes, reports the pointer
and closes.

**Done when:** that client, identified by hash, shows the same window content
on Linux and on SavanXP, redraws after being resized, follows a recorded
pointer path and closes from the caption without leaving a process,
connection, port or surface. The protocol engine passes its host tests,
including malformed and truncated requests. Native and POSIX windows and the
taskbar behave as before. `linux-xclient` is a smoke scenario.

### L6 — Xeyes

Size M, if the earlier gates were honest.

Run the pinned Xeyes with its library closure from `/system/linux`, from the
Start menu and from Files. Add the requests its inventory lists beyond L5's,
and no extension it can do without.

**Done when:** the same executable opens a visible window, draws both eyes,
and places the pupils correctly for several known pointer positions,
including positions outside its window. It survives occlusion, moving,
resizing, closing and relaunching without leaked processes, connections or
surfaces. Input sequences, logs and visual assertions are kept; a start-up
log or one screenshot does not close the goal. Native and POSIX desktop
behaviour still passes, and the executable, its libraries and resources
survive a normal build and a guest restart. `linux-xeyes` is a smoke
scenario, skipped when the guest tree is not installed.

### L7 — later profiles

Not planned in detail. Each names its consumer and records its inventory
first:

- S3 and Linux signals: masks, pending state, frames, `rt_sigreturn`,
  interruption and restart.
- Process creation with a shell: `fork`, `execve`, wait status, pipes and
  redirection in a pinned static shell running a script.
- Threads: `clone`, `futex`, thread exit and robust lists, with a
  multithreaded consumer on several vCPUs.
- glibc, static and dynamic. Its start-up asks for more than musl's, as the
  build-host trace shows.
- X extensions by consumer: SHAPE, RENDER, XInput 2, `BIG-REQUESTS`.
- The fast `syscall` entry, when a measurement shows the trap is the cost.
- Copy-on-write `fork`, shared file mappings, `/proc` beyond one entry,
  `epoll`, network sockets.

Not promised without a separate decision: a distribution, a package manager,
systemd, namespaces and containers, 32-bit programs, Wayland, direct
rendering, kernel modules. Running Wine's Linux build is a separate question
and no part of the Win32 plan.

## Implementation reference: FreeBSD Linuxulator

FreeBSD's Linuxulator is the primary implementation reference: a maintained
layer that runs unmodified 64-bit Linux binaries on another kernel
([Handbook](https://docs.freebsd.org/en/books/handbook/linuxemu/)). Linux
remains the behavioural authority. A Linuxulator limitation or a
FreeBSD-specific behaviour is not something to reproduce.

| FreeBSD source | Use here |
| --- | --- |
| [`sys/amd64/linux/linux_sysvec.c`](https://github.com/freebsd/freebsd-src/blob/main/sys/amd64/linux/linux_sysvec.c) | Argument registers, results, initial registers and the start-up stack |
| [`sys/amd64/linux/syscalls.master`](https://github.com/freebsd/freebsd-src/blob/main/sys/amd64/linux/syscalls.master) | A declarative call inventory; consider generating the dispatcher's declarations, checked against the Linux table |
| [`sys/compat/linux/linux_file.c`](https://github.com/freebsd/freebsd-src/blob/main/sys/compat/linux/linux_file.c) | Flag, descriptor and error translation around file operations |
| [`sys/compat/linux/linux_mmap.c`](https://github.com/freebsd/freebsd-src/blob/main/sys/compat/linux/linux_mmap.c) | The `mmap` and `mprotect` corner cases worth copying as tests |
| [`sys/compat/linux/`](https://github.com/freebsd/freebsd-src/tree/main/sys/compat/linux) | Later: processes, signals, futexes. Read only what a fixture requires |

It is a reference, not a component to compile against Sx. Its code depends on
FreeBSD's process, VFS, VM and locking interfaces. Implement the
corresponding operation on this kernel; do not import a BSD interface layer
to satisfy those dependencies.
[NetBSD's compatibility layer](https://www.netbsd.org/docs/compat.html) is a
second reading. OpenBSD removed its own in 6.0.

## Starting the implementation

For whoever picks this up, human or agent. Read this document, then
[KERNEL_NATIVE_API.md](../../kernel/KERNEL_NATIVE_API.md) for how POSIX was
separated from the kernel, [SECTIONS.md](../../kernel/SECTIONS.md), and
`subsystems/posix/kernel/` as the existing example of a kernel half.
`AGENTS.md` governs every commit.

**Order of work.** Each line leaves the tree green.

1. L0. It needs a Linux host, which the build host is, and no kernel change.
2. E1, with its `nativetest` assertion.
3. S1 and the rest of L1.
4. The region list with host tests, then S2a and L2.
5. L3, L4.
6. The `x11d` protocol engine with host tests can start any time after L0;
   P1 and L5 need L4.

**The first fixture**, proven on the build host:

```sh
clang -nostdlib -static -fuse-ld=lld -o hello hello.S
```

```asm
    .globl _start
_start:
    mov  $1, %eax            # write
    mov  $1, %edi
    lea  msg(%rip), %rsi
    mov  $6, %edx
    syscall
    mov  $60, %eax           # exit
    mov  $7, %edi
    syscall
msg: .ascii "hello\n"
```

**Rules that are easy to break.**

- A call exists because a pinned fixture issues it. No call returns success
  without doing its work.
- The Linux layer calls the neutral kernel. It does not forward to the POSIX
  dispatcher and includes no POSIX SDK header.
- Linux numbers, structure layouts and flag values are written in the
  subsystem's own headers from the documented ABI.
- Guest files are never committed, and a valid `build/disk.img` is never
  recreated to install them.
- A phase closes on its "Done when", with hashes and retained outputs.

## Evidence and tests

| Source | Role | Limit |
| --- | --- | --- |
| Linux on the build host | The behavioural reference: the same file, its output, its trace | One kernel and libc version; record them |
| Host tests | The region list, flag and error translation, structure layouts, the X protocol engine | No kernel behaviour beyond what the test encodes |
| Real X clients on the build host against the `x11d` engine | Protocol conformance with clients nobody here wrote | Linux only; skipped elsewhere |
| Smoke scenarios in QEMU | The acceptance gate of every phase, with serial logs and screenshots through QMP | Slower; run after the host tests pass |
| The call trace of the subsystem | Line-by-line comparison with the host trace of the same binary | Addresses, process ids and times are normalized; statuses, byte counts and order are not |
| [Linux Test Project](https://github.com/linux-test-project/ltp) | Selected cases, reduced | Its harness needs far more than the call under test. A reduced case is reported as reduced |
| FreeBSD and Linux sources | Reading | Reference only, under the rule below |

Report pass, fail, skip, crash and timeout separately; a test that could not
run is not coverage. Fixtures that need the guest tree are skipped, not
failed, when it is not installed.

## Build, source and licence rules

- New code under `subsystems/linux/` is the project's own, under its licence.
  The kernel half is compiled into the kernel image, so nothing copyleft goes
  into it ([SYSTEM_LAYERING.md](../../userland/SYSTEM_LAYERING.md#licensing-and-prior-art)).
- The Linuxulator is `Reference` by default. A small self-contained
  conversion may become a `Selective port` after its revision, file licence
  and notices are reviewed and recorded in
  [THIRD_PARTY_PROVENANCE.md](../../third-party/THIRD_PARTY_PROVENANCE.md),
  under [THIRD_PARTY_ADOPTION.md](../../third-party/THIRD_PARTY_ADOPTION.md).
- musl, BusyBox, Xeyes and the X.Org libraries are guest files with their own
  licences. They are fetched by hash, installed under `/system/linux`, and
  recorded in the provenance registry when an image that contains them is
  distributed.
- Host tools are found by CMake and listed in
  [BUILD_CMAKE.md](../../build/BUILD_CMAKE.md). The musl toolchain is optional
  and its fixtures are skipped without it.
- Kernel, loader and drawing changes run the host tests, `native-test`, the
  POSIX and thread regressions and `./build.sh smoke sxgui-smoke`, then the
  persistence check of [AGENTS.md](../../../AGENTS.md), extended to the files
  under `/system/linux`.
- Each phase that changes behaviour gets its `[Unreleased]` entry in the
  commit that lands it, and what was built is described under `docs/`. This
  roadmap stays a plan.

## Risks

| Risk | Effect | Answer |
| --- | --- | --- |
| The pinned Xeyes needs an extension after all | L6 grows | L0 traces it with extensions absent before any server code. Each of its fallbacks is in its source, and the package links the same optional libraries the source guards |
| musl's dynamic linker asks for memory behaviour the region list lacks | L4 grows | The inventory comes first, and S2b is sized from it |
| The trap is too slow for a real workload | Poor performance | Measure at L3. The fast entry is a contained change behind the same dispatcher |
| Lowering the user address floor exposes an assumption elsewhere | Regressions in native or POSIX programs | It lands alone, with the full regression set, before any Linux code depends on it |
| One process cannot hold several `windowd` connections | A client with two top-level windows cannot be shown | Check at L5; Xeyes needs one. A neutral change with its own consumer if required |
| The work turns into a system call catalogue | The goal recedes | The matrix is the scope. A call without a fixture is not written |

## References

- [Linux x86-64 system call table](https://github.com/torvalds/linux/blob/master/arch/x86/entry/syscalls/syscall_64.tbl)
  and [x86 UAPI](https://github.com/torvalds/linux/tree/master/arch/x86/include/uapi/asm)
- [System V x86-64 psABI](https://gitlab.com/x86-psABI/x86-64-ABI), for the
  process start-up state
- musl: [system call entry](https://git.musl-libc.org/cgit/musl/tree/arch/x86_64/syscall_arch.h),
  [start-up](https://git.musl-libc.org/cgit/musl/tree/src/env/__libc_start_main.c)
  and [dynamic linker](https://git.musl-libc.org/cgit/musl/tree/ldso/dynlink.c)
- [X Window System protocol](https://www.x.org/releases/current/doc/xproto/x11protocol.html)
  and [Xeyes](https://gitlab.freedesktop.org/xorg/app/xeyes)
- [Thread groundwork](../done/THREADS_ROADMAP.md),
  [the Sx API](../../kernel/KERNEL_NATIVE_API.md) and
  [sections and views](../../kernel/SECTIONS.md)
