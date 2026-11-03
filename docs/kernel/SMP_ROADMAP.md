# SMP — running SavanXP on more than one core

> **Status: phases 0 to 4 done, on master. The roadmap is closed; what remains
> is splitting the big kernel lock, one subsystem at a time, when
> [`smp-stats`](#reading-smp-stats) says which.** This document was the
> measurement: what the kernel already had, what it was missing, and in what
> order to attack it so that every phase boots and can be verified on its own.
>
> Phase 0 landed as `smp::` ([smp.hpp](../../include/kernel/smp.hpp),
> [smp.cpp](../../arch/x86_64/smp.cpp)): the APs start, identify themselves by
> their own LAPIC ID and park in a `hlt` loop, and a ping IPI proves the ICR
> delivers. Verified on 4 cores in **both** APIC modes — x2APIC (`-cpu max`)
> and xAPIC (`-cpu qemu64`, the VirtualBox path; SMEP later broke `qemu64`,
> see phase 4, and `-cpu max,-x2apic` tests xAPIC now). Nothing schedules on
> them: from the outside the system behaves exactly as it did on one core.
>
> Phase 1 gave every core its own TSS and its own `smp::Cpu` (current process,
> idle, reschedule request), indexed by the TSS selector. Still nothing
> schedules on the APs.
>
> Phase 2 made every core run user processes, one at a time inside the kernel,
> under a single big kernel lock.
>
> **QEMU boots two cores by default** (`build.sh`, `run_qemu.py`,
> `run_smoke.py`, `shoot.sh`; `--smp 1` still gives one), and so does every
> smoke. [Why two, and what was measured](#two-cores-by-default).
>
> Phase 3 keeps the other cores' TLBs honest when a kernel page is unmapped —
> lazily, at lock acquisition, not with an IPI
> ([why](#phase-3--tlb-shootdown)). A boot self-test catches an AP reading a
> stale translation and proves the flush removes it.
>
> Phase 4 paid the debt around the lock without splitting it: a panic stops
> every core by NMI, uACPI has real locks, device interrupts go to the APs, and
> the lock counts its own contention per entry point (`smp-stats:`). The
> counters' first finding was QEMU's: under TCG, `pause` in the lock's spin
> made four cores crawl.

The short version: **bringing up the other cores is the cheap part.** The
kernel has around 450 mutable globals and no lock discipline at all, so the
expensive part is deciding what protects them — and the answer, for a long
while, should be "one lock".

## Two structural advantages worth naming first

Two decisions already in the tree make SMP far cheaper here than in a typical
hobby kernel. Both should be defended rather than traded away.

**1. The kernel is already non-preemptible.** Every IDT gate is an interrupt
gate ([`cpu_init.cpp:16`](../../arch/x86_64/cpu_init.cpp:16)), the syscall gate at
vector `0x80` included, and nothing in the syscall path re-enables interrupts.
`IF` is 0 from the moment a process enters the kernel until it leaves. That is
a big kernel lock, implicitly held, with no code written to implement it. Phase
2 is not introducing a new concurrency model; it is writing down the one that
already exists and making it hold across cores.

**2. Syscalls never block on the kernel stack.** A syscall that cannot complete
records a state and returns `kBlockedResult`
([`process.hpp:213`](../../include/kernel/process.hpp:213)); the waker then **retries the
operation on the sleeper's behalf** and completes it — see
`wake_blocked_readers_for_pipe()`
([`process.cpp:726`](../../kernel/process.cpp:726)), which re-runs `try_pipe_read`
for every process parked on that pipe. There are no kernel threads halted
halfway down a call stack, no sleep queues holding kernel context, and no
stack-switching context switch: the scheduler picks a `SavedContext` and
`iretq`s into it (`pick_next_runnable()`,
[`process.cpp:594`](../../kernel/process.cpp:594)). A second core therefore needs
no second copy of anything the scheduler owns — only its own idea of which
process it is running.

## Inventory

### Already there

- **Local APIC**, in both modes: xAPIC through MMIO and x2APIC through MSRs,
  behind one pair of accessors
  ([`cpu_init.cpp:191`](../../arch/x86_64/cpu_init.cpp:191)). `local_apic_id()` is
  already exported.
- **A calibrated periodic LAPIC timer** with a PIT fallback
  ([`timer.cpp`](../../arch/x86_64/timer.cpp)), and a tick handler that already
  returns the next context to run
  ([`timer.cpp:210`](../../arch/x86_64/timer.cpp:210)).
- **MADT parsing and IOAPIC programming** ([`ioapic.cpp`](../../kernel/ioapic.cpp)),
  with interrupt source overrides resolved.
- **FPU/SSE state per process**, saved and restored on switch
  ([`cpu.hpp`](../../include/arch/x86_64/cpu.hpp)) — already per-context, not per-CPU.

### Missing

| Gap | Where |
| --- | --- |
| No IPIs. The ICR is unimplemented, and the code says why: "no consumers while there is no SMP" | [`cpu_init.cpp:178`](../../arch/x86_64/cpu_init.cpp:178) |
| One global GDT, one global TSS. `set_kernel_stack()` writes the single `g_tss.rsp0` on every switch | [`cpu_init.cpp:117`](../../arch/x86_64/cpu_init.cpp:117), [`cpu_init.cpp:768`](../../arch/x86_64/cpu_init.cpp:768) |
| No per-CPU infrastructure: no `struct Cpu`, no GS base, no way to ask "which core am I" beyond the LAPIC ID | — |
| `parse_madt()` only handles type 1 (IOAPIC) and type 2 (ISO). Type 0 (Local APIC) — the CPU list — is skipped | [`ioapic.cpp:57`](../../kernel/ioapic.cpp:57), [`ioapic.cpp:143`](../../kernel/ioapic.cpp:143) |
| GSIs are routed to the BSP in physical mode, hardcoded | [`ioapic.cpp:262`](../../kernel/ioapic.cpp:262) |
| No spinlock primitive. The only lock in the kernel is a bare `__atomic_test_and_set` in SxFS | [`sxfs.cpp:148`](../../kernel/sxfs.cpp:148) |
| uACPI's spinlocks and mutexes are stubs that only save and restore `IF` | [`uacpi_glue.cpp:428`](../../kernel/uacpi_glue.cpp:428) |
| The vendored `limine.h` is a 197-line subset with no `limine_mp_request` | [`vendor/limine.h`](../../vendor/limine.h) |
| QEMU is launched without `--smp`; every smoke runs on one core | [`build.sh`](../../build.sh) |
| Scheduler state is global and singular: `g_current`, `g_idle`, `g_schedule_cursor`, `g_resched_pending` | [`process.cpp:61`](../../kernel/process.cpp:61) |

## Phase 0 — bring the APs up — **DONE**

**Goal:** the other cores execute kernel code and park in `hlt`. Nothing else
changes.

What it actually took, against the plan below:

- **The Limine path was the right call.** `limine_mp_request` went into the
  vendored header; the APs arrive in long mode, on their own stacks, on the
  same page tables — no real-mode trampoline, no identity mapping. The MADT
  type 0 path stays available behind the same `smp::` API if another bootloader
  ever matters.
- **The MP structures survive the boot for free.** They live in
  bootloader-reclaimable memory, which this kernel never reclaims —
  `memory::initialize` takes only `usable` regions
  ([`physical_memory.cpp:117`](../../kernel/physical_memory.cpp:117)). That removed
  the usual hazard of the bootloader's AP stacks being recycled underfoot.
- **Two things an AP must not do**, both found by reading before writing:
  `ltr` on the shared TSS (its Busy bit makes the second load a #GP), and
  `initialize_local_apic()`, which sets LINT0 to ExtINT — legal for exactly one
  core in the system, and the BSP already claimed it. Hence the narrower
  `ap_initialize_cpu()` / `ap_initialize_local_apic()` pair.
- **An AP cannot print.** The console is unprotected shared state, so the parked
  cores touch nothing but their own registers and atomics. The BSP does all the
  reporting.
- **Step 3 was not needed.** Limine gives each AP a stack, and a parked core
  does not outgrow it. Allocating kernel stacks belongs to phase 1, where the AP
  actually runs something.

**Delivered:** `smp: 4 cores reportados, 4 en linea (bsp lapic 0, x2APIC)` and
`smp: ping IPI 3/3 ok` under `./build.sh smoke smoke --smp 4`.

The original plan follows.

1. Enumerate the CPUs. Two options: add `limine_mp_request` to the vendored
   header, or extend `parse_madt()` with entry type 0. The Limine path also
   hands over the APs already in long mode with their own stacks, which saves
   writing a real-mode trampoline and the identity mapping it needs — that is
   the single biggest saving available in this phase, and the reason to prefer
   it.
2. Write `send_ipi()` in `arch`, covering both APIC modes. The 64-bit
   x2APIC ICR is one MSR write; the xAPIC one is two MMIO writes, high word
   first, and needs the delivery-status poll.
3. Give each AP a kernel stack and park it.

**Deliverable:** `cpu: 3 APs online` in the boot log under `--smp 4`, with the
system otherwise behaving exactly as it does today. **Estimate: 2-3 days.**

## Phase 1 — per-CPU state — **DONE**

**Goal:** each core can hold its own current process, without any core yet
being allowed to schedule.

What it actually took, against the plan below:

- **One GDT, a TSS per core.** Not a GDT per core: every TSS descriptor sits in
  the one shared GDT, side by side from selector `0x28`, all installed by the
  BSP before any AP starts. Each core then loads its own with `ltr`.
- **Indexed by the TSS selector, not the LAPIC ID.** Each core has a different
  TSS loaded, so `str` names the core with a register read
  (`arch::cpu_index()`). The LAPIC ID is an MSR or MMIO read that can
  exit the VM under KVM on every access, and the current process is
  read dozens of times per syscall. `ltr` is privileged, so user space cannot
  change the answer. Each AP reads its selector back at boot and the BSP checks
  it: `smp: TSS propio en 3/3 APs ok`.
- **`smp::Cpu` holds `current`, `idle` and `resched_pending`.** The 148
  references across `process.cpp` and both dispatchers now go through
  `this_cpu()`. `g_schedule_cursor` stays global on purpose: it is the cursor
  of the one shared queue that phase 2 puts under the lock.
- **Deferred: an idle process per AP.** Only the BSP has one. Whether the idle
  of an AP is a process like the BSP one (a process slot, an address space and
  16 KiB of kernel stack each) or a `hlt` loop in the kernel is a phase 2
  decision, and phase 2 is its first consumer.
- **Still not needed: kernel stacks per AP.** A parked AP never reaches ring 3,
  so its `rsp0` is never used.

Verified on 4 cores in both APIC modes, as in phase 0.

The original plan follows.

- **A TSS per CPU.** This is the hard requirement, not a nicety: `rsp0` is
  where the CPU lands on a ring 3 to ring 0 transition, and two cores sharing
  one `rsp0` corrupt each other on the first concurrent syscall.
- **A GDT per CPU**, because the TSS descriptor lives in it. The **IDT can stay
  shared** — it is written once during init and read-only afterwards.
- **`struct Cpu`**, indexed by LAPIC ID. Reading the LAPIC ID on kernel entry is
  slower than `swapgs`, but it is correct, it needs no MSR discipline in the
  entry stubs, and the entry paths are three
  ([`context.S`](../../arch/x86_64/context.S)). Take the simple version first; a
  GS-based fast path is an optimization for later, and one that has to be
  designed against interrupt gates rather than `syscall`/`sysret`.
- **An idle process per CPU.** Today there is one `g_idle`
  ([`process.cpp:75`](../../kernel/process.cpp:75)); every core needs something to
  run when the ready queue is empty.
- Move `g_current` into `struct Cpu`. This is the change that touches the most
  call sites, and it is mechanical.

**Estimate: 3-5 days.**

## Phase 2 — the big kernel lock and the multi-core scheduler — **DONE**

**Goal:** all cores run user processes. Correct, with essentially no scaling.

What it actually took, against the plan below:

- **Five doors, not three.** The lock (`smp::lock_kernel()`, a ticket
  spinlock from [`spinlock.hpp`](../../include/kernel/spinlock.hpp)) is taken by
  the syscall, timer and reschedule-IPI stubs in
  [`context.S`](../../arch/x86_64/context.S), by `dispatch_external_vector` for
  device vectors, and by `handle_exception` for faults from ring 3 — a
  user page fault grows the stack or kills the process, and both are kernel
  state. The ping IPI does not take it: the BSP sends it during boot and waits
  for the acknowledgement.
- **Released after the stack switch, never before.** The stubs release it after
  `movq %rax, %rsp`, and `resume_context` after loading the new `rsp`.
  Releasing it in C, still on the outgoing process's kernel stack, lets another
  core resume that process; its next syscall lands at the top of the very stack
  the first core is still executing on.
- **Boot runs without the lock.** The APs are parked until
  `process::start_init()`, so there is nobody to exclude; `start_init` takes it,
  creates the idles and releases the APs with `smp::start_scheduling()`.
- **Sleeping inside the kernel was the real work.** Four places did `sti; hlt`
  in the middle of a syscall — the idle's `SYS_YIELD`, the network stack's
  `wait_for_tick`, the PC speaker's beep and `tty::read_line` — and one driver
  (the PS/2 LED update) turned interrupts back on unconditionally from inside
  the tick. All now go through `smp::wait_for_interrupt()`, which releases the
  lock around the `hlt`, or save and restore `IF`.
- **A context preempted in ring 0 is pinned to its core.** A process sleeping
  in `wait_for_interrupt` can be preempted by the tick with its context in the
  middle of a syscall. That context is only resumed on the core it slept on
  (`runnable_here()` in [`process.cpp`](../../kernel/process.cpp)): the kernel code
  around it — `cpu_index()` is deliberately fusable by the compiler — assumes
  the core does not change during one kernel entry.
- **The idle of an AP is a process, like the BSP's.** Deferred from phase 1.
  The scheduler only knows how to return to a `SavedContext`, so an in-kernel
  `hlt` loop would have needed a second way out of the kernel. Each idle costs
  a process slot, 16 KiB of kernel stack and two pages; each core returns only
  to its own.
- **One clock, N preemption timers.** Each AP starts its LAPIC timer with the
  BSP's calibrated count (`timer::start_on_secondary()`). Only the BSP's tick
  advances `timer::ticks()` and does the system's periodic work — sleeper and
  timeout scans, device service, input polling, `timer-stats`. With N cores
  adding to it, everything paced in ticks would run N times too fast.
- **Killing a process that runs on another core is deferred.** Its kernel
  stack and page tables are live over there. `terminate_process` marks it
  (`exit_requested` since threads; a per-thread `kill_pending` before) and
  sends the reschedule IPI; the owning core terminates it on its next return
  to ring 3 — the IPI itself, the next tick, or the end of the syscall it was
  in.
- **No reschedule IPI for wakeups.** See
  [the trap below](#known-traps): the same-core handoff stays, and an idle core
  picks up whatever is left in the queue on its own tick, at most 1 ms later.
  The IPI exists, but only kills use it.
- **CPU accounting counts every core.** `cpu_ticks_total` is now the ticks that
  found a process running on *any* core, so one tick still increments exactly
  one process; `cpu_online` is the number of cores that schedule, which is also
  the number of idle processes. [Details](../userland/SYSTEM_MONITORING.md#more-than-one-core).

**Delivered:** `smp: planificando en 4 de 4 cores` under
`./build.sh smoke smoke --smp 4`,
and `smptest` in the smoke suite: it sees a child in `State::running` from
inside its own syscall — impossible on one core — kills a child spinning on
another core, and runs a pipe ping-pong.

**One measurement to keep: the lock is held across disk I/O.** On the
development host (an i5-2400, 4 logical CPUs) `windowd-smoke` passes at
`./build.sh smoke windowd-smoke --smp 2` under the available accelerators but
fails at `./build.sh smoke windowd-smoke --smp 4`: its size-hint check assumes
`progman` finishes its startup scan of `/` within 60 frames.
That scan is ATA PIO inside syscalls, under the lock; with four vCPUs on four
host CPUs, the cores spinning for the lock take host time from the thread that
emulates the disk, and `progman` measured more than 1.8 s of CPU without
getting there. Nothing is lost or corrupted — it is phase 4's problem showing
up early, and the reason not to size `--smp` to every host CPU.

**Not done:** TLB shootdown, which became [phase 3](#phase-3--tlb-shootdown).
Also still pending: a `panic` on one core does not stop the others, and every
device interrupt still goes to the BSP (phase 4).

The original plan follows.

- **A real spinlock** in `include/kernel/spinlock.hpp`: a ticket lock that saves
  and restores `IF`, with `pause` in the spin body. Recursion is not needed if
  the acquisition points are the entry points and only the entry points.
- **Take the BKL on kernel entry, release it on exit.** There are exactly three
  doors, which is why this is tractable: `savanxp_handle_syscall`,
  `savanxp_handle_timer_interrupt`
  ([`timer.cpp:228`](../../arch/x86_64/timer.cpp:228)) and
  `dispatch_external_vector`
  ([`cpu_init.cpp:280`](../../arch/x86_64/cpu_init.cpp:280)).
- **A shared ready queue under the BKL.** `pick_next_runnable()`
  ([`process.cpp:1147`](../../kernel/process.cpp:1147)) must skip processes already
  running on another core, so `Process` grows an owning-CPU field. Without it,
  two cores resume the same `SavedContext` and the process forks in place.
  `has_runnable_non_idle()` ([`process.cpp:1138`](../../kernel/process.cpp:1138))
  needs the same treatment, or an idle core spins waking itself for a process
  another core is already running.
- **A reschedule IPI.** `resched_pending`
  (per core since phase 1, in [`smp::Cpu`](../../include/kernel/smp.hpp))
  currently hands the CPU to a
  woken process on the syscall return path. When the waker and the target are on
  different cores, that becomes an IPI to the core running something less
  urgent — or, in the first cut, nothing at all: the next tick will pick it up,
  at the cost of latency.
- **Release the BKL around the idle `hlt`**, or the idle core holds the lock and
  the machine stops.

Once user processes run on more than one core,
[asynchronous present](../desktop/GRAPHICS_PERF.md#asynchronous-present) is ready to turn
the compositor round trip into real overlap. It was implemented and measured
on one core, and reverted only because one core has nothing to overlap with.

This phase is where the debugging lives. **Estimate: 1-2 weeks.**

## Phase 3 — TLB shootdown — **DONE**

What it actually took, against the plan below: **no IPI at all.**

- **The planned IPI deadlocks under the BKL.** The core that unmaps holds the
  lock; the cores it would wait on are, as often as not, spinning for that same
  lock with `IF=0` — interrupt gates — and never take the IPI. Waiting without
  the lock would work, but it would mean dropping the lock in the middle of an
  unmap.
- **The BKL already gives the ordering the IPI was for.** Every access to kernel
  memory that can be unmapped happens with the lock held, so a core only has to
  be current *before it takes the lock*, not at the moment of the unmap.
  `unmap_kernel_page` bumps a global generation; `smp::lock_kernel()` calls
  `vm::sync_kernel_tlb()`, which flushes this core's TLB (toggling `CR4.PGE`,
  so global pages go too) when its generation is behind. The cost is one
  comparison per lock acquisition and one full flush per core per batch of
  unmaps, which are rare: the compositor's display surface, uACPI regions.
- **Today no correct code needed it** — the audit that preceded the change.
  The kernel mapping window is a bump allocator (`g_kernel_mmio_next` only
  grows), so an unmapped kernel VA is never mapped again; a stale translation
  could only be used by a use-after-unmap bug, which on one core faults and on
  several silently reads freed memory. The generation check turns that back into
  a fault, and it keeps the system correct the day the window starts reusing
  addresses, which it will have to eventually: the window never gives back
  address space, nor the page tables under ranges that were unmapped.
- **User space needs no mechanism, but the reasons are now rules.** The
  argument below holds, and is written down next to `sync_kernel_tlb()` in
  [`vmm.hpp`](../../include/kernel/vmm.hpp): an address space is loaded on at most
  one core, a process switch reloads `CR3`, and only the space of the process
  running *here* or of one running *nowhere* is ever unmapped. Deferring the
  kill of a process that runs on another core (phase 2) is what keeps the last
  part true.
- **Proven at boot, not argued.** `smp::selftest_tlb()` has an AP read a kernel
  page, retargets that page to another physical page on the BSP
  (`vm::retarget_kernel_page`), and reads again: `smp: TLB perezosa ok (antes
  0xa1, sin sincronizar 0xa1, sincronizando 0xb2)`. The unsynchronized read
  returning the old byte is the stale translation, observed under the supported
  accelerators; the synchronized one is the fix.

**Threads ended the user-space half of that argument.** Two threads of one
process on two cores share an address space, so "loaded on at most one core"
no longer holds; phase 2 of [THREADS_ROADMAP.md](THREADS_ROADMAP.md) replaced
it with a user shootdown built the same way: an IPI that nobody waits for, a
flush at the next `lock_kernel()`, and pages freed only after every core
involved has flushed.

**The rule this leaves for phase 4:** the moment any code touches unmappable
kernel memory without the lock, the lazy flush stops being enough, and the IPI
comes back — designed against cores that wait for locks with interrupts
enabled.

The original plan follows.

`map_page`/`unmap_page` invalidate only when the current `CR3` matches the space
being modified ([`vmm.cpp:473`](../../kernel/vmm.cpp:473),
[`vmm.cpp:499`](../../kernel/vmm.cpp:499)). Two different exposures:

- **User space: mostly safe by construction.** There were no threads when this
  was planned, so an address space was loaded on at most one core at a time, and
  every unmap path runs on behalf of the process that owns the space while
  holding the BKL. A `CR3` reload flushes non-global entries, so a core that ran
  the process earlier does not keep stale entries. Native threads lifted the
  one-core rule with a user-space shootdown
  ([THREADS_ROADMAP.md](THREADS_ROADMAP.md#phase-2--the-address-space-on-many-cores--done)).
- **Kernel space: a real requirement.** `create_address_space()` copies the
  kernel PML4 entries into every new space
  ([`vmm.cpp:406`](../../kernel/vmm.cpp:406)) — those entries point at *shared*
  lower tables, so a kernel mapping change propagates to every space and every
  core. And `map_kernel_pages`/`unmap_kernel_pages`
  ([`vmm.cpp:698`](../../kernel/vmm.cpp:698)) are called **at runtime**, not only
  during init: the GPU drivers map and unmap surface backing on every import and
  destroy ([`virtio_gpu.cpp:2269`](../../kernel/virtio_gpu.cpp:2269),
  [`fb_gpu.cpp:667`](../../kernel/fb_gpu.cpp:667)).

So: an invalidation IPI, broadcast on kernel-range changes, with the initiator
waiting for acknowledgement before it reuses the physical page. **Estimate: 2-4
days.**

## Phase 4 — the debt around the BKL, and a way to measure it — **DONE**

**Goal, as narrowed when it started:** close what phase 2 left owed — a panic
that stops only one core, uACPI's placeholder locks, every device interrupt on
the BSP — and build the measurement the rule below asks for. **Split nothing
without it.** The only contention ever measured (`windowd-smoke --smp 4`
failing on the lock held across ATA PIO, [above](#phase-2--the-big-kernel-lock-and-the-multi-core-scheduler--done))
no longer reproduced when the phase started: the disk is AHCI with DMA now,
and the test passes.

What it actually took:

- **A panic stops every core.** `panic()` and a ring-0 exception call
  `smp::stop_other_cores()` before printing: one core claims the stop with a
  compare-and-swap, sends every other online core an NMI (`arch::send_nmi`,
  delivery mode NMI on the same ICR path as `send_ipi`) and waits, bounded,
  for each to acknowledge. The NMI is what makes it work: a core spinning for
  the kernel lock the panicking core still holds has `IF=0` and would never
  take a normal IPI. The NMI handler checks `smp::stopping()` before anything
  else and halts with NMIs blocked. Two cores failing at once: the loser halts
  quietly and the winner reports. The panic ends with
  `smp: other cores stopped (n/n)`, or `NOT stopped (k/n)` if some did not
  answer. `panic-smoke` crashes the kernel on purpose through
  `POWER_IOC_PANIC` on `/dev/power` (system identity only) and waits for that
  line: 1/1 on two cores, 3/3 on four, in both APIC modes.
- **The lock counts its contention, per door.** `smp::lock_kernel(Door)`
  records, for syscall, timer, reschedule IPI, device IRQ, ring-3 fault, the
  retake after `wait_for_interrupt` (`wake`) and the scheduler start
  (`other`): acquisitions, how many found the lock taken, cycles waited and
  cycles held. `sync::acquire` now says whether it waited. The BSP's tick prints
  it next to `timer-stats:`, every 2000 ticks, [read like this](#reading-smp-stats).
  The counters live under the lock they measure, so they cost two `rdtsc` per
  acquisition and no atomics.
- **The first thing it measured was not the kernel: it was `pause` under TCG.**
  With four cores under TCG the smoke timed out, the guest saw 75-83% of its
  ticks lost, and `smptest`'s pipe ping-pong took 37 s (the 1.1 s measured in
  phase 2 was on another host and QEMU). The counters said the lock was
  *held* 22% of the time while the cores spent 92% of theirs *waiting* for it:
  a lock mostly free that nobody manages to take. QEMU's TCG emulates `pause`
  by leaving translated code through its global lock, so four cores spinning
  with `pause` queue behind each other inside QEMU, holder included. Without
  `pause` in the spin, the same smoke passes in 35 s and the ping-pong takes
  0.37 s. `pause` stays on hardware and under KVM, where it is what lets KVM's
  pause-loop exiting run a preempted holder instead of the spinner (0.023 s for
  the ping-pong there): `arch::lock_spin_hint()` drops it only when CPUID's
  hypervisor leaf says `TCGTCGTCGTCG` (`cpu: QEMU TCG, the kernel lock spins
  without pause`). Only the lock's spin changes; device polling keeps `pause`.
- **uACPI has real locks.** Spinlocks are ticket locks that clear `IF` first, as
  the API asks; mutexes take a compare-and-swap and spin until their timeout;
  events count with atomics; and `uacpi_kernel_get_thread_id` returns the
  running thread (a per-core number during boot, before there is one). Under the
  BKL none of them ever waits — whoever holds a uACPI lock is inside uACPI, and
  so holds the kernel lock — so they are what keeps uACPI correct the day it
  runs without the BKL, not a fix for today. An infinite mutex wait that lasts
  5 s panics: by then two cores are waiting on each other.
- **Device interrupts go to the APs.** `route_gsi()` still targets the core that
  routes, which during boot is the BSP; `smp::start_scheduling()` then calls
  `ioapic::spread()`, which rewrites each routed GSI's destination round-robin
  over the scheduling APs (`ioapic: GSI 1 (vector 51) -> lapic 1`). Not earlier:
  a parked AP that took a device IRQ would run the driver alongside a boot that
  holds no lock. The BSP is left out because its tick already does the
  system's periodic work. A keyboard IRQ was observed handled on core 1, and
  `kbd-smoke` passes. Two limits, both written next to `ioapic::kMaxDestination`:
  - **An APIC ID above 254 gets no device interrupts.** An IOAPIC entry names
    its destination in 8 bits (255 is broadcast). Logical destination mode, the
    remedy the original plan named, does not help in x2APIC mode, where logical
    IDs are 32 bits; the remedy is interrupt remapping (VT-d / AMD-Vi), which
    this kernel does not have. Such cores are skipped.
  - **MSI-X stays on the BSP.** virtio-gpu programs its MSI-X entry at boot,
    before the APs schedule; moving it means rewriting that entry afterwards,
    and nothing measured asks for it.
- **A bug from v0.4.0: no SMEP, no APs.** Verifying in xAPIC mode with
  `-cpu qemu64` found every AP offline. `ap_initialize_cpu()` required SMEP
  outright since the commit that enabled it, while the BSP treats it as
  optional; on a processor without SMEP the system had quietly run on one core.
  The APs now mirror the BSP ([KERNEL_SECURITY.md](KERNEL_SECURITY.md)).
  `-cpu max,-x2apic` is the better way to test xAPIC: it drops only the x2APIC.

**Not done, on purpose: splitting the BKL.** Each subsystem is split when
`smp-stats` shows it is the one, and not before. What the counters showed when
this phase closed is [below](#what-the-counters-showed); the candidates, in the
order the global counts suggest, and what each split must respect, are the
original plan that follows. One rule from phase 3 travels with every split: the
moment code touches unmappable kernel memory without the lock, the lazy TLB
flush stops being enough and the IPI comes back.

### Reading `smp-stats`

```
smp-stats: window=3826 ms acq=24559 contended=10% wait=38% of 4 cores (max 689221 us) held=54% (max 690095 us, syscall #59 posix)
smp-stats: doors acq/wait ms/held ms: syscall=8675/4313/2477 timer=8039/2945/69 resched=0/0/0 irq=0/0/0 fault=447/0/4 wake=7316/60/7 other=0/0/0
```

- `contended` is the share of acquisitions that found the lock taken.
- `wait` is time spent spinning, against **every** scheduling core's time:
  100% would be all cores spinning for the whole window.
- `held` is time the lock was held, against **one** core's time: it is held by
  one core at a time, so 100% means it was never free. A `held` near 100% is a
  lock that is the bottleneck; a `wait` far above `held` is a lock that is free
  but not being taken, which is what the TCG `pause` problem looked like.
- The longest hold names its door and, for a syscall, its number and subsystem
  (`handle_syscall` tags it); `#59 posix` is `SAVANXP_SYS_FSCHECK`.
- The second line splits acquisitions, milliseconds waited and milliseconds held
  by door. Silent on one core.

### What the counters showed

Measured on a Ryzen 5 1600 (12 logical CPUs), QEMU 11.1, when phase 4 closed.
Windows of 2000 ticks; ranges are over the windows of one run (for `smoke`,
over the first, busiest window of two runs).

| Run | `contended` | `wait` | `held` | Longest hold |
| --- | --- | --- | --- | --- |
| `windowd-smoke`, KVM, 2 cores | 28-36% | 15-21% of 2 | 46-84% | up to 76 ms, `SXN_SYS_SPAWN_STDIO` |
| `windowd-smoke`, KVM, 4 cores | 28-56% | 1-40% of 4 | 6-87% | up to 67 ms, `SXN_SYS_SPAWN_STDIO` |
| `smoke`, KVM, 4 cores | 10-22% | 38-43% of 4 | 54-60% | 0.7-0.8 s, `SAVANXP_SYS_FSCHECK` |

- **The desktop smoke keeps the lock busy.** While `windowd-smoke` launches
  its programs one after another, the lock is held up to 87% of the time, and
  the longest holds are program launches: `SXN_SYS_SPAWN_STDIO` reads the
  image from disk and maps it without letting go, 50-76 ms each. Next come
  `SXN_SYS_DEVICE_CONTROL` calls, the compositor's GPU requests, up to 12 ms.
  Those are the first candidates the counters name: image loading (storage and
  the VFS under it), then the GPU path. In the quiet window after the launches,
  `held` drops to 6%.
- **`SYS_FSCHECK` is the longest single hold**, 0.7-0.8 s: the smoke's
  `fscheck` walks the whole filesystem in one syscall. A test tool, not a
  workload; it is the reason the `smoke` row's maximum is not worth chasing.
- **Under TCG, `wait` overstates.** On two cores under TCG, `taskbar-smoke`
  showed `wait=53%` of two cores with `held=29%` — more waiting than there was
  holding to wait for, which two honest cores cannot produce. Under KVM the
  same smokes keep `wait` below `held`. The likely mechanism, not confirmed in
  QEMU's source: a vCPU spinning with `IF=0` while an interrupt is pending goes
  through QEMU's global lock on every block of translated code, and stalls there
  with the kernel lock already free. Read TCG's `wait` as an upper bound and act
  on KVM's.

The original plan follows.

Open-ended, and deliberately last. The BKL is correct; it just does not scale.
Splitting it is a per-subsystem campaign, and each subsystem should only be
split once there is a measurement showing it is the contended one.

The order suggested by the global counts (`kernel/` and `arch/` hold roughly 450
mutable globals) is: `physical_memory` (`g_regions`), `heap` (`g_first_arena`),
the handle table in `object`, pipes, `vfs`/`sxfs`, `tty`/`console`, `clipboard`,
`net`, then the drivers.

**The drivers are the bulk of the work, not the core kernel.**
[`virtio_gpu.cpp`](../../kernel/virtio_gpu.cpp) is 3876 lines with 47 globals, a
hand-rolled `AtomicSectionGuard` whose spin limits have never actually contended
([`virtio_gpu.cpp:577`](../../kernel/virtio_gpu.cpp:577)) and a recovery path that
assumes it is alone. [`ps2.cpp`](../../kernel/ps2.cpp) has 40 globals.
[`rtl8139.cpp:182`](../../kernel/rtl8139.cpp:182) states outright that its critical
section is only atomic on a single core. The AC'97 driver is polling with no IRQ
of its own, and its feed loop is written against a scheduler that cannot preempt
it.

Also in this phase: real spinlocks and mutexes for uACPI
([`uacpi_glue.cpp:428`](../../kernel/uacpi_glue.cpp:428)), and IOAPIC routing that
picks a destination CPU instead of pinning to the BSP
([`ioapic.cpp:262`](../../kernel/ioapic.cpp:262)) — including logical destination
mode, since an x2APIC ID does not necessarily fit in the 8 bits the current
write uses.

## Verification

The existing smoke suite runs on one core and will therefore keep passing while
being blind to every race introduced. Phases 0-3 need `--smp` variants:

- `build.sh` gains an `--smp <n>` switch that reaches the QEMU argument list.
  **Done in phase 0**; it feeds both the interactive and the smoke
  invocations. It defaulted to 1 until the switch to two cores below.
- At least `smoke`, `windowd-smoke` and `sxfs-smoke` get an `--smp 4` run. The
  filesystem and compositor smokes are the ones that actually exercise shared
  state from several processes at once. **Done in phase 2**, with the caveat
  about host CPUs above; `smoke` also carries `smptest`, which fails when the
  cores reported as scheduling do not actually run processes at the same time.
- The automated smokes honour `--accel`, so the same suite runs under TCG or
  KVM. **Done in phase 2.**
- TCG stays the accelerator for the smokes, for determinism — but note that TCG
  serializes far more than real hardware does, so a clean TCG run is weak
  evidence. A KVM run (`--accel kvm`) is where memory-ordering bugs surface.
- `panic-smoke` panics the kernel with every core running and fails unless the
  others acknowledge the stop. **Added in phase 4.**

## Two cores by default

The default went from one core to two once every smoke had passed on two.
Measured under TCG on the development host (an i5-2400, 4 logical CPUs):

- **All 51 QEMU smokes pass with `--smp 2`** (`pointer-smoke` with the
  `--virtio` it requires), and the timing-sensitive ones (`smoke`,
  `windowd-smoke`, `session-smoke`, `elevation-smoke`, `native-hw-smoke`,
  `kbd-smoke`, `pthread-test`, `taskmgr-smoke`) passed two more times each.
  The slowest two, `sxgui-smoke` and `taskbar-smoke`, take the same ~170 s on
  one core: their waits are fixed.
- **The display path got faster, not slower.** The
  [`spin` scenario](../desktop/GRAPHICS_PERF.md#which-workload-measures-what)
  composed about 1 240 fps on one core and 2 200 to 2 600 on two, read from
  `compositord-stats`: `windowd` and `compositord` now overlap instead of
  taking turns. The same-core handoff of a synchronous RPC is unchanged.

Two and not more: `--smp 4` on a 4-CPU host still fails `windowd-smoke` (the
lock held across disk I/O, [above](#phase-2--the-big-kernel-lock-and-the-multi-core-scheduler--done)),
and half the host's CPUs leaves the thread that emulates the disk room to run.
VirtualBox is unaffected: its core count is the VM's setting.

Phase 4 revisited both reasons on a Ryzen 5 1600 (12 logical CPUs), with AHCI
as the default disk: `windowd-smoke --smp 4` passes, and so does `smoke
--smp 4` under TCG and KVM — under TCG only since the lock stopped spinning
with `pause` there ([phase 4](#phase-4--the-debt-around-the-bkl-and-a-way-to-measure-it--done)).
Two stays the default: it is what the whole suite is verified on.

## Known traps

- **VirtualBox.** The APIC there has already cost one round of debugging (the
  I/O APIC masking LINT0, which required xAPIC-over-MMIO support and a restored
  virtual wire). AP bring-up under VirtualBox should be treated as its own
  investigation, not as a footnote to the QEMU work.
- **`pause` under TCG.** QEMU's TCG emulates `pause` by leaving translated code
  through its global lock, so a spin loop with `pause` on several vCPUs
  serializes them inside QEMU, lock holder included. The kernel lock detects
  TCG and spins without it ([phase 4](#phase-4--the-debt-around-the-bkl-and-a-way-to-measure-it--done)).
  A new lock that spins should use `arch::lock_spin_hint()`, not `cpu_relax()`.
- **The RAM budget.** A kernel stack, a GDT and a TSS per CPU are small, but an
  idle process per core costs a real process slot each. The machine runs in
  512 MiB, and the boot log puts the usable figure at 368 MiB with another
  46 MiB reclaimable.
- **`kMaxProcesses` and the fd budget** are unchanged by SMP, but a scheduler
  that actually runs four processes at once makes the existing ceilings easier
  to hit.
- **Phase 2's first cut for the reschedule IPI undoes a measured fix.** Every
  synchronous RPC on the desktop — `windowd` writing a request to
  `compositord` and blocking on the reply — relies on `resched_pending`
  handing the CPU to the woken reader on the same core. Before pipe completions
  requested it, that round trip was 99% of the cost of a frame: 205 ms against
  127 µs of GPU work on a 672-pixel frame. After, present got ~470x faster
  and Doom went to 35 FPS in VirtualBox
  ([numbers](../desktop/GRAPHICS_PERF.md#what-that-bought)). If `windowd` and
  `compositord` land on different cores and the wakeup waits for the next tick,
  that regression comes back. Implement the IPI in the same phase, or keep the
  two on one core until it exists, and rerun
  [the `spin` scenario](../desktop/GRAPHICS_PERF.md#which-workload-measures-what) to check.
  Phase 1 kept the one-core half intact: `resched_pending` lives in
  `smp::Cpu`, the waker marks its own core, and that core still reschedules
  on the syscall return. What phase 2 adds is the cross-core half.

  **What phase 2 measured.** The first cut sent a woken process to an idle core
  by IPI instead of handing over the waker's core. With `smptest`'s pipe
  ping-pong (2000 round trips, the shape of a synchronous RPC) under TCG, that
  took 1791 ms on 4 cores against 544 ms on one: the waker blocks on the reply
  right away, so the handoff was the shorter path all along. Keeping the
  handoff brought TCG to ~1100 ms, and neither an IPI for the displaced waker
  nor a cache-hot migration delay improved on it. The rest is TCG itself: under
  hardware acceleration the same test is much faster, so wakeups keep the
  one-core behaviour and nothing else; if a real workload ever shows the
  displaced waker waiting for a tick, that is the measurement to start from.

## Is it worth it?

Honestly: not yet, and the reason is worth writing down.

**There were no threads** when this was written. A process was one context,
so SMP parallelized *distinct processes* and nothing else; native threads
([THREADS_ROADMAP.md](THREADS_ROADMAP.md)) now run in parallel too, and POSIX
gets them with pthreads. The heaviest consumer in the
system, Doom, is single-threaded — it does not get faster on four cores. What
would genuinely improve is latency across the desktop: the compositor painting
on one core while an application computes on another, instead of taking turns
through a single round-robin queue.

That makes phases 0-3 worth doing on their own terms — roughly **3-5 weeks** of
focused work for a correct multi-core kernel with a big lock — while splitting
the lock should wait for either threads or a measurement proving that a specific
subsystem is the bottleneck. Splitting locks before there is contention to split
is how a kernel acquires races it cannot reproduce.

Threads have landed, and phase 4 built the measurement. Its first reading
names program loading and the GPU path as where the lock is held longest, but
under a smoke that launches a dozen programs back to back
([numbers](#what-the-counters-showed)); whether an interactive desktop feels
it is the next measurement, and the one to take before splitting anything.
