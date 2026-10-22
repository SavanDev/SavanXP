# Threads — more than one execution context per process

> **Status: phases 0 and 1 done (2026-10-22), phases 2-5 not started.** This
> document is the inventory: what the kernel assumed about "one process, one
> context", what had to move, and the order in which to move it so that every
> phase boots and can be verified on its own.
>
> Phase 0 landed as `process::Thread` in
> [process.hpp](../../include/kernel/process.hpp): the scheduler, every waker
> and every blocking call work on threads, and a process has exactly one.
> [What it took](#phase-0--the-split-one-thread-per-process--done) differs from
> the plan in three small ways, recorded there.
>
> Phase 1 gave native processes more than one thread
> (`SXN_SYS_THREAD_CREATE`, `_EXIT`, `_EXIT_CODE`, slice 23 of
> [KERNEL_NATIVE_API.md](KERNEL_NATIVE_API.md)). Threads of one process take
> turns on the CPU: they do not run in parallel until phase 2.

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

### Phase 0 — the split, one thread per process — **DONE**

What landed:

- `process::Thread` and a static table of `kMaxThreads` (256). A process's
  lifecycle is `Process::life` (`unused`, `alive`, `zombie`); a thread's
  scheduling state is `Thread::state`, whose `zombie` means "ended, slot not
  released yet".
- `smp::Cpu::current` and `smp::Cpu::idle` are `Thread*`. `process::current()`
  and `sx::current()` still return the process; `current_thread()` is new.
- Every call that can return `kBlockedResult` takes the calling `Thread&`
  (`read_handle`, `write_handle`, `wait_on_handle`, `wait_on_many_handles`,
  `waitpid_child`, `sleep_via_timer`, `poll_fds`, `prepare_exec_image`,
  `fork_process`, `begin_object_wait`, the native read and write). Everything
  else keeps taking the `Process&` that owns the handles.
- `detail::allocate_thread` / `release_thread` own the kernel stacks, which no
  longer appear in process creation, `fork` or the idle.
- **Walks stop at the highest slot ever used** (`g_thread_bound`). The
  scheduler and the wakers walk the table under the big kernel lock on every
  tick and every pipe write; walking all 256 slots instead of the 64 process
  slots slowed the desktop's startup enough on 4 cores for `windowd-smoke` to
  catch it (the panel opened during the test's 640x400 fullscreen phase and
  kept that width: 2 failures in 10 runs, against 0 in 10 before the split).
- `switch_to_thread` reloads `CR3` only when the live value differs, so a
  switch between two threads of one process keeps the TLB. Comparing against
  the live register rather than the previous thread's process also covers
  `exec`, which swaps the address space under the same thread.
- The wait-recheck hook is `subsystem::recheck_parked_thread(Thread&, ...)`.

Where it differs from the plan:

- **POSIX `poll` state stays per process.** With one thread per process it
  is equivalent, and moving it needs `register_thread_state_size`, which has
  no other user yet. It moves in phase 1, when it starts to matter.
- **CPU accounting stays per process** (`Process::cpu_ticks`): the Task
  Manager reads processes, and a per-thread counter has no reader yet.
- **A process that reaps itself releases the thread running the exit.** An
  exiting orphan, or one whose parent already waits, is reaped inside its own
  `terminate_current`, so its kernel stack is freed while still in use until
  the scheduler leaves it. That was already true before the split; it is safe
  only because nothing allocates in between and the big kernel lock is held
  until `resume_context` has switched stacks. Phase 1's `THREAD_EXIT` must not
  rely on it.

Verified with no new check, as planned: the existing scenarios pass unchanged.

### Phase 1 — a second thread, one core per address space — **DONE**

What landed:

- `SXN_SYS_THREAD_CREATE` (entry, argument, stack top, tid out → handle),
  `SXN_SYS_THREAD_EXIT`, and `SXN_SYS_THREAD_EXIT_CODE`, which the plan did
  not have: a waitable handle says *that* a thread ended, `GetExitCodeThread`
  says *how*. Wrappers in `sxn.h`; `sxn_trap4` carries a fourth argument in
  r10.
- `object::ThreadObject`: one per thread, created with it, waitable and
  manual-reset, signaled once with the exit code. The thread and each handle
  hold a reference, so a handle outlives the thread.
- The last live thread's exit ends the process with its code
  (`live_thread_count`); `SXN_SYS_EXIT` from any thread, a kill and a fault end
  every thread, and every thread object is signaled after all of them are
  zombies, so a sibling is never woken by its own process's end.
- **A thread that ends while its process lives on is retired, not freed.**
  Its kernel stack is the one in use, so it goes to `smp::Cpu::retired` and
  the next `choose_next_context` on that core frees it from another stack.
  Releasing a thread clears any core's pointer to it, so a reap that comes
  first cannot free it twice.
- **The interim rule** is `sibling_running_elsewhere` in `runnable_here`:
  checked only for processes with more than one thread slot, so a
  single-threaded process pays nothing. It keeps "at most one thread of a
  process is running" true at every scheduling decision, which is also what
  keeps `terminate_current`'s assertion (nothing deferred the exit) true.
  A thread asleep in ring 0 that the tick preempted is *ready*, not running,
  so a sibling may run elsewhere meanwhile; that thread then resumes only when
  no sibling runs, and the rule holds again before it can exit.

- **A kill can end the thread this core just switched to.** A thread with a
  kill pending that blocks in a syscall hands its core to the next thread,
  which may be a sibling; the kill then ends the process at the syscall's
  exit, sibling included. `handle_syscall` used to return to that sibling
  anyway, so a dead thread ran in user mode, and when the parent reaped the
  process from another core, its slot was freed under the core still running
  it (a kernel page fault in `handle_reschedule_ipi`, about once in ten
  `native-test` runs on 4 cores). It now picks another thread when the one on
  this core is no longer running. `release_thread` asserts that no other core
  is running the thread it frees, which is how the bug was caught.
- **`SXN_SYS_PROCESS_QUERY` reports a process with a kill pending as ended.**
  `TERMINATE` of a process running on another core returns before that core
  tears it down; the query said "running" until then, and `native-test`
  failed on several cores. It was so before threads.

Where it differs from the plan:

- **Native only.** The calls are not in the set open to every subsystem, so a
  POSIX process still has exactly one thread. That made two items of the plan
  moot until phase 5, where they move: per-thread subsystem state
  (`register_thread_state_size`, and POSIX `poll` state in it) and the rule
  that `exec` ends the other threads.

Verified by `native-test` on 1, 2 and 4 cores (25 runs in a row on 4): workers sharing memory and
ending with their own codes, joined through their handles; a child killed with
a thread blocked, one spinning and its main thread blocked; a child whose main
thread ends first; `SXN_SYS_EXIT` from a second thread.

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
`fork`/`exec` rules above. Two kernel items moved here from phase 1, because a
POSIX process cannot have a second thread before this: per-thread subsystem
state (`register_thread_state_size`, with POSIX `poll` state in it) and `exec`
ending the other threads. The thread calls join the set open to every
subsystem (`native_call_open_to_all_subsystems`). Check: a pthread test that hammers `malloc`,
`errno` and a mutex-protected counter from several threads on several cores.

### Afterwards

Thread counts in `SXN_SYS_PROCESS_QUERY` and the Task Manager, and the next
Win32 prerequisite: exceptions delivered to user mode, which also gives POSIX
`SIGSEGV` handlers.
