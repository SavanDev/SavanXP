# Threads — more than one execution context per process

> **Status: phases 0-5 done (2026-10-22).** This
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
> [KERNEL_NATIVE_API.md](KERNEL_NATIVE_API.md)), taking turns on the CPU.
> Phase 2 lets them run on several cores at once, with a user-space TLB
> shootdown that never waits for the other cores. Phase 3 gives each thread
> its own `%fs` and `%gs` (`SXN_SYS_SET_THREAD_BASE`, slice 24). Phase 4
> lets them sleep on a word of memory (`SXN_SYS_WAIT_ADDRESS` and
> `SXN_SYS_WAKE_ADDRESS`, slice 25), the primitive locks are built on.
> Phase 5 gives POSIX programs `pthread.h` on top of all of it.

Threads are the first kernel prerequisite of a future Win32 subsystem — the
others are per-thread TLS registers, exceptions delivered to user mode and
reserve/commit virtual memory — but they are not Win32 work. pthreads, the
ports that need them (FFmpeg, Xash3D) and the asynchronous present of the
compositor all want the same thing.

## The short version

**Before phase 0 the kernel had no thread concept: `Process` was the unit of
scheduling.** `object::Type::thread` was in the enum and nothing used it. The
work split in two very different halves, and all of it has landed since:

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
- **The interim rule** (lifted in phase 2) was `sibling_running_elsewhere` in `runnable_here`:
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

### Phase 2 — the address space on many cores — **DONE**

What landed:

- **The kernel records which address space each core holds.**
  `vm::load_address_space` is now the only way a thread's space gets into
  CR3 (`switch_to_thread`), and it notes the PML4 per core. `resume_context`
  reloads the same value; `sx`'s copy to another process's memory loads a
  space for one `memcpy` within one hold of the lock, so it records nothing.
- **Unmapping asks, and does not wait.** `unmap_page` invalidates locally and
  notes every other core holding the space; at the end of the batch (the
  whole view) those cores get a new shootdown number and a reschedule IPI
  (`smp::kick`). `smp::lock_kernel()` calls `vm::sync_user_tlb()`, which
  reloads CR3 when this core's number is behind. A core in user mode takes
  the IPI and enters the kernel; one spinning for the lock or asleep in
  `wait_for_interrupt` catches up when it gets the lock, before it can touch
  user memory. A CR3 reload by a thread switch counts as a flush.
- **Pages are freed late, by section reference.** The unmapped view's
  reference to its section goes on an intrusive list in `SectionObject`
  (`deferred_next`, `_refs`, `_cpus`, `_sequence`), not its raw pages: a
  file-backed section on the list is still a live, cached section that
  another mapping may pick up. `vm::reclaim_deferred_views` releases it once
  every core in the mask has flushed; the BSP's tick and every unmap call it.
  No capacity limit: a section queued twice keeps one node and waits for the
  later shootdown. The rollback of a failed `map_section_view` goes through
  the same path, since a sibling may already have touched the pages.
- **Nothing changes for a single-threaded process.** Its space is loaded on
  no other core, so the reference is released on the spot, as before.
- **The interim rule is gone**, and with it what it guaranteed: that a
  thread ending its process never finds a sibling running elsewhere.
  `kill_pending`/`kill_exit_code` moved from `Thread` to
  `Process::exit_requested` (with `exit_code`, first request wins).
  `terminate_process` kicks every thread running on another core and returns;
  `terminate_current` then **parks** the calling thread — a zombie whose
  object is signaled with the rest — and switches away. Each running sibling
  stops at its next entry to the kernel (IPI, tick, end of syscall) and calls
  back in; the last one to stop finds nobody running and tears the process
  down. A ready thread of an ending process is not scheduled again
  (`runnable_here`), unless its context is in ring 0, halfway through a
  syscall that must finish.
- `destroy_address_space` panics if another core still has the space loaded.

Where it differs from the plan:

