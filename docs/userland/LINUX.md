# SSL: the Savan Subsystem for Linux, as built

What exists of the Linux subsystem and how it works. The plan, the paths
rejected and the phases still ahead are in the
[roadmap](../roadmaps/open/LINUX_SUBSYSTEM_ROADMAP.md); the measured scope of
the first profile is in [LINUX_XEYES_PROFILE.md](LINUX_XEYES_PROFILE.md).

**Built so far: E1, the `syscall` trap; L1 with S1, a static, libc-free
Linux executable runs unchanged, started by the `linux` launcher; and the
memory half of L2 with S2a: a static musl program starts, uses its
thread-local data and its heap, and prints.** Files and clocks are the rest
of L2. Every call not below answers `-ENOSYS`.

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
  (3). Anything else, including a SavanXP image, is `-ENOEXEC`, and so is an
  image that names an interpreter (`PT_INTERP`) until the kernel loads one
  (L4). The selector is the only thing that makes a process Linux. Nothing is
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
| `AT_BASE` | 0: no interpreter yet |
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
one at `0x200000`. A static PIE (`ET_DYN`) goes at `0x400000`, as any other.

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
| Per-process state | Two bitmaps of the numbers already reported, as unimplemented and as asked for something unsupported (512 bits each); the program break |
| Per-thread state | `set_tid_address`'s pointer |

A call SSL has, asked for a variant it does not offer (a shared mapping, a
file mapping, an `arch_prctl` code other than `ARCH_SET_FS`), fails with its
Linux error and is logged once per call per process:
`linux: pid=N syscall M: unsupported WHAT`.

Linux numbers and error values are written in `linux_abi.hpp` from the Linux
ABI, under `ssl::abi`. Nothing there comes from the POSIX SDK.

### The calls

| Call | What it does |
| --- | --- |
| `write` (1) | Descriptors 0, 1 and 2 are the process's handles of the same numbers, the ones the spawn gave. Any other is `-EBADF`, until the descriptor table of L2. An unreadable buffer is `-EFAULT`; zero bytes is 0 whatever the buffer. The write is the native one (`process::native_write_handle`), so a full pipe parks the thread and the reader's progress finishes the call |
| `writev` (20) | The same descriptors. The vector is read twice: every length first (`-EINVAL` past `SSIZE_MAX`, more than 1024 entries), then each segment is written. Only the first segment with bytes may block, since the waker completes the call with that segment's count; a later one that would block ends the call short, and the caller writes the rest, as musl's stdio does. A count is capped at `MAX_RW_COUNT`, as is `write`'s |
| `ioctl` (16) | `-ENOTTY` on a standard descriptor, whatever the request: no descriptor is a terminal yet. That is Linux's answer for a file or a pipe to `TCGETS` and `TIOCGWINSZ`, the two musl asks. `-EBADF` otherwise |
| `mmap` (9) | Private anonymous memory, an anonymous region of S2a ([MEMORY_REGIONS.md](../kernel/MEMORY_REGIONS.md)). `MAP_FIXED` replaces the regions in the range; `MAP_FIXED_NOREPLACE` is `-EEXIST` over anything. A hint is taken when free. `MAP_DENYWRITE`, `MAP_EXECUTABLE`, `MAP_NORESERVE` and `MAP_STACK` change nothing here, as on Linux; another flag is refused. A shared mapping is `-EINVAL`, a file mapping `-ENODEV` (S2b, L4), writable and executable memory `-EACCES` (W^X, a recorded difference from Linux), and a fixed mapping over memory that is not a region `-EINVAL` |
| `munmap` (11) | Takes the range out of the regions and frees its pages. A range with no region is 0; one with memory that is not a region is `-EINVAL` |
| `mremap` (25) | Inside one region: shrinks in place, grows in place when the pages after it are free, otherwise moves with `MREMAP_MAYMOVE` (`-ENOMEM` without). Outside one region `-EFAULT`. `MREMAP_FIXED` and `MREMAP_DONTUNMAP` are refused |
| `brk` (12) | The break starts at the end of the image (`VmSpace::image_end`); growing maps a region exactly after it, shrinking unmaps. It answers where the break is, the old place when it could not move, as Linux's does |
| `arch_prctl` (158) | `ARCH_SET_FS` sets the thread's FS base, musl's thread pointer; an address outside the user half is `-EPERM`. Other codes are refused |
| `set_tid_address` (218) | Keeps the pointer and answers the thread id, which is the process id: a Linux process has one thread until `clone` (L7) |
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

Then it asks the kernel to start what it must refuse, and checks that the
process count did not change: a bad header, an image cut after its ELF
header, another machine, a missing file, a path climbing out of the guest
tree, a relative path, a missing handle, an unreadable subsystem block or
environment, and an unknown subsystem.

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
