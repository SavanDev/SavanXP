# i386 processes and preemptive scheduling

The i386 target supports independent ELF32 processes, timer-driven preemption,
`spawn`, `fork`, `waitpid`, `exit`, `exec`, `kill`, `yield`, blocking I/O and sleeping. This
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

Each started CPU has its own architectural 104-byte protected-mode TSS and
eight-byte GDT descriptor. The I/O bitmap offset lies beyond the TSS limit,
denying direct user port access. Each scheduling CPU owns its current process
and pinned idle process; the reference ticket lock serializes kernel entries and is
released after changing stacks. All scheduling CPUs use local APIC timers.
The BSP can fall back to the PIT when the local APIC is unavailable.
See [I386_SMP.md](I386_SMP.md) for startup, remote termination and TLB rules.

Each process owns a 16 KiB kernel stack, a saved return frame and an aligned
512-byte FXSAVE area. Switching eagerly saves/restores x87, all eight SSE
registers, the x87 control word and MXCSR. A new process starts with empty x87
tags, control word `0x037f`, MXCSR `0x1f80` and zeroed register storage. FS/GS and
EFLAGS are independent too. Kernel C++ is compiled without SSE; the explicit
context helpers are the only kernel users of these registers.

ELF32 programs retain the image base `0x08000000` and stack top `0x0f000000`.
The user range now extends to `0x40000000` for section views. The stack reserves
1 MiB with eight initial pages and grows on user not-present faults; its
permanent guard is `0x0eeff000`. Private CR3 directories use two-level non-PAE
paging. Holes remain absent; text/rodata are read-only and data/BSS/stack writable.
Borrowed section pages have independent object/view references and are not freed
as private image pages. [I386_MEMORY.md](I386_MEMORY.md) defines ownership, fork,
heap growth and ELF32 loading. There is no NX: readable user pages are executable.

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

The reference 64-slot process table, private directories and 16 KiB kernel stacks remain
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

The scheduler rotates runnable processes every twenty ticks at 1000 Hz,
giving a nominal 20 ms quantum. The local APIC delivers periodic ticks on
each scheduling CPU. The BSP can fall back to the PIT. A CPU-bound process
is preempted even without `yield` or `sleep`. Interrupt gates keep kernel critical
sections atomic. A continuation inside the unlocked interrupt-wait helper can
be preempted by the timer, including an idle yield or a blocking network call;
it can resume only on its original CPU. Long critical work can still delay a
tick, so this is not a real-time scheduling guarantee.

Sleep, timer objects, polling and asynchronous receive deadlines use the
monotonic clock. The BSP checks those deadlines when it next handles a tick.

Processes move between running, runnable, sleeping, reading, writing, polling, child/object waiting and zombie
states. Asynchronous waits store their return frame/request and return a
different frame to assembly. Network connect/send waits can retain a C++
continuation, saved as a shorter ring-0 frame on the process kernel stack.
Timer delivery and scheduler boundaries service deadlines, blocked I/O and polling. The keyboard
queue is a shared console stream, not reset by spawning a child. When normal
tasks are blocked, each CPU runs its own ring-3 idle process. Idle yield halts
inside the kernel with the global lock released, attributing timer ticks to
that idle. BSP ticks service deadlines even when interrupting that wait; kernel
continuations can be suspended there and resume on their original CPU.
[I386_SMP.md](I386_SMP.md) defines the lock and kill lifetime rules.

## 3. Process lifecycle and userland policy

The kernel creates one idle per scheduling CPU before starting the reference
`/bin/init`, whose PID is recorded dynamically for supervision and orphan
adoption. Init and idle processes reject kill requests with `EACCES`. The
reference graphical restart policy is retained. Orphans are reparented to init for collection.
An explicit `console-init` diagnostic boot starts `/disk/bin/init32`, which
runs `sysinfo`, reaps orphan diagnostics and supervises the reference `sh`. CPIO supplies
`/bin` and `/README`; the persistent `/disk/bin` remains first in command lookup.
The [POSIX contract](I386_POSIX.md) describes canonical input, shared descriptors,
redirection, pipes and transactional `exec`. No kernel `EXIT` stage chooses the
next binary. `sh32` is a diagnostic child; its commands remain:

- `run PATH [ARGS]`: spawn a child and wait for its exit status.
- `start PATH [ARGS]`: spawn a child, print its PID and return to the prompt.
- `wait PID`: collect a background child's status.
- `desktop`: run `/disk/bin/desktop32` as a foreground child.
- `multitest`: run the process regression, then return to the shell.

`spawn` copies and validates the path, argument table and strings before loading.
The SDK limit is 15 arguments of up to 63 characters each; the initial argv table
uses 4-byte pointers and a null terminator. Invalid user ranges return `EFAULT`,
excess arguments or strings return `E2BIG`, and a missing or invalid image returns
`ENOENT` or `ENOEXEC`. The supported formats are i386 ET_EXEC and PIE ET_DYN with the embedded ELF32 loader.