- **The check is in `native-test`, not a boot self-test.** A `shootdown`
  child reads a view in a loop that never enters the kernel while its main
  thread unmaps the view (and drops the section's last reference); the reader
  has to die with a #PF. With `sync_user_tlb` disabled, the same test fails
  on 4 cores — the reader reads on through the stale translation — which is
  the proof that the test sees what it is meant to. Two more children make the
  exit paths race running threads: `exitall` ends the process from a second
  thread while the main one spins, and `threads` is killed with two spinning
  threads.
- **No new IPI vector.** The reschedule IPI already enters the kernel through
  `lock_kernel()`, which is where the flush happens.

Verified by `native-test` on 1, 2 and 4 cores (25 runs in a row on 4), and
the regular smokes unchanged, the desktop ones on 4 cores (`smoke`,
`windowd-smoke`, `session-smoke`, `taskbar-smoke`).

### Phase 3 — per-thread `FS_BASE` / `GS_BASE` — **DONE**

What landed:

- `Thread::fs_base` and `gs_base`, swapped by `switch_to_thread` on a real
  switch next to the FPU state (`arch::x86_64::save_user_segment_bases` /
  `load_user_segment_bases`). **The kernel uses neither segment** —
  `cpu_index()` reads `TR` and there is no `swapgs` — so the MSRs hold the
  running thread's values across every kernel entry and nothing has to be
  saved on the way in.
- **Saved by reading the MSRs, not trusted from the fields.** While a thread
  runs, the MSRs are the live copy. A user `mov` to `%fs` can change the base
  without the kernel knowing, and `CR4.FSGSBASE` would let `wrfsbase` do it
  too; reading them back on the way out keeps either from going stale.
- `SXN_SYS_SET_THREAD_BASE` (slice 24 of
  [KERNEL_NATIVE_API.md](KERNEL_NATIVE_API.md)): user addresses or 0 only,
  since a non-canonical base is a `#GP` on the kernel's `wrmsr`.
- `exec` resets both to 0; POSIX `fork` copies them from the calling thread
  (from the MSRs when it is the one running, as with the FPU).

Where it differs from the plan:

- **No TLS argument to thread creation.** Linux's `CLONE_SETTLS` sets the base
  before the thread runs; here a new thread starts at 0 and sets its own, so it
  must not touch `%fs`/`%gs` before that call. pthreads can wrap that in its
  start routine; if a race ever matters, a fifth argument is the fix.
- **No call to read a base back.** TLS blocks start with a self-pointer
  (`%fs:0` on x86-64 System V, `%gs:0x30` in the Win64 TEB), which is how
  user code finds them.
- `CR4.FSGSBASE` stays off.

Verified by `native-test` on 1, 2 and 4 cores: three threads and the main one
point both bases at blocks of their own and read them back 2000 times each,
yielding every 64 rounds. With the swap removed from `switch_to_thread` the
test fails on one core and on four.

### Phase 4 — wait on an address — **DONE**

What landed:

- `SXN_SYS_WAIT_ADDRESS` (address, expected value, size, timeout) and
  `SXN_SYS_WAKE_ADDRESS` (address, count), slice 25 of
  [KERNEL_NATIVE_API.md](KERNEL_NATIVE_API.md). A parked thread is
  `State::blocked_wait` with `WaitReason::address`, the address in
  `Thread::wait_address`; the tick's `wake_wait_timeouts` expires its
  deadline like an object wait's.
- **No lost wake, by the kernel lock.** The waiter reads the word and parks
  under the lock; a waker stores its new value and then takes the lock to
  wake. Either the waker finds the thread parked, or the waiter's read comes
  after the store and sees the change (`-EAGAIN`). When the big lock is
  split (SMP phase 4), the parked set needs a lock of its own that the
  read-and-park holds across.
- **The parked set is the thread table**, scanned by (process, address) —
  one address space per process, so that is the (address space, virtual
  address) key of the plan. That key is private: two processes sharing a
  section never meet, even at the same address, which is `WaitOnAddress`'s
  rule too. The private case is the one every lock needs.
- **Oldest first.** Each wait takes a ticket; a wake releases the lowest
  tickets, so a lock handed over one waiter at a time is fair.

Where it differs from the plan:

- **Sizes 1, 2, 4 and 8**, as `WaitOnAddress` takes them, not only the 32-bit
  word of a futex. The size is an argument, the word must be aligned to it,
  and only its bytes are compared.
- **A word already changed is `-EAGAIN`**, as `futex` reports it;
  `WaitOnAddress` calls it success. The caller loops on the word either way.
- No bitset, requeue or priority-inheritance variants. pthreads condition
  variables can do without requeue (wake all and let them retry), at the
  price of a thundering herd that only matters with many waiters.

Verified by `native-test` on 1, 2 and 4 cores: the immediate returns and the
refusals, three waiters on one word released one and then the rest, and a
three-state mutex built on the pair keeping a counter exact while four threads
take it 5000 times each (and sleep holding it now and then, so the others park
even on one core). With the wake turned into a no-op the test fails, and so
it does with a wait that never parks.

### Phase 5 — pthreads in the POSIX SDK — **DONE**

What landed, in the kernel:

- **The thread calls and the address waits are open to every subsystem**
  (`native_call_open_to_all_subsystems`): a POSIX process reaches
  `SXN_SYS_THREAD_CREATE`, `_EXIT`, `_EXIT_CODE`, `SET_THREAD_BASE`,
  `WAIT_ADDRESS` and `WAKE_ADDRESS` as it already reached the object floor.
- **Per-thread subsystem state.** `subsystem::register_thread_state_size`,
  allocated on first use by `process::thread_subsystem_state` and freed with
  the thread. POSIX keeps a thread's `poll()` request there
  (`posix::ThreadState`); the current directory stays per process. Before
  this, two threads polling at once shared one request.
- **`exec` ends the other threads** (`end_other_threads`), after the path is
  resolved and before the image is loaded. Siblings stopped in user mode or
  parked by a `kBlockedResult` call end in place. One running on another core,
  or asleep in ring 0, gets `Thread::end_requested` and a kick, and ends at its
  next kernel entry, as a process with `exit_requested` does; the exec rewinds
  `rip` over its `int $0x80` and runs again once the core turns around
  (`ImageFailure::threads_running`, never reported to the caller). When two
  threads exec at once, the one asked to end first loses.
- **Ring-0 sleeps give up.** The blocking loops in `net.cpp` (connect, send
  window, UDP receive, ARP, ping) check `process::current_thread_must_stop`
  after every wake and return `-EINTR` (`SX_EINTR`, new) when the process is
  ending or an exec asked the thread to end. `terminate_process` now waits
  for a thread preempted inside such a call, as it waits for one running
  elsewhere, so the call releases what it holds before the process goes.
- **Socket calls that sleep hold their object.** `connect_fd`, `sendto_fd`,
  `recvfrom_fd` and the socket case of `write_handle` keep a reference to the
  handle's object across the call (`HeldObject`), so a sibling closing the
  handle cannot free the socket under it. The blocking `read` already parked
  the `kBlockedResult` way and looks the handle up again on each wake.

What landed, in the POSIX SDK (`runtime/pthread.c`, `pthread.h`, `sched.h`):

- create, join, detach, self, equal, exit; stack size and detach state
  attributes; normal, recursive and error-checking mutexes; condition
  variables (with `CLOCK_MONOTONIC` through `pthread_condattr_setclock`);
  once; keys with destructors; `sched_yield`.
- **A thread is one anonymous mapping**: stack, static TLS block, TCB and the
  descriptor. A joinable thread's mapping and handle are released by the
  join; a detached one goes on a list when it ends and is released by the next
  create, join or detach once its handle is signaled.
- **TLS, variant II.** `%fs` points at a TCB (`sx_thread.h`): `%fs:0` is its own
  address, `%fs:0x28` a copy of the stack guard. The executable's `PT_TLS` is
  copied below it. The main thread's block is built by `sx_tls_init`, called
  from `sx_start_dynamic` after the loader, which finds `PT_TLS` through
  `sx_image_base`.
- **`errno` per thread**, in the TCB; a global until `sx_tls_init` runs.
- **The heap, stdio and the socket table take a recursive lock** at their
  public entry points (`sx_lock`), free until the first `pthread_create`. One
  lock for all of stdio. `fork` takes the heap and stdio locks across the
  call, so the child never inherits one held by a thread it does not have.

Where it differs from the plan:

- **The static link layout changed.** The kernel passes the start of the
  first `PT_LOAD` as the ELF header's address, which was only true for PIE
  images: the static linker script left the headers unmapped and the text at
  `0x400000`. It now maps them there (`0x400000 + SIZEOF_HEADERS`) with the
  text right after, so `sx_image_base` is the header for every program. The
  script is a `LINK_DEPENDS` of every static program now; it was not, and
  editing it relinked nothing.
- **Native programs get no TLS from the runtime.** They link the same runtime
  but own their segment bases: `sx_tls_init` returns when `EI_OSABI` is
  native, so errno stays global and the runtime's locks stay off there.
- **Only the executable has TLS.** The loader refuses the TLS relocations, so
  a shared library with `__thread` does not load; dynamic TLS
  (`__tls_get_addr`) waits for a library that needs it.
- **`runtime/pthread.c` is a runtime unit** like `posix.c`, which needs it:
  CMake, `tools/build-user.sh` and the ports that compile the runtime
  themselves (`doomgeneric`, `ccleste`, `ffmpeg/runtime.sh`) all list it. A
  port script that compiles `posix.c` without it fails to link
  (`sx_lock_acquire` undefined), which is how `verify_doom_persistence.sh`
  found the first one.
- **No requeue.** A broadcast wakes every waiter and they contend for the
  mutex; see phase 4.
- **A blocked `read` on a socket whose handle a sibling closed** stays parked
  (it looks the handle up again and finds nothing), as a pipe read does. It
  ends with the thread or the process.

Verified by `pthread-test` (`subsystems/posix/userland/pthreadtest.c`) on 1, 2
and 4 cores: four workers each take a mutex 20000 times around a counter,
churn `malloc`/`free` with patterns, flip and check `errno` and `__thread`
variables while yielding, run a `pthread_once` together and leave key values
for destructors; a producer and three consumers through a condition
variable; a timed wait on the monotonic clock; two threads in `poll()` at
once; 80 detached threads, more than the process's handles; ten forks while
two threads allocate and print; an exec from a child with threads spinning,
blocked and asleep; `exit()` from a second thread. Each of these was broken on
purpose and the test caught it: without the runtime locks and with one `poll`
request per process it hangs (the smoke times out), with a global `errno` it
fails, and with an exec that leaves the threads running the kernel panics.

### Afterwards

Thread counts in `SXN_SYS_PROCESS_QUERY` and the Task Manager, and the next
Win32 prerequisite: exceptions delivered to user mode, which also gives POSIX
`SIGSEGV` handlers.
