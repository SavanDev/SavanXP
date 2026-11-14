# i386 descriptors and the POSIX base

This milestone adapts the v0.4.0 process I/O model to the existing protected-mode
scheduler. It restores the reference console shell and its BusyBox subset,
without claiming full v0.4.0 parity. The [roadmap](I386_PORT.md) defines the
remaining dependencies; [the study](I386_PORT_STUDY.md) records the decisions
and the failures that motivated them.

## Descriptor ownership

Each process has 64 `process::HandleEntry` slots. A slot retains a shared
`object::IoObject`, carries granted access and flags, and is allocated from the
lowest free descriptor. The shared types live in `include/kernel/io_types.hpp`
and `include/kernel/object.hpp`; `kernel/object_lifetime.cpp` contains the
reference retain/release/type-check operations, extracted without changing their
semantics. The i386 adapter supplies process-memory copies and scheduler/device
integration in `arch/x86/io.cpp`.

An independent open creates another I/O object. `dup` and `dup2` retain the same
object, sharing offsets, directory iteration and nonblocking state. Root init
receives separate read/write/write TTY objects at descriptors 0/1/2. Normal
`spawn` inherits those three entries only, including the shared objects behind
redirection; unrelated descriptors do not leak into children. `SPAWN_FD` selects
the parent's input/output entries using ECX/EBX for arguments four/five on i386.
The SDK's existing `spawn_fd`/`spawn_fds` convenience functions also work.

Exit, a user exception and termination through `kill` release all descriptors.
A zombie keeps its wait status and slot, not its file or pipe references. The
single GPU client is also a shared I/O object: only its holders can operate it,
and its last close releases console suppression. A GPU handle outside the three
standard channels does not pass to a spawned child.

The reference bounds remain 256 I/O objects and 64 pipes globally. A full process
descriptor table returns `EBADF`, matching the reference; exhausting the object
or pipe pool returns `ENOMEM`. Partial pipe construction rolls back both objects,
its descriptors and the ring reservation. Installed objects are never discarded
without releasing their references.

## Files, directories and paths

Regular-file read/write, seek, stat/fstat, directory iteration, mkdir/rmdir,
unlink, truncate, rename, chdir/getcwd and sync use the existing VFS/SxFS modules.
The persistent format and additive image synchronization remain unchanged.
Paths normalize relative to the process CWD, which spawn inherits and exec
preserves. Directory descriptors hold their own shared iteration position.
Open objects prevent unlink/rmdir and directory rename according to the
reference's `EBUSY` policy.

Syscalls validate every user page and require writable pages for destination
buffers. Invalid pointers return `EFAULT`; read/write access mismatches return
`EBADF`. Copies to or from an inactive task walk its physical aliases and split
at 4 KiB boundaries. This includes blocked I/O completion, not just active
syscalls. No user mapping can currently change concurrently with a blocked
operation in the single-threaded, single-CPU process model.

`seek` uses the signed 32-bit SDK `long` range and rejects negative/overflowing
positions. Append opens start at the current file end, as in v0.4.0; this is not
an additional seek-proof append guarantee. File I/O retains the reference's
256-byte scratch-copy path and SxFS journal behavior. Large writes can be slow
under TCG because each growing chunk commits metadata; the port does not change
the filesystem's persistence algorithm to improve a test's elapsed time.

## Pipes, polling and scheduling

A byte pipe has an 8192-byte ring and reader/writer endpoint counts. Duplicate or
inherited descriptors retain their endpoint object; the endpoint count falls
only on its last release. Reads block on an empty pipe with writers and return
EOF after the final writer closes. Writes block when full and return `EPIPE`
without readers. Nonblocking empty reads return `EAGAIN`; nonblocking writes no
larger than the ring require room for the whole record before changing it.
Larger nonblocking writes may return partial progress.

The process stores a blocked operation's descriptor, user address, size and write
progress. The PIT and scheduler boundaries retry it through physical aliases,
complete its saved frame and make it runnable. No kernel C continuation sleeps
on its stack. `poll` stores up to 64 entries and checks readability/writability,
EOF/HUP, missing readers/ERR, invalid descriptors/NVAL and deadlines. A negative
descriptor is ignored; a negative timeout has no deadline. `fcntl` shares the
reference get/set nonblocking operations across duplicated descriptors.

## Console and shell

