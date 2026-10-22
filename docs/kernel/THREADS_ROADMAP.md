# Threads — more than one execution context per process

> **Status: measured, not started (2026-10-21).** This document is the
> inventory: what the kernel assumes today about "one process, one context",
> what has to move, and the order in which to move it so that every phase
> boots and can be verified on its own.

Threads are the first kernel prerequisite of a future Win32 subsystem — the
others are per-thread TLS registers, exceptions delivered to user mode and
reserve/commit virtual memory — but they are not Win32 work. pthreads, the
ports that need them (FFmpeg, Xash3D) and the asynchronous present of the
compositor all want the same thing.

## The short version

**The kernel has no thread concept: `Process` is the unit of scheduling.**
`object::Type::thread` exists in the enum and nothing uses it. The work splits
in two very different halves:

- **The split itself is mechanical but wide.** Every field that describes "what
  the CPU is doing" — saved context, kernel stack, FPU area, the core it last ran
  on, every wait and block field — moves from `Process` to a new `Thread`, and
  every place that schedules, blocks or wakes a `Process` starts doing it to a
  `Thread`. No behaviour changes while each process still has exactly one.
- **The second thread is what is actually hard**, because it breaks one rule the
  SMP work wrote down: *an address space is loaded on at most one core at a
  time*. That rule is why user-space TLB coherence needed no mechanism
  ([SMP_ROADMAP.md](SMP_ROADMAP.md#phase-3--tlb-shootdown)). Two threads of
  one process on two cores need a real user-space shootdown.

The plan keeps those apart: the split lands first with no new behaviour, the
second thread lands next restricted to one core per address space, and the
shootdown lifts that restriction afterwards.

## Inventory

### What `Process` holds today

`include/kernel/process.hpp`, classified by who it belongs to once threads
exist:

| Stays in `Process` (the container) | Moves to `Thread` (the execution context) |
|---|---|
| `pid`, `parent_pid`, `orphaned` | `state` (ready/running/blocked/sleeping) |
| `name` | `context` (`SavedContext*`, on the kernel stack) |
| `address_space` (`vm::VmSpace`) | `kernel_stack_base`, `kernel_stack_size` |
| `handles[kMaxFileHandles]` | `fpu_state[512]` |
| `subsystem_id`, `subsystem_state` | `cpu` (where a ring-0 context must resume) |
| `exit_code`, alive/zombie state | `time_slice`, `cpu_ticks` |
| `idle` | `wait_reason`, `wait_handle_count`, `wait_all`, `waited_objects[]`, `waiting_for_pid`, `wait_status_address` |
| | `blocked_io_fd`, `blocked_read_*`, `blocked_write_*` |
| | `wake_deadline_ms`, `sleep_timer` |
| | `kill_pending`, `kill_exit_code` (one flag per running context) |

`State` itself splits: a process is *alive* or *zombie*; a thread is
*ready, running, blocked_read, blocked_write, blocked_wait or sleeping*.
`State::unused` stays as the slot marker of both tables.

### Who touches those fields

Measured with grep over the tree, counting references to the fields that move:

| File | `state` | current process | `context` | `blocked_*` |
|---|---:|---:|---:|---:|
| `kernel/process.cpp` | 45 | 43 | 70 | 74 |
| `kernel/native_dispatch.cpp` | 5 | 1 | 142 | 10 |
| `subsystems/posix/kernel/posix_translation.cpp` | 7 | 2 | 7 | 31 |
| `subsystems/posix/kernel/posix_dispatch.cpp` | 2 | 85 | 240 | 0 |
| `kernel/sx.cpp` | 0 | 9 | 0 | 0 |

Most of the `context` hits in the two dispatchers are reads of syscall
arguments (`context->rdi`, ...) and do not change. The ones that do change are
the ones that complete a blocked call on someone else's behalf
(`proc.context->rax = result`): those write into the *thread* that blocked.

`kernel/process.cpp` walks `g_processes` in **20 places**. Twelve of them look
for a blocked or runnable context — the wakers (`wake_sleepers`,
`wake_wait_timeouts`, `wake_blocked_readers_for_pipe`,
`wake_blocked_writers_for_pipe`, `recheck_parked_processes`,
`wake_parked_waiters`, `wake_blocked_socket_readers`, `wake_waiters_for_object`,
`notify_tty_line_ready`), the waitpid parent lookup and the scheduler
(`pick_next_runnable`, `has_runnable_non_idle`). Those become walks over the
thread table. The other eight are about processes as such (slot allocation,
`find`, children, zombies, snapshots) and stay.

### Scheduler and per-core state

- `smp::Cpu::current` and `smp::Cpu::idle` are `process::Process*`. They become
  `Thread*`; "the current process" is `current->process`.
- `switch_to_process` does four things: FPU save/restore, `rsp0` in the TSS,
  `CR3`. With threads the first two are per thread, and `CR3` is reloaded only
  when the incoming thread belongs to a different address space — which is
  also the first real win of the split: switching between two threads of one
  process keeps the TLB.
- `runnable_here` already has the rule a thread needs: a context left in ring 0
  by `smp::wait_for_interrupt` (TTY, network, PC speaker, one POSIX path)
  resumes only on the core it slept on. That rule moves unchanged.
- The idle of each core is a full process with one page of ring-3 code. It
  stays a process with one thread; nothing gains from turning it into a
  kernel-only thread now.

### Kill and exit

- `terminate_process` already copes with a target running on another core
  (`kill_pending` + `smp::kick`). With threads, terminating a process means
  doing that **for each of its threads**: the ones parked or ready are torn
  down in place, the ones running elsewhere are flagged and kicked, and the
  process's resources (handles, address space) are released only when the last
  thread is gone — never while another core is still on its page tables.
- The exit of the last thread is the exit of the process. This matches both
  POSIX (`exit()` ends every thread, returning from `main` calls `exit()`) and
  Win32 (`ExitThread` on the last thread ends the process).

### Subsystem state

- `subsystem::register_process_state_size` gives each subsystem private bytes
  per process. POSIX keeps the cwd there — process state, correct — but also
  the in-flight `poll()` request (`posix::ProcessState::poll_*`), which is
  **per thread**: two threads can each be blocked in their own `poll`.
- So the subsystem layer gains a second registration,
  `register_thread_state_size`, and the poll fields move there. The
  wait-recheck hook (`subsystem::recheck_parked_process`) is called per thread.

### Userland

- **No TLS anywhere.** `errno` is one global (`posix.c` returns `&g_errno`),
  the heap has no lock, and neither does stdio. The ELF loader and `ldso`
  ignore `PT_TLS`.
- **`%fs` and `%gs` are free.** The kernel loads both with the data selector
  and never uses them: the core index comes from the TSS selector (`str`), not
  from a per-CPU segment. So per-thread `FS_BASE`/`GS_BASE` need no `swapgs`
  discipline — the kernel just writes the two MSRs on a switch.
- The stack canary (`__stack_chk_guard`) is per process and stays that way:
  every thread of a process shares it, as on every other system.

### Budget

- Kernel stacks are 8 contiguous pages (32 KiB) per context. Today that is one
  per process (`kMaxProcesses = 64`); with threads it is one per thread.
- `Thread` is ~0.8 KiB in place (512 of them are the FPU area). A static table
  of 256 threads is ~200 KiB of kernel BSS, which is the cost of not having a
  thread allocator yet.

## Decisions taken

**One id counter for processes and threads.** The main thread's tid is the
pid; further threads draw from the same counter, so no tid ever equals a pid
of another process. That gives POSIX `gettid() == getpid()` on the main thread
and gives Win32 the unique `GetCurrentThreadId` it expects.

**A thread is a waitable object.** `object::Type::thread` gets its object: it
is signalled when the thread exits and carries the exit code. That is
`pthread_join` and `WaitForSingleObject(hThread)` with the wait machinery that
already exists (`SXN_SYS_WAIT`, `SXN_SYS_WAIT_MANY`).

**User stacks are allocated by userland.** The creator passes entry point,
argument and stack top; the kernel never picks a user stack for a new thread.
This is what both pthreads and Win32 (`RtlCreateUserStack` lives in `ntdll`)
do. Demand growth (`grow_user_stack`) stays for the main stack only; a thread
stack is committed up front until reserve/commit memory exists.

**Round robin over threads, not over processes.** A process with eight
runnable threads gets eight shares of the CPU. That is what Linux and NT do,
and fairness between processes is a scheduler policy for later, not part of
introducing threads.

**`fork` copies only the calling thread; `exec` ends all the others.** This is
POSIX's rule, and the only one that does not leave threads running against a
replaced address space.

**Synchronization gets one kernel primitive: wait on an address.** pthread
mutexes and condition variables, Win32 `CRITICAL_SECTION`, `SRWLOCK` and
`WaitOnAddress` all reduce to "sleep while this word holds this value" plus
"wake N sleepers on this word" — Linux's futex, NT's keyed events. One native
call pair, keyed by address space and virtual address, parked through the same
deadline machinery as every other wait.

## Phases

Each phase boots, runs every existing smoke unchanged, and adds a check of its
own.

### Phase 0 — the split, one thread per process

`Thread` and its table, every moved field, `smp::Cpu::current` as `Thread*`,
the wakers and the scheduler over threads, `register_thread_state_size` and
POSIX poll state moved into it. Every process has exactly one thread, created
and destroyed with it.

No new syscall and no visible change: this phase is measured by everything that
already exists still passing (`smoke`, `windowd-smoke`, `sxgui-smoke`, the
Doom persistence check) and by a switch between two processes still costing
what it did.

### Phase 1 — a second thread, one core per address space

- `SXN_SYS_THREAD_CREATE` (entry, argument, stack top → thread handle),
  `SXN_SYS_THREAD_EXIT`, and the thread object signalled on exit.
- Process exit ends every thread; last thread exit ends the process.
- **Interim rule:** a thread is runnable on a core only if no *other* thread of
  its address space is running on another core. That keeps the SMP rule "one
  address space, one core" literally true, so user-space TLB coherence still
  needs nothing, and it is correct on any number of cores. Threads interleave
  but do not yet run in parallel.
- Check: a native guest test that creates threads, has them write to shared
  memory and signal events, joins them with `WAIT_MANY`, and kills a process
  with threads blocked, ready and running.

### Phase 2 — the address space on many cores

- `vm::VmSpace` records which cores have it loaded.
- Every user-space unmap or permission downgrade sends a shootdown IPI to those
  cores. **It does not wait for the acknowledgement** — that is the deadlock
  phase 3 of SMP found: the cores it waits on may be spinning for the kernel
  lock with `IF=0`. Instead the physical pages are **freed late**: they go to a
  deferred list and return to the allocator only once every core in the mask
  has flushed, either in the IPI handler (it was in user mode, `IF=1`) or at
  its next `smp::lock_kernel()` (it was spinning, and flushes before touching
  anything). A stale translation can then only ever point at a page that is
  still owned by the process.
- Lift the interim rule of phase 1.
- Check: a boot self-test in the style of `smp::selftest_tlb`, with two threads
  of one process on two cores, one unmapping a view the other is reading.

### Phase 3 — per-thread `FS_BASE` / `GS_BASE`

Two fields in `Thread`, written to the MSRs on every switch into a thread of
user mode, and a native call to set them (the `arch_prctl` of this system).
`CR4.FSGSBASE` (user-mode `wrfsbase`) is optional and can come later. POSIX
TLS uses `%fs`; Win64's TEB uses `%gs`, so both are needed and neither
conflicts with the kernel.

### Phase 4 — wait on an address

`SXN_SYS_WAIT_ADDRESS` (address, expected value, timeout) and
`SXN_SYS_WAKE_ADDRESS` (address, count). The parked set is keyed by
(address space, virtual address), so two processes sharing a section wait on
the same word only if they agree on the address — the same limitation
`WaitOnAddress` has, and the private case is the one every lock needs.

### Phase 5 — pthreads in the POSIX SDK

`pthread_create/join/detach/self/exit`, mutexes, condition variables, `once`
and keys on top of phases 1-4; `PT_TLS` in `crt0` and `ldso` for static TLS;
`errno` per thread; locks in the heap and in stdio; the POSIX translation's
`fork`/`exec` rules above. Check: a pthread test that hammers `malloc`,
`errno` and a mutex-protected counter from several threads on several cores.

### Afterwards

Thread counts in `SXN_SYS_PROCESS_QUERY` and the Task Manager, and the next
Win32 prerequisite: exceptions delivered to user mode, which also gives POSIX
`SIGSEGV` handlers.