`waitpid` accepts a specific child or `SAVANXP_WAIT_ANY` (`-1`). It returns the
child's PID and its plain integer exit status, including when the child exited
before the call. A null status pointer is allowed. A nonexistent, unrelated or
already-collected child returns `ECHILD`. Zombies retain their slot until
collected. When a parent exits, its children are reparented to the recorded init, which reaps
existing zombies and future exits. Without a surviving init, orphans are detached
and reclaimed automatically. PIDs increase rather
than being reused with slots, and stop allocating before the signed PID range
would overflow.

GPU descriptors use shared I/O objects. Queries can use independent opens;
presentation requires the single graphics-session owner. Normal spawn inherits
only descriptors 0, 1 and 2. Exit/fault releases ownership and imported surfaces;
see [I386_GRAPHICS.md](I386_GRAPHICS.md) for the reference device contract.
Input/output descriptors now follow the reference I/O model rather than permanent
numbered kernel channels. `sh32` and `keyecho` explicitly open `/dev/input0` and
replace their stdin to retain raw-event diagnostics; the reference shell reads
canonical TTY lines. [I386_POSIX.md](I386_POSIX.md) defines that ownership and its
remaining terminal limitations.

## 4. Fault containment

The exception stubs preserve registers before loading kernel selectors, clear DF
and normalize an error word for both classes of CPU exception. Their frame has
an extra error word compared with the IRQ/syscall frame. A recovered stack fault
removes that word and resumes the original context; other faults terminate.
The handler checks the saved CS privilege level, reports vector/error/EIP/CS
and CR2 for page faults, then terminates a faulting user task with status
`128 + vector`, matching v0.4.0's `terminate_current_from_exception`. Waiters,
orphans, GPU ownership and memory follow the normal exit path. The scheduler
returns another task's valid IRQ/syscall frame to assembly.

Kernel exceptions remain fatal, as do NMI, double fault and machine check even
when they interrupt user code. Invalid syscall buffers return `EFAULT` before
the kernel touches them: validation walks every user page and requires write
permission for read/input, wait status, system-info and GPU-info destinations.
A CPU does not switch a process within its syscall. Kernel waits release the
global lock and resume on the same CPU; remote termination records a pending
exit and cannot destroy that process until it reaches a safe return boundary.

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
timer preemptions per worker, as well as successful pattern checks. A serial
`MULTITASK PASS` alone is insufficient.

The same run checks specific/any-child waits, argument boundaries and bad
pointers, capacity exhaustion, sleeping wakeups, repeated slot reuse, null
status pointers, orphan cleanup, process counts and GPU ownership on exit.
It also checks #UD, divide error, privileged-instruction #GP and #PF from null,
absent, kernel, RAM-alias, read-only text and stack-guard accesses. A ring-3
`IN` from port `0x80` must produce #GP before completing the I/O. A fault with
null data selectors and DF set exercises the exception entry. The host requires
the page-fault error bits and CR2 values, surviving siblings, successful waits,
GPU reacquisition and identical free-page counts after teardown. A `memhog32`
helper tests zeroed large BSS and wait-result copies crossing noncontiguous
physical pages at a page-table boundary. Repeated loads fill the table on the
normal machine or exhaust physical RAM on the 32 MiB machine; failed loads and
reaped children must return the exact baseline free-page count.

The host also types `start`, `wait`, `run` and `exit`, checking background
collection, fault status 134 followed by a working shell prompt, foreground
status and return from the diagnostic to the reference shell. The POSIX check also
verifies reference-shell restart by init32. The explicit `--expect-desktop` diagnostic checks console clearing, fullscreen
graphics and cursor restoration. Normal `smoke desktop` now boots the reference
session, while `sxgui-smoke` verifies actual toolkit glyphs and wheel/scrollbar input. The harness uses a
snapshot of the disk and retains serial logs/screenshots with `--artifacts`.

Process-info accounting records running ticks, including kernel time, while
the system reports ticks summed across scheduling CPUs. Uptime uses
monotonic time independent of both tick counters. Socket reads/receives park in the native
scheduler and copy through the waiting process's address space; killed audio
owners release their session. The optional native OSABI survives fork and
changes on successful exec. See [I386_SERVICES.md](I386_SERVICES.md).

## Remaining boundaries

User-installed signal handlers and ASLR remain unported. Events, timers,
semaphores and wait-any/all now use the reference objects; see
[I386_GRAPHICS.md](I386_GRAPHICS.md). RAM above the mapped first GiB is not allocated. User read-only pages and supervisor mappings are protected,
but absence of NX prevents execute-disable and full W^X enforcement. Physical
laptop validation is reserved for the v0.4.2 target.
