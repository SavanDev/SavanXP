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

ELF32 programs retain the virtual range `0x08000000..0x09000000`, with an eager
64 KiB stack at its top and an absent 4 KiB guard immediately below it. Private
CR3 directories use two-level non-PAE paging for user mappings. Only PT_LOAD
pages and the stack exist: holes remain absent, text/rodata are read-only unless
an ELF segment sharing their page requires writes, and data/BSS/stack are writable.
There is no NX on this CPU; readable user pages are also executable.

The v0.4.0 physical allocator (`kernel/physical_memory.cpp`) supplies zeroed
4 KiB image pages and page tables on demand during loading, replacing the old
96 MiB static image reservation. The architecture adapter consumes Multiboot2
usable RAM below 1 GiB and excludes the kernel, boot information, modules and
framebuffer. Physical RAM has supervisor-only aliases at
`0x40000000..0x80000000`; kernel low identity mappings also remain supervisor-only.
Loading and blocked-task result copies use those aliases without activating the
destination CR3. Copies split at page boundaries because adjacent virtual pages
can have unrelated physical addresses. Kernel video identity mappings cannot
replace the RAM alias window; a framebuffer overlapping it is unsupported.

The six-slot process table, private directories and 16 KiB kernel stacks remain
bounded static metadata. A full table returns `EAGAIN`; physical exhaustion
returns `ENOMEM` and reclaims all pages from the partial load. Exit or fault
releases the user pages/tables after switching CR3; a zombie retains only its
slot and wait status. Fresh allocations are zeroed before reuse. `system_info`
reports allocator-managed total pages and current `memory_free_bytes`.

Each kernel stack has a bottom canary; the context, loader, process and syscall
sources also reject compiler frames larger than 4096 bytes. The linker rejects
a kernel whose static storage reaches the user range. These checks help catch
Debug aggregate temporaries exceeding the stack budget.

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
children do not inherit it. Normal exit and fault termination release video ownership only for the owning
process. Standard input/output remain shared console channels. This is not a
general descriptor table, process-group model or terminal job-control layer.

## 4. Fault containment

The exception stubs preserve registers before loading kernel selectors, clear DF
and normalize an error word for both classes of CPU exception. Their frame has
an extra error word compared with the IRQ/syscall frame; it is never resumed.
The handler checks the saved CS privilege level, reports vector/error/EIP/CS
and CR2 for page faults, then terminates a faulting user task with status
`128 + vector`, matching v0.4.0's `terminate_current_from_exception`. Waiters,
orphans, GPU ownership and memory follow the normal exit path. The scheduler
returns another task's valid IRQ/syscall frame to assembly.

Kernel exceptions remain fatal, as do NMI, double fault and machine check even
when they interrupt user code. Invalid syscall buffers return `EFAULT` before
the kernel touches them: validation walks every user page and requires write
permission for read/input, wait status, system-info and GPU-info destinations.
The single-CPU kernel does not schedule within a syscall, and user mappings
cannot currently change while a blocked operation retains a pointer.

## Verification

Build both the disk image and the ISO before checking the running guest:

```sh
./build.sh build
./build.sh smoke
./build.sh smoke memory  # 32 MiB RAM: mandatory ENOMEM rollback
# Or invoke the underlying harness after ./build.sh iso:
python3 tools/legacy_boot_test.py --iso build/SavanXP.iso \
  --disk-image build/disk.img --expect-processes \
  --expect-desktop build/linux-i386/desktop32 --artifacts /tmp/savanxp-i386-processes
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
It also checks #UD, divide error, privileged-instruction #GP and #PF from null,
absent, kernel, RAM-alias, read-only text and stack-guard accesses. A fault with
null data selectors and DF set exercises the exception entry. The host requires
the page-fault error bits and CR2 values, surviving siblings, successful waits,
GPU reacquisition and identical free-page counts after teardown. A `memhog32`
helper tests zeroed large BSS and wait-result copies crossing noncontiguous
physical pages at a page-table boundary. Repeated loads fill the table on the
normal machine or exhaust physical RAM on the 32 MiB machine; failed loads and
reaped children must return the exact baseline free-page count.

The host also types `start`, `wait`, `run` and `exit`, checking background
collection, fault status 134 followed by a working shell prompt, foreground
status and shell restart by init32. The desktop check then proves that shell blocking, console clearing, graphics,
mouse movement/buttons and cursor restoration still work. The harness uses a
snapshot of the disk and retains serial logs/screenshots with `--artifacts`.

## Remaining boundaries

There is no fork, exec replacement, signal delivery, SMP, dynamic image loading,
ASLR, section-object integration, heap growth or demand-grown reference stack.
The virtual window remains 16 MiB and the stack 64 KiB; RAM above the mapped first
GiB is not allocated. User read-only pages and supervisor mappings are protected,
but absence of NX prevents execute-disable and full W^X enforcement. These are
explicit parity gaps; the memory/fault milestone does not claim the complete
v0.4.0 memory model. Physical laptop validation is still outstanding.
