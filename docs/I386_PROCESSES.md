# i386 processes and preemptive scheduling

The i386 target supports independent ELF32 processes, PIT-driven preemption,
`spawn`, `waitpid`, `exit`, `yield`, blocking keyboard reads and sleeping. This
extends the [real-hardware route](REAL_HARDWARE.md) after its minimal desktop
milestone. It does not reuse the x86_64 scheduler or promise that target's full
POSIX/process feature set.

## 1. Context, stacks and memory

`arch/x86/context.hpp` defines the saved IRQ/syscall frame: GS, FS, ES, DS, all
`pusha` registers, the vector, EIP, CS and EFLAGS, followed by ESP/SS for ring 3.
Assembly saves the interrupted registers before loading kernel selectors,
aligns a separate C call area, and restores the frame returned by the handler.
There is no dependency on a frame guessed from TSS.esp0. Ring 0 IRQs have no
hardware ESP/SS pair, so their frames are retained by pointer rather than copied
as full ring 3 frames.

Each process owns a 16 KiB kernel stack, a saved return frame and an aligned
512-byte FXSAVE area. Switching eagerly saves/restores x87, all eight SSE
registers, the x87 control word and MXCSR. A new process starts with empty x87
tags, control word `0x037f`, MXCSR `0x1f80` and zeroed register storage. FS/GS and
EFLAGS are independent too. Kernel C++ is compiled without SSE; the explicit
context helpers are the only kernel users of these registers.

ELF32 programs retain the virtual range `0x08000000..0x09000000`, with a 64 KiB
initial stack at its top. Each process has a private CR3 directory mapping that
range to different physical backing memory. Kernel, framebuffer and physical
backing aliases remain supervisor-only. Loading copies into the new process's
physical alias without replacing the caller's active mapping. Image segments
must leave room for the stack; BSS, unused memory and the stack are zeroed on
reuse. The linker rejects a kernel whose static storage reaches the user range.

Until there is an i386 physical allocator, six slots reserve 96 MiB of static
image backing in total. Exhaustion returns `EAGAIN`; it does not evict another
process. Each kernel stack has a bottom canary; the context, loader, process and
syscall sources also reject compiler frames larger than 4096 bytes. These checks
help catch Debug aggregate temporaries exceeding the stack budget.

## 2. Scheduler

The PIT remains at 100 Hz. The scheduler rotates runnable processes every two
ticks, giving a 20 ms quantum in delivered PIT time. A CPU-bound process is
preempted even without `yield` or `sleep`. User execution is preemptible; kernel
execution is atomic with respect to task switching. Long kernel work can delay
a tick, so this is not a real-time scheduling guarantee.

Processes move between running, runnable, sleeping, reading, waiting and zombie
states. A blocking syscall stores its return frame and request, then returns a
different frame to assembly; it does not retain a sleeping C++ continuation.
PIT delivery wakes deadlines and completes pending keyboard reads. The keyboard
queue is a shared console stream, not reset by spawning a child. If all tasks
are blocked, a separate kernel idle stack/frame executes `sti; hlt` until the
next IRQ. IRQs interrupting kernel code return to that code; the scheduler only
switches at ring 3 boundaries or from idle.

## 3. Process lifecycle and userland policy

The kernel starts `/disk/bin/init32` as PID 1. `init32` launches `sysinfo`, waits
for it, then launches `sh32` and restarts the shell when it exits. No kernel
`EXIT` stage chooses the next binary. Shell commands are:

- `run PATH [ARGS]`: spawn a child and wait for its exit status.
- `start PATH [ARGS]`: spawn a child, print its PID and return to the prompt.
- `wait PID`: collect a background child's status.
- `desktop`: run `/disk/bin/desktop32` as a foreground child.
- `multitest`: run the process regression, then return to the shell.

`spawn` copies and validates the path, argument table and strings before loading.
The SDK limit is 15 arguments of up to 63 characters each; the initial argv table
uses 4-byte pointers and a null terminator. Invalid user ranges return `EFAULT`,
excess arguments or strings return `E2BIG`, and a missing or invalid image returns
`ENOENT` or `ENOEXEC`. The supported image format remains static i386 ET_EXEC.

`waitpid` accepts a specific child or `SAVANXP_WAIT_ANY` (`-1`). It returns the
child's PID and its plain integer exit status, including when the child exited
before the call. A null status pointer is allowed. A nonexistent, unrelated or
already-collected child returns `ECHILD`. Zombies retain their slot until
collected. When a parent exits, live orphans become detached and are reclaimed
on exit; existing orphan zombies are reclaimed immediately. PIDs increase rather
than being reused with slots, and stop allocating before the signed PID range
would overflow.

The GPU stays a single-client device. Its descriptor belongs to its opener PID;
children do not inherit it. Exit releases video ownership only for the owning
process. Standard input/output remain shared console channels. This is not a
general descriptor table, process-group model or terminal job-control layer.

## Verification

Build both the disk image and the ISO before checking the running guest:

```sh
SAVANXP_ARCH=i386 SAVANXP_BUILD_DIR=build/i386-linux SAVANXP_OUTPUT_ROOT=build/i386 \
  ./build.sh build
SAVANXP_ARCH=i386 SAVANXP_BUILD_DIR=build/i386-linux SAVANXP_OUTPUT_ROOT=build/i386 \
  ./build.sh iso
python3 tools/legacy_boot_test.py --iso build/i386/SavanXP.iso \
  --disk-image build/i386/disk-i386.img --expect-processes \
  --expect-desktop build/i386-linux/desktop32 --artifacts /tmp/savanxp-i386-processes
```

`multitest32` starts two instances at the same virtual addresses. Its assembly
probe keeps distinct general registers, segment selectors, carry/direction
flags, stack canaries, x87 values/control words, all eight XMM registers and
MXCSR patterns. It uses uptime queries but never yields or sleeps in the CPU
loop. The host requires at least 200 ms of overlap and at least three recorded
PIT preemptions per worker, as well as successful pattern checks. A serial
`MULTITASK PASS` alone is insufficient.

The same run checks specific/any-child waits, argument boundaries and bad
pointers, capacity exhaustion, sleeping wakeups, repeated slot reuse, null
status pointers, orphan cleanup, process counts and GPU ownership on exit.
The host also types `start`, `wait`, `run` and `exit`, checking background
collection, foreground status and shell restart by init32. The desktop check then proves that shell blocking, console clearing, graphics,
mouse movement/buttons and cursor restoration still work. The harness uses a
snapshot of the disk and retains serial logs/screenshots with `--artifacts`.

## Remaining boundaries

There is no fork, exec replacement, signal delivery, SMP, dynamic image loading,
ASLR, per-page permission policy or recoverable user-fault teardown. Exceptions
still report and halt the guest. PSE maps each user image in writable 4 MiB pages,
without NX on the legacy CPU. Private address spaces prevent processes from
sharing their images/stacks accidentally, but do not constitute the x86_64
security model. Allocator-backed memory, finer page protection and fault-local
termination are follow-up work; physical laptop validation is still outstanding.