Normal commands boot into the reference `sh` source and shared `shell_core`,
launched/restarted by `init32` after `sysinfo`. The shell retains its parser,
prompt, builtins, command lookup, pipelines and redirection. Its eleven BusyBox
applets are `cat`, `cp`, `echo`, `false`, `ls`, `mkdir`, `mv`, `ps`, `rm`, `sleep`
and `true`, using the original `vendor/busybox-port` sources. This is the same
subset as v0.4.0, not an unmodified full BusyBox/ash environment. Core tools such
as uname, df, rmdir, truncate, sync and errtest also build as static ELF32.

The canonical console uses the reference pending-line discipline: editing and
echo occur in the kernel, Enter makes a line ready, and a read consumes the line
without its newline. Ctrl+L clears it. The prototype's raw-event clients remain
available through `/dev/input0`; `sh32` and `keyecho` explicitly replace stdin
with that stream. Active raw stdin/read/poll consumers suppress TTY draining so
canonical input does not steal their events. This is a compatibility bridge for
the diagnostics and minimal desktop, not terminal job control or process groups.
Input layout changes require a write-capable descriptor. The ISO angle-bracket
key supplies `<`/`>` for shell redirection with the default ES keyboard map.

Keyboard IRQ diagnostics still reach COM1 but do not paint or scroll the visible
console. This prevents trace rendering from delaying PIT delivery and filling
the raw-event queue while a long command is typed. The shell session and normal
kernel/user output remain visible.

A failed pipeline closes all owned endpoints/redirection descriptors, terminates
already-launched children and waits for them before showing another prompt.
This also handles capacity failure after some stages have started. The small
process table can still reject a pipeline requiring more live tasks than fit;
recovery does not claim increased process capacity.

Static `exec` first loads a replacement into a separate directory. Failure
releases that staging image and leaves the original intact. Success switches
through the supervisor directory, frees the old image, commits the new mappings
and user frame, and starts with fresh FPU/BSS state. PID, parent, CWD and open
descriptors survive. Supported kill dispositions terminate with `128 + signal`;
SIGCHLD is non-terminating. User-installed signal handlers and fork remain gaps.

## Read-only root and persistent disk

GRUB loads `initramfs.cpio` as a Multiboot2 module. The existing CPIO reader
supplies `/bin` and `/README`, while `/disk` stays on the separate persistent
SxFS image. The physical allocator excludes the module's memory. Only CPIO
staging copies have debug symbols stripped with LLVM objcopy, keeping the archive
small enough for 32 MiB machines; disk binaries retain their build bytes.
The current kernel starts disk init only when SxFS is mounted, so a diskless ISO
boot is still a kernel/CPIO check rather than a full live session.

## Verification and remaining boundaries

```sh
./build.sh build
./build.sh smoke posix
./build.sh smoke
./build.sh smoke memory
./build.sh test
python3 tools/verify_posix_persistence.py
./tools/verify_doom_persistence.sh --jobs 4
```

`posixtest32` checks shared and independent offsets, rights and pointer failures,
paths and directory ownership, inherited channels, failed/successful exec,
nonblocking/wrapped pipes, polling and a 24 KiB producer/copy/reader pipeline.
Ninety-six faulting children exercise descriptor/pipe cleanup; repeated full-table
pipe failures and 300 pipe reuse cycles exercise allocation rollback. It also
runs every BusyBox applet and checks copied bytes. The host types pipelines,
redirection, CWD operations, exec and shell exit/restart, and audits live
processes/descriptors and the exact free-page baseline after missing-command and
capacity failures. The regression calls `SPAWN_FD` directly to verify ECX/EBX
argument mapping and invalid-descriptor rollback. Existing fdtest, waittest,
pipestress, seektest, renametest and truncatetest run as actual ELF32 programs.

Normal smoke runs use disk snapshots. `verify_posix_persistence.py` alone creates
a disposable copy, allows guest writes to it, rebuilds that copy through normal
`build.sh build`, cold-boots it and verifies exact file bytes. It checks the
source image hash too. Its writable-disk harness flag is for this isolated check;
it is never enabled by normal smoke commands.

Process-info CPU counters and an explicit idle-process entry still need the
reference accounting integration. The six-process table, 16 MiB user window,
64 KiB eager stack, 1 GiB managed RAM limit and lack of NX remain. Fork, full signals/job control, section objects,
shared libraries, the reference full desktop and the later device services are
not established by these tests. The next milestone is memory objects and ELF32
shared-library semantics.
