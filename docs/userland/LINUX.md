# SSL: the Savan Subsystem for Linux, as built

What exists of the Linux subsystem and how it works. The plan, the paths
rejected and the phases still ahead are in the
[roadmap](../roadmaps/open/LINUX_SUBSYSTEM_ROADMAP.md); the measured scope of
the first profile is in [LINUX_XEYES_PROFILE.md](LINUX_XEYES_PROFILE.md).

**Built so far: E1, the `syscall` trap; L1 with S1, a static, libc-free
Linux executable runs unchanged, started by the `linux` launcher; L2
with S2a: a static musl program runs unchanged, with its thread-local data,
its heap, files under the guest root and the clocks; L3 adds the bounded
static BusyBox tools below; L4 runs the pinned dynamic musl `jq` with its
original libraries.** Every call not below answers `-ENOSYS`.

## The `syscall` instruction raises #UD

`configure_efer` in `arch/x86_64/cpu_init.cpp` runs on the boot processor and
on every application processor. It sets EFER.NXE, which W^X needs, and clears
EFER.SCE. With SCE clear, the `syscall` instruction raises an invalid-opcode
exception (#UD) instead of jumping to `LSTAR`. Limine already leaves SCE
clear, but the kernel states the bit itself. With SCE set and nothing in
`LSTAR`, ring 3 could send the processor to address 0 in ring 0.

Vector 6 has its own stub, `x86_64_invalid_opcode_entry` in
`arch/x86_64/context.S`:

- **From ring 0** (CS's low bits clear), it jumps with the stack untouched to
  the ordinary exception handler, `isr_6`, through
  `savanxp_invalid_opcode_kernel`. It does not take the kernel lock: a kernel
  `ud2` can happen with the lock held.
- **From ring 3**, it is a `CONTEXT_ENTRY`, like the `int 0x80` and timer
  entries. All registers are saved as a `process::SavedContext`, the kernel
  lock is taken (counted against `Door::exception`), and the handler returns
  the context to resume, which may belong to another thread.

The other exception vectors keep their compiler-generated
`__attribute__((interrupt))` stubs. Those save only what the handler clobbers
and cannot hand back a different context, which is why #UD needed its own.

## Trap dispatchers

`process::handle_invalid_opcode` serves every #UD from ring 3:

1. It looks up the trap dispatcher of the process's subsystem
   (`subsystem::trap_dispatcher_for`).
2. If there is one, the code is 64-bit (CS is `kUserCodeSelector`) and the
   two bytes at RIP, read through `read_from_process_memory`, are `0f 05`, it
   does what the instruction does: RIP moves past it, RCX gets the new RIP,
   R11 the flags. It then calls the dispatcher. The return path is the same
   as for `int 0x80` (`finish_user_call`): termination requested meanwhile, a
   sibling's exec, and the handover to a thread the call woke.
3. Anything else is a real invalid opcode. The kernel prints
   `user: exception #6 pid=N name=... rip=0x...`, then
   `user: exception #6 killed pid=N`, and the process ends with status
   128 + 6.

A subsystem registers one with `subsystem::register_trap_dispatcher`. Only
Linux does. A native or POSIX process that executes `syscall` therefore ends
as it always did, and Linux numbers can never reach their tables: the
dispatcher is chosen by the process, not by the number.

## Starting a Linux program

### The launcher

```text
linux [-e NAME=VALUE]... GUEST_PATH [ARGUMENT]...
```

`/system/linux.sxe` (`subsystems/linux/launcher/linux.c`), a SUN program, is
the only way in. `GUEST_PATH` names a file in the guest tree: `/fixtures/hello`
is `/system/linux/fixtures/hello`. The program gets `GUEST_PATH` as `argv[0]`,
the arguments after it, and exactly the environment the `-e` options give,
in order. Nothing is inherited. It shares the launcher's standard handles.
The launcher waits for it and exits with its status. When the program cannot
start, the launcher names the reason on standard error and exits with 127.

### The spawn request

The launcher calls `SXN_SYS_SPAWN_EX` with `SXN_SPAWN_FLAG_SUBSYSTEM` and,
in RDX, a `struct sxn_spawn_subsystem`: `SXN_SUBSYSTEM_LINUX`, `envc` and
`envp` (`sxn_spawn_ex_subsystem` in `sxn.h`). With that flag:

- The request's `path` is a guest path. `process::linux_guest_path` puts it
  under `/system/linux`. `.` is dropped, and `..` never climbs above the
  guest root. A relative path is refused with `-ENOENT`.
- The environment is copied like `argv`: at most 128 strings in 8 KiB, never
  truncated. An unreadable block or environment, or another subsystem, is
  `-EINVAL`.
- The image must be a Linux x86-64 executable: `EI_OSABI` System V (0) or GNU
  (3). Anything else, including a SavanXP image, is `-ENOEXEC`. A `PT_INTERP`
  path is opened in the guest tree with the process's credentials. A missing
  interpreter is `-ENOENT`, a denied one `-EACCES`; malformed, duplicate or
  nested interpreter headers are `-ENOEXEC`. The selector is the only thing that makes a process Linux. Nothing is
  guessed from the file.

Without the flag, nothing changes: `EI_OSABI` chooses between native and
POSIX, as before. A refused spawn leaves no process, handle or mapping
behind.

### The start-up state

`elf::load_user_image` takes an `elf::LinuxStartup` and then builds Linux's
stack instead of SavanXP's (`build_linux_stack` in `kernel/elf.cpp`). From
the top down: 16 random bytes, the `AT_EXECFN` string, the environment
strings and the argument strings. Above a 16-byte aligned RSP sit `argc`,
`argv[]` and a null, `envp[]` and a null, and the auxiliary vector:

| Entry | Value |
| --- | --- |
| `AT_PHDR`, `AT_PHENT`, `AT_PHNUM` | The program header table in memory (from `PT_PHDR`, or the `PT_LOAD` that holds it), 56, its count |
| `AT_PAGESZ` | 4096 |
| `AT_BASE` | The interpreter load bias, or 0 for a static executable |
| `AT_ENTRY` | The image's entry point |
| `AT_UID`, `AT_EUID`, `AT_GID`, `AT_EGID` | The process's credentials. The effective group is the group: the kernel keeps no separate one |
| `AT_SECURE` | 0 |
| `AT_RANDOM` | The 16 bytes, from `arch::random_u64` |
| `AT_EXECFN` | The guest path the program was started by |

`AT_SYSINFO_EHDR` is absent: there is no vDSO, so a C library makes real
system calls for the clocks. Every general register but RSP is zero at the
entry point.

A Linux `ET_EXEC` may be linked anywhere above the user floor, `0x10000`
([USER_ADDRESS_SPACE.md](../kernel/USER_ADDRESS_SPACE.md)); lld puts a static
one at `0x200000`. A PIE (`ET_DYN`) goes at `0x400000`, as any other. The Linux interpreter must
be `ET_DYN`; it is mapped separately at bias `0x40000000`, with its own ELF
validation and W^X checks. Overlap with the main image is refused. The kernel
enters the interpreter while `AT_ENTRY` and `AT_PHDR` still name the main
executable. All those pages are regions, so the interpreter can protect RELRO.
The subsystem supplies an interpreter provider to the ELF loader; the loader
itself never resolves guest paths. SavanXP's user-mode loader is not involved.

### What is inherited

A Linux process's children would be Linux processes: `fork` copies the
subsystem with the rest of the process. No Linux program can fork or exec
yet, since both are `-ENOSYS` until the shell profile of L7. An exec into
another image keeps the Linux ABI only when that call exists.

## The kernel half

`subsystems/linux/kernel/`, compiled into the kernel. `initialize_linux_subsystem`
registers, for `subsystem::Id::linux`:

| Registration | What it does today |
| --- | --- |
| Trap dispatcher, `dispatch_linux_syscall` | The calls below. Every other number returns `-ENOSYS` (Linux's 38) and is logged once per process: `linux: pid=N unimplemented syscall M` |
| Ordinary dispatcher, `dispatch_linux_int80` | `int 0x80` is the 32-bit Linux ABI, which SSL does not offer: always `-ENOSYS` |
| Per-process state | Two bitmaps of the numbers already reported, as unimplemented and as asked for something unsupported (512 bits each); the program break; the descriptor table; the working directory and creation mask |
| Per-thread state | `set_tid_address`'s pointer and a pending `sendfile` pipe wait |
| Wait-recheck hook | Retries a `sendfile` syscall when its blocking output pipe can accept the pending chunk, or its last reader closed |

A call SSL has, asked for a variant it does not offer (a shared mapping, an explicit `sendfile` offset, an `arch_prctl` code other than
`ARCH_SET_FS`), fails with its Linux error and is logged once per call per process:
`linux: pid=N syscall M: unsupported WHAT`.

Linux numbers and error values are written in `linux_abi.hpp` from the Linux
ABI, under `ssl::abi`. Nothing there comes from the POSIX SDK.

### Descriptors and paths

A Linux descriptor is an entry of the subsystem's own table (256 of them),
naming one kernel handle and carrying its Linux flags; the kernel's handle
table is not renumbered. So numbering follows Linux's rule, the lowest free
descriptor, and `O_CLOEXEC` is kept for the exec of L7. Descriptors 0, 1 and
2 are entered on first use from the handles of the same numbers the spawn
gave.

Every path resolves under `/system/linux` through `process::linux_guest_path`,
`..` never above it. The working directory starts at `/`; `chdir` changes it
in SSL state and relative paths resolve from it. The creation mask starts at
0022. Files are the VFS's (`sx::open_file`,
`vfs::open`, `vfs::unlink`) with the process's credentials, so SavanXP's
permission checks apply underneath. The test image has `/system/linux/work`,
`/work` to a guest, for the files fixture.

### The calls

| Call | What it does |
| --- | --- |
| `read` (0) | On an open descriptor, through the native read (`process::native_read_handle`): a pipe with nothing in it parks the thread. An unwritable buffer is `-EFAULT`; a descriptor open for writing only is `-EBADF` |
| `write` (1) | On an open descriptor, `-EBADF` otherwise. An unreadable buffer is `-EFAULT`; zero bytes is 0 whatever the buffer. The write is the native one (`process::native_write_handle`), so a full pipe parks the thread and the reader's progress finishes the call |
| `open` (2) | A regular file or read-only directory, with the access mode, `O_CREAT` (with the process umask applied) and `O_TRUNC`; `O_CLOEXEC` is kept, `O_LARGEFILE` is always true and `O_NOCTTY` has no terminal to refuse. Any other flag is refused: `O_APPEND` needs every write at the end, not only the first, directories open read-only, and `O_DIRECTORY` requires one. The lowest free descriptor; `-EMFILE` when the table or the kernel's is full |
| `close` (3) | Frees the descriptor and closes its handle |
| `stat` (4), `fstat` (5) | Linux's `struct stat`: the type, the permission bits and owner SavanXP records, the size, 512-byte blocks, the modification time for all three times, and a node number (`vfs::node_number`) for `st_ino`. A pipe, socket or device handed to the process gives its type alone |
| `lseek` (8) | `SEEK_SET`, `SEEK_CUR` and `SEEK_END` on a file; `-ESPIPE` on a pipe, socket or device; before 0 is `-EINVAL` |
| `lstat` (6) | As `stat`: the VFS has no symbolic links |
| `getdents64` (217) | A directory cursor encoded as aligned Linux records, with node numbers, types and names. A record that does not fit remains for the next call; a buffer too small for one record is `-EINVAL` |
| `getcwd` (79), `chdir` (80) | The guest working directory, confined to the guest root; changing it requires a searchable directory. `getcwd` returns the byte count including NUL; too small is `-ERANGE` |
| `mkdir` (83), `umask` (95) | Directory creation through the VFS, with the per-process creation mask; `umask` returns the old mask |
| `getuid` (102) | The real uid in the process credentials |
| `fcntl` (72) | `F_GETFD` and `F_SETFD`, with `FD_CLOEXEC` kept in the Linux descriptor table. Other commands fail with `-EINVAL` |
| `sendfile` (40) | A regular readable file to a writable handle, one page per call (a permitted short transfer). A full blocking pipe parks before transferring and retries when room is available; the file offset advances only by bytes written. The measured profile uses a null offset; explicit offsets are refused with `-EINVAL` and logged |
| `unlink` (87) | Removes a file; a directory is `-EISDIR`, as on Linux |
| `writev` (20) | On an open descriptor. The vector is read twice: every length first (`-EINVAL` past `SSIZE_MAX`, more than 1024 entries), then each segment is written. Only the first segment with bytes may block, since the waker completes the call with that segment's count; a later one that would block ends the call short, and the caller writes the rest, as musl's stdio does. A count is capped at `MAX_RW_COUNT`, as is `write`'s |
| `ioctl` (16) | `-ENOTTY` on an open descriptor, whatever the request: no descriptor is a terminal yet. That is Linux's answer for a file or a pipe to `TCGETS` and `TIOCGWINSZ`, the two musl asks. `-EBADF` otherwise |
| `mmap` (9) | Private anonymous memory and eager private file snapshots, through regions ([MEMORY_REGIONS.md](../kernel/MEMORY_REGIONS.md)). `MAP_FIXED` replaces the regions in the range; `MAP_FIXED_NOREPLACE` is `-EEXIST` over anything. A hint is taken when free. `MAP_DENYWRITE`, `MAP_EXECUTABLE`, `MAP_NORESERVE` and `MAP_STACK` change nothing here, as on Linux; another flag is refused. A shared mapping is `-EINVAL`, writable and executable memory `-EACCES` (W^X, a recorded difference from Linux), and a fixed mapping over memory that is not a region `-EINVAL` |
| `mprotect` (10) | Changes access on whole pages, splitting and merging regions; `-ENOMEM` for a hole, `-EINVAL` for unaligned addresses or unknown protection bits, `-EACCES` for W+X. Resident data survives `PROT_NONE` and restoration. The neutral protection mechanism currently refuses while another CPU holds the space; see [MEMORY_REGIONS.md](../kernel/MEMORY_REGIONS.md) |
| `getrandom` (318) | Flags 0, up to 256 bytes per call (a permitted short result), from `arch::random_u64`: RDRAND or its TSC-mix fallback. This is the existing platform entropy source, not a new cryptographic entropy service. An invalid destination is `-EFAULT`; other flag variants are refused and logged |
| `readlink` (89) | The VFS has no links: `-EINVAL` for an existing path, the lookup error for a missing one |
| `munmap` (11) | Takes the range out of the regions and frees its pages. A range with no region is 0; one with memory that is not a region is `-EINVAL` |
| `mremap` (25) | Inside one region: shrinks in place, grows in place when the pages after it are free, otherwise moves with `MREMAP_MAYMOVE` (`-ENOMEM` without). Outside one region `-EFAULT`. `MREMAP_FIXED` and `MREMAP_DONTUNMAP` are refused |
| `brk` (12) | The break starts at the end of the image (`VmSpace::image_end`); growing maps a region exactly after it, shrinking unmaps. It answers where the break is, the old place when it could not move, as Linux's does |
| `arch_prctl` (158) | `ARCH_SET_FS` sets the thread's FS base, musl's thread pointer; an address outside the user half is `-EPERM`. Other codes are refused |
| `set_tid_address` (218) | Keeps the pointer and answers the thread id, which is the process id: a Linux process has one thread until `clone` (L7) |
| `clock_gettime` (228) | What musl calls with no vDSO, for `clock_gettime` and `time()`. `CLOCK_REALTIME` is the RTC's date carried by the monotonic clock in nanoseconds (`rtc::epoch_ns`); `CLOCK_MONOTONIC` is `timer::monotonic_ns`. The other clocks Linux has are refused |
| `nanosleep` (35) | A wait on the thread's timer (`process::sleep_via_timer`). The kernel keeps deadlines in milliseconds, so the wait is rounded up, plus one: never shorter than asked. Nothing interrupts it, so the remaining time is never written |
| `exit` (60) | Ends the calling thread with `status & 0xff`. The last thread ends the process, which with one thread is always |
| `exit_group` (231) | Ends the process with `status & 0xff` |

**The error numbers.** The kernel's own errors (`sx_status`) use Linux's
numbers, so a kernel result reaches the program unchanged. That matters when
the layer never sees the result: a write that blocked is finished by whoever
wakes the thread, and that code writes the kernel's value. A `static_assert`
in `linux_abi.hpp` fixes every shared number, so renumbering one breaks the
build instead of a Linux program.

## How it is tested

**The trap for other subsystems.** `nativetest` spawns eight children at once
that execute `syscall` with RAX set to 1, Linux's `write`. Each must end with
status 128 + 6, and none may write. It runs on 1, 2 and 4 virtual CPUs
(`./build.sh smoke native-test --smp N`).

**L1 and L2, `linux-hello`.** `linuxtest` (`subsystems/linux/tests/linuxtest.c`), a
SUN parent, runs each fixture through the launcher with standard output on
a pipe. It compares the bytes and the exit status with what Linux gave for
the same files, recorded by `subsystems/linux/profile/profile.sh`:

| Fixture | What it shows |
| --- | --- |
| `hello` | `hello`, status 7 |
| `startup` | argv, the environment from `-e`, all registers zero at entry, RSP aligned, the thirteen auxiliary entries right, two million `write` calls with every preserved register intact (including XMM0, XMM7 and XMM15), RCX and R11 as `syscall` leaves them, status 3 from `exit_group` |
| `errors` | `-EFAULT`, a zero-byte write from a bad address, `-EBADF`, `-ENOSYS` for a number Linux lacks, and `-ENOSYS` from `int 0x80` |
| `regions` | Anonymous memory at its edges, libc-free: a hole punched in touched pages, `MAP_FIXED` over touched pages reading zero again, one `munmap` across pieces and a hole, the kernel reading an untouched page and refusing a `PROT_NONE` one, `mremap` in place, blocked and moving, shrinking, `brk` up, down and up again, and the argument errors. It prints values, not verdicts: Linux's output is the reference |
| `args` | Static musl: argv and the environment through musl's start-up, `getenv` |
| `tls` | Thread-local data of the main thread: initialized, zeroed and 64-byte aligned, written, and `%fs:0` pointing at itself |
| `heap` | `malloc` from one byte to 6 MiB, three rounds, and `realloc` to 4 MiB: `brk`, a guard page `MAP_FIXED` over the break, `mmap`, `munmap` and `mremap` moving. Then `linuxtest` checks that free memory came back within 1 MiB: an unmapped page lost would not come back even when the program exits |
| `clock` | Both clocks, `time()`, and a 20 ms `nanosleep` that the monotonic clock sees last at least that long |
| `files write`, `check`, `remove` | In `/work`: create a file, `write`, `writev`, close, then `fstat`, `lseek` from the start, the end and the current offset, a read in the middle and a short one at the end, end of file, `-EBADF` writing a read-only descriptor and reading a closed one, `stat`, `-ENOENT`, `-ENOTTY` from `tcgetattr` and `isatty`; read back; `unlink` and `stat` after it |

Then it asks the kernel to start what it must refuse, and checks that the
process count did not change: a bad header, an image cut after its ELF
header, another machine, a missing file, a path climbing out of the guest
tree, a relative path, a missing handle, an unreadable subsystem block or
environment, and an unknown subsystem.

**L2, `linux-restart`.** A file a Linux program wrote is read back after a
restart of the machine. `linuxtest --restart` runs `files write` on the
first boot, then creates a marker file and flushes it, which syncs the whole
volume, and logs `LINUX RESTART READY`. The host's callback
(`tools/linux_restart_smoke.py`) resets the machine over QMP. On the second
boot the file is there, so `linuxtest` runs `files check` and `files remove`.

The fixtures are built by `subsystems/linux/Linux.cmake` with the same
commands `profile.sh` uses, and `linux-fixture-hash-test` checks that they are
the files the references came from. The static musl ones link against the
pinned `musl-dev` of the profile's Alpine root, which `profile.sh setup`
fetches by hash; `SAVANXP_LINUX_MUSL_ROOT` points at it
(`build/linux-profile/root` by default). Without it they are not built, and
`linuxtest` reports each as skipped. The damaged images are made from `hello`
by `subsystems/linux/profile/damage.py`. Test images get all of them, with
the references, under `/system/linux/fixtures`.

**In `smp-stats`.** A trapped call is tagged `linux#N` and counted at the
`syscall` door, not at the fault door it came in by
([SMP_ROADMAP.md](../roadmaps/open/SMP_ROADMAP.md#reading-smp-stats)). That is
what the trap-cost measurement of L3 reads. Under TCG, `startup`'s loop
already shows about 465,000 `write` calls per 2-second window holding the
lock 1.1 s of it; TCG is not the measurement, KVM is.

**Executable in the image.** A Linux executable has no suffix to tell it
by. `tools/sxfs_sync.py` gives mode 0755 to every file under `system/linux/`
that is executable on the build host, which is how the build leaves the
fixtures. The references next to them stay 0644.

**L3, `linux-tools`.** The unmodified Alpine BusyBox static executable from
L0 is checked by SHA-256 before staging, and `linuxtest --tools` compares
`echo`, `cat`, `ls -1`, `pwd`, `wc` and `mkdir -p` against Linux's bytes and
exit codes. Standard error is captured too, including `cat` on a missing
path. No shell is part of this profile. The prepared tree and listing flags
avoid unstable inode, timestamp, owner and ordering fields; no output is
normalized. `pwd` has its own Linux reference taken at `/`, SSL's initial
working directory, rather than L0's `/work`.

The static musl `tools` fixture checks relative paths, root confinement,
`getcwd` with an undersized buffer, descriptor flags, a non-directory,
`getdents64` with a short buffer followed by retry and EOF, the creation mask
and directory search denied by permissions. The denial runs through a launcher
spawned as uid 1000; the system identity is tested separately, with Linux's
`CAP_DAC_OVERRIDE` as its reference. A 20,000-byte `sendfile` transfer exceeds
the pipe capacity and compares the payload and final byte count. References
are produced by `profile.sh trace`. Missing BusyBox makes `linux-tools` fail explicitly.

**KVM measurement (2026-11-15).** `./build.sh smoke linux-hello --accel kvm`
on the Ryzen 5 1600, with two virtual CPUs and QEMU's host CPU model, passed
the two-million-call register check. In a 2,000 ms statistics window,
`smp-stats` reported `linux#1=203/18/803083`: 803,083 zero-byte writes,
203 ms holding the kernel lock (about 0.253 microseconds per call), maximum
18 microseconds. The observed window rate was about 402,000 calls/second.
The held time excludes entry, lock waiting and return; neither figure is a
round-trip latency guarantee. The fast entry remains deferred until a named
workload needs it. The `linux-tools` gate also passes with `--accel kvm`.

**L4, dynamic programs.** `linux-dynamic` runs the pinned, unchanged Alpine
`jq` against `libjq.so.1`, `libonig.so.5` and musl, checks its JSON/regex result
against Linux, and runs the dynamic `dynamic`/`libl4tls.so` fixture. It covers
initialized, zeroed and aligned TLS in a shared library, private writes,
fixed file replacement, partial-page zero fill, inaccessible pages beyond
EOF, protection changes retaining data, executable file pages, close/unlink
lifetime, `getrandom`, `readlink`, and 320 simultaneous private file maps with
separate anonymous guards. After exit, free memory must return within 1 MiB.
The fixture is built and hashed with the same commands as `profile.sh`.

Platform-policy checks separately assert that `mprotect` refuses W+X and that
CPU writes to read-only pages and execution of NX pages kill the process.
Until L7 provides signals, a protection page fault exits with 128 + 14 rather
than Linux's SIGSEGV status. Damaged dynamic fixtures test duplicate, nested
and unterminated `PT_INTERP`, including the absence of leaked processes.

`linux-dynamic-missing-lib` removes `libonig` from a disposable image and
compares musl's entire output and status with Linux's pinned failure reference.
`linux-dynamic-missing-interp` removes the interpreter and asserts spawn's
`ENOENT`, no leaked process, the launcher's 127 exit status, and the same
`No such file or directory` error description as Linux. Only that case removes
the diagnostic wrapper prefix: the Linux reference uses BusyBox `chroot`,
whereas SSL uses the SUN launcher. No musl diagnostic is normalized.

All guest runtime files are test-app staging inputs, validated by SHA-256.
They are not SavanXP shared libraries. Explicit file-map offsets are page
aligned and do not change the descriptor offset; shared maps and growing a
file snapshot with `mremap` remain unsupported. See
[MEMORY_REGIONS.md](../kernel/MEMORY_REGIONS.md) for the eager snapshot's
coherence and EOF contract.

## L5: local streams and the X11 service

The L5 implementation is present; its final regression verification is pending.
The unchanged `xclient` fixture has SHA-256
`37fa1659c4ae26d05874e1ee5fa0b5805cece69bac98de49f944ed9592c9d5b8`.
It uses the pinned Alpine musl and six original libraries: X11, xcb, Xau,
Xdmcp, bsd and md. Only test images stage this closure. The normal build
preserves externally installed Linux programs and resources.

### Stream ABI

A socket is an SSL descriptor with a write handle and a read handle from
`process::connect_port`, the neutral mechanism also used by SUN. Ordinary
file descriptors still name one handle. The descriptor reserves its number
before connection and closes both handles on close or process exit. SSL does
not call the native or POSIX dispatchers.

| Call | L5 forms |
| --- | --- |
| `socket` (41) | `AF_UNIX`, `SOCK_STREAM`, protocol 0, optional `SOCK_CLOEXEC` and `SOCK_NONBLOCK`; other families/protocols are refused |
| `connect` (42) | `/tmp/.X11-unix/X0` and `X1` map to native ports `x11d.0` and `x11d.1`; abstract addresses return `ECONNREFUSED`, so libxcb retries the pathname |
| `getpeername` (52) | The connected Unix address, with Linux's truncated-output and actual-length convention |
| `getsockopt` (55) | `SOL_SOCKET`: `SO_SNDBUF` reports the actual 8192-byte output pipe, `SO_TYPE` is stream, `SO_ERROR` is 0 for this synchronous connection; other options are refused |
| `fcntl` (72) | Adds `F_GETFL` and `F_SETFL` for `O_NONBLOCK`, keeping descriptor-local `FD_CLOEXEC` separate; both socket pipes follow nonblocking mode |
| `poll` (7) | Up to 256 copied `pollfd` entries, read/write readiness, hangup, errors, invalid and negative descriptors, and finite/infinite waits; parked calls are rechecked by SSL's wait hook |
| `recvmsg` (47), `recvfrom` (45) | Stream bytes with flags 0 or `MSG_DONTWAIT`, short reads, EOF and `EAGAIN`; vectors are validated before consuming input. No ancillary messages are supplied. A blocking vector read retries after the input pipe wakes |
| `shutdown` (48) | Read, write or both halves; closing the write pipe supplies EOF to the server |
| `access` (21) | Guest-root paths with the VFS's search and requested permission checks |
| `uname` (63) | Linux `utsname` layout: `Linux`, host `savanxp`, the SavanXP version with `-ssl`, and machine `x86_64` |

`read`, `write` and `writev` use the corresponding socket pipe. The send
buffer is bounded, so libxcb may try `setsockopt` (54) to enlarge it; that
call remains explicitly unimplemented (`ENOSYS`, logged once), and this
profile continues with the actual buffer. Listening, descriptor passing,
other address families and abstract-name listeners are outside L5.

### Service lifetime and trust

For `DISPLAY=:0`, `:0.0`, `:1` or `:1.0`, the SUN launcher starts
`/system/x11d.sxe` on demand. There is no X service on the boot path. A
readiness pipe and an acknowledged SUN lease keep the listener alive between
startup and the Linux client's connection. Racing launchers share the named
listener. Each lease or X connection has a SUN worker; an X worker acquires
its own `windowd` connection after receiving its first setup bytes. A probe
that supplies no setup creates no surface. The service reaps its workers
and releases its port 500 ms after the last worker exits; its initial idle
grace is five seconds. A launcher never terminates another client's service.

The ports are ordinary user ports because a SUN launcher must also work as
an ordinary user. SSL accepts a listener owned by the same effective uid or
by the system; another user's listener is `EACCES`. X authorization data is
not supported. This is a local desktop profile, not a network X server.

### Protocol and drawing

`subsystems/linux/x11/x11.c` is portable C, shared by the SUN adapter and the
host TCP adapter. It accepts little-endian X11 protocol 11 clients, bounded
64 KiB requests and fragmented streams. Invalid lengths, resource IDs,
capacity and unsupported requests produce core X errors; an unresynchronizable
or truncated stream fails the connection. There are 16 window slots, 32 GCs
and 32 dynamic atoms per connection. Every extension is reported absent.

L5 implements `CreateWindow`, `ChangeWindowAttributes`, `MapWindow`,
`DestroyWindow`, `InternAtom`, the root's absent `RESOURCE_MANAGER` query,
`WM_NAME` and `WM_PROTOCOLS`, `CreateGC`, `FreeGC`, filled rectangles and full
ellipses, `QueryPointer`, `GetInputFocus` and `NoOperation`. The platform
painter fills X11 ellipse spans using X11's edge convention; its ordinary
ellipse primitive has a different convention and is unchanged. Resize sends
`ConfigureNotify` followed by `Expose`; the fixture repaints at its new size.
The caption's close request sends `WM_DELETE_WINDOW`; without that protocol
the connection ends. Disconnect frees the connection's windows and bitmaps.

The first desktop adapter supports one mapped top-level window per X client,
with up to eight workers including launch leases. A second top-level or a
child mapping is refused with `BadAlloc`, never drawn into the wrong surface.
The portable engine keeps the tree, but the extra child-window and pixmap
rendering needed by Xeyes is L6. This avoids changing windowd's one-main-window
per-process policy before a measured consumer requires it.

### Verification gates

`linux-x11-protocol-test` feeds the real engine with split headers and bodies,
truncated setup and requests, bad lengths and masks, duplicate IDs, exhausted
resources, sequence wrap, resize and disconnect. `linux-x11-xlib-test` runs
the unchanged pinned ELF through its original interpreter on Linux against
the engine's TCP adapter. It compares the window pixels with L0's Linux
capture, follows the recorded pointer path, resizes and closes cleanly.
Without the pinned Alpine root it is reported as skipped.

`linux-transport-reference-test` runs the static musl `transport` fixture on
Linux against a real Unix listener. Before starting the desktop, `linux-xclient`
runs the same fixture over a SUN port: descriptor flags, socket status and
peer address, invalid buffers without consumed input, vector receives,
nonblocking reads, poll timeout and invalid entries, half-close and EOF.

`linux-xclient` runs the Xlib ELF through the SUN launcher inside QEMU. Its host
callback compares every content pixel with the Linux capture, verifies root
and local pointer coordinates along seven positions, drags the frame to
260x180, verifies the repaint and clicks the caption's close button. The
resized reference relocates the same Linux-rasterized shapes as the fixture
does. The SUN parent checks exit status, the expected output, disappearance
of the listener, the original process and desktop-handle counts, and reclaimed
memory. Captures and logs stay under `build/smoke-logs/`. `tools/shoot.sh`
also exercises the client in the normal desktop session.
