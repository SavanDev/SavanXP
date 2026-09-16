# SMP — running SavanXP on more than one core

> **Status: phases 0 to 3 done, on master. Phase 4 not started.** This document is
> the measurement: what the kernel already has, what it is missing, and in what
> order to attack it so that every phase boots and can be verified on its own.
>
> Phase 0 landed as `smp::` ([smp.hpp](../include/kernel/smp.hpp),
> [smp.cpp](../arch/x86_64/smp.cpp)): the APs start, identify themselves by
> their own LAPIC ID and park in a `hlt` loop, and a ping IPI proves the ICR
> delivers. Verified on 4 cores in **both** APIC modes — x2APIC (`-cpu max`)
> and xAPIC (`-cpu qemu64`, the VirtualBox path). Nothing schedules on them:
> from the outside the system behaves exactly as it did on one core.
>
> Phase 1 gave every core its own TSS and its own `smp::Cpu` (current process,
> idle, reschedule request), indexed by the TSS selector. Still nothing
> schedules on the APs.
>
> Phase 2 made every core run user processes, one at a time inside the kernel,
> under a single big kernel lock. `build.ps1 -Smp <n>` still defaults to one
> core.
>
> Phase 3 keeps the other cores' TLBs honest when a kernel page is unmapped —
> lazily, at lock acquisition, not with an IPI
> ([why](#phase-3--tlb-shootdown)). A boot self-test catches an AP reading a
> stale translation and proves the flush removes it.

The short version: **bringing up the other cores is the cheap part.** The
kernel has around 450 mutable globals and no lock discipline at all, so the
expensive part is deciding what protects them — and the answer, for a long
while, should be "one lock".

## Two structural advantages worth naming first

Two decisions already in the tree make SMP far cheaper here than in a typical
hobby kernel. Both should be defended rather than traded away.

**1. The kernel is already non-preemptible.** Every IDT gate is an interrupt
gate ([`cpu_init.cpp:16`](../arch/x86_64/cpu_init.cpp:16)), the syscall gate at
vector `0x80` included, and nothing in the syscall path re-enables interrupts.
`IF` is 0 from the moment a process enters the kernel until it leaves. That is
a big kernel lock, implicitly held, with no code written to implement it. Phase
2 is not introducing a new concurrency model; it is writing down the one that
already exists and making it hold across cores.

**2. Syscalls never block on the kernel stack.** A syscall that cannot complete
records a state and returns `kBlockedResult`
([`process.cpp:47`](../kernel/process.cpp:47)); the waker then **retries the
operation on the sleeper's behalf** and completes it — see
`wake_blocked_readers_for_pipe()`
([`process.cpp:1377`](../kernel/process.cpp:1377)), which re-runs `try_pipe_read`
for every process parked on that pipe. There are no kernel threads halted
halfway down a call stack, no sleep queues holding kernel context, and no
stack-switching context switch: the scheduler picks a `SavedContext` and
`iretq`s into it (`pick_next_runnable()`,
[`process.cpp:1147`](../kernel/process.cpp:1147)). A second core therefore needs
no second copy of anything the scheduler owns — only its own idea of which
process it is running.

## Inventory

### Already there

- **Local APIC**, in both modes: xAPIC through MMIO and x2APIC through MSRs,
  behind one pair of accessors
  ([`cpu_init.cpp:191`](../arch/x86_64/cpu_init.cpp:191)). `local_apic_id()` is
  already exported.
- **A calibrated periodic LAPIC timer** with a PIT fallback
  ([`timer.cpp`](../arch/x86_64/timer.cpp)), and a tick handler that already
  returns the next context to run
  ([`timer.cpp:210`](../arch/x86_64/timer.cpp:210)).
- **MADT parsing and IOAPIC programming** ([`ioapic.cpp`](../kernel/ioapic.cpp)),
  with interrupt source overrides resolved.
- **FPU/SSE state per process**, saved and restored on switch
  ([`cpu.hpp`](../include/kernel/cpu.hpp)) — already per-context, not per-CPU.

### Missing

| Gap | Where |
| --- | --- |
| No IPIs. The ICR is unimplemented, and the code says why: "no consumers while there is no SMP" | [`cpu_init.cpp:178`](../arch/x86_64/cpu_init.cpp:178) |
| One global GDT, one global TSS. `set_kernel_stack()` writes the single `g_tss.rsp0` on every switch | [`cpu_init.cpp:117`](../arch/x86_64/cpu_init.cpp:117), [`cpu_init.cpp:768`](../arch/x86_64/cpu_init.cpp:768) |
| No per-CPU infrastructure: no `struct Cpu`, no GS base, no way to ask "which core am I" beyond the LAPIC ID | — |
| `parse_madt()` only handles type 1 (IOAPIC) and type 2 (ISO). Type 0 (Local APIC) — the CPU list — is skipped | [`ioapic.cpp:57`](../kernel/ioapic.cpp:57), [`ioapic.cpp:143`](../kernel/ioapic.cpp:143) |
| GSIs are routed to the BSP in physical mode, hardcoded | [`ioapic.cpp:262`](../kernel/ioapic.cpp:262) |
| No spinlock primitive. The only lock in the kernel is a bare `__atomic_test_and_set` in SxFS | [`sxfs.cpp:148`](../kernel/sxfs.cpp:148) |
| uACPI's spinlocks and mutexes are stubs that only save and restore `IF` | [`uacpi_glue.cpp:428`](../kernel/uacpi_glue.cpp:428) |
| The vendored `limine.h` is a 197-line subset with no `limine_mp_request` | [`vendor/limine.h`](../vendor/limine.h) |
| QEMU is launched without `-smp`; every smoke runs on one core | [`build.ps1`](../build.ps1) |
| Scheduler state is global and singular: `g_current`, `g_idle`, `g_schedule_cursor`, `g_resched_pending` | [`process.cpp:73`](../kernel/process.cpp:73) |

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
  ([`physical_memory.cpp:117`](../kernel/physical_memory.cpp:117)). That removed
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
`smp: ping IPI 3/3 ok` under `build.ps1 -Smp 4`.

The original plan follows.

1. Enumerate the CPUs. Two options: add `limine_mp_request` to the vendored
   header, or extend `parse_madt()` with entry type 0. The Limine path also
   hands over the APs already in long mode with their own stacks, which saves
   writing a real-mode trampoline and the identity mapping it needs — that is
   the single biggest saving available in this phase, and the reason to prefer
   it.
2. Write `send_ipi()` in `arch::x86_64`, covering both APIC modes. The 64-bit
   x2APIC ICR is one MSR write; the xAPIC one is two MMIO writes, high word
   first, and needs the delivery-status poll.
3. Give each AP a kernel stack and park it.

**Deliverable:** `cpu: 3 APs online` in the boot log under `-smp 4`, with the
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
  (`arch::x86_64::cpu_index()`). The LAPIC ID is an MSR or MMIO read that can
  exit the VM under KVM or WHPX on every access, and the current process is
  read dozens of times per syscall. `ltr` is privileged, so user space cannot
  change the answer. Each AP reads its selector back at boot and the BSP checks
  it: `smp: TSS propio en 3/3 APs ok`.
- **`smp::Cpu` holds `current`, `idle` and `resched_pending`.** The 162
  references in `process.cpp` and both `syscall_dispatch.inc` now go through
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
  ([`context.S`](../arch/x86_64/context.S)). Take the simple version first; a
  GS-based fast path is an optimization for later, and one that has to be
  designed against interrupt gates rather than `syscall`/`sysret`.
- **An idle process per CPU.** Today there is one `g_idle`
  ([`process.cpp:75`](../kernel/process.cpp:75)); every core needs something to
  run when the ready queue is empty.
- Move `g_current` into `struct Cpu`. This is the change that touches the most
  call sites, and it is mechanical.

**Estimate: 3-5 days.**

## Phase 2 — the big kernel lock and the multi-core scheduler — **DONE**

**Goal:** all cores run user processes. Correct, with essentially no scaling.

What it actually took, against the plan below:

- **Five doors, not three.** The lock (`smp::lock_kernel()`, a ticket
  spinlock from [`spinlock.hpp`](../include/kernel/spinlock.hpp)) is taken by
  the syscall, timer and reschedule-IPI stubs in
  [`context.S`](../arch/x86_64/context.S), by `dispatch_external_vector` for
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
  (`runnable_here()` in [`process.cpp`](../kernel/process.cpp)): the kernel code
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
  `kill_pending` and sends the reschedule IPI; the owning core terminates it on
  its next return to ring 3 — the IPI itself, the next tick, or the end of the
  syscall it was in.
- **No reschedule IPI for wakeups.** See
  [the trap below](#known-traps): the same-core handoff stays, and an idle core
  picks up whatever is left in the queue on its own tick, at most 1 ms later.
  The IPI exists, but only kills use it.
- **CPU accounting counts every core.** `cpu_ticks_total` is now the ticks that
  found a process running on *any* core, so one tick still increments exactly
  one process; `cpu_online` is the number of cores that schedule, which is also
  the number of idle processes. [Details](SYSTEM_MONITORING.md#more-than-one-core).

**Delivered:** `smp: planificando en 4 de 4 cores` under `build.ps1 -Smp 4`,
and `smptest` in the smoke suite: it sees a child in `State::running` from
inside its own syscall — impossible on one core — kills a child spinning on
another core, and runs a pipe ping-pong.

**One measurement to keep: the lock is held across disk I/O.** On the
development host (an i5-2400, 4 logical CPUs) `windowd-smoke` passes at
`-Smp 2` under TCG and WHPX but fails at `-Smp 4` under both: its size-hint
check assumes `progman` finishes its startup scan of `/disk` within 60 frames.
That scan is ATA PIO inside syscalls, under the lock; with four vCPUs on four
host CPUs, the cores spinning for the lock take host time from the thread that
emulates the disk, and `progman` measured more than 1.8 s of CPU without
getting there. Nothing is lost or corrupted — it is phase 4's problem showing
up early, and the reason not to size `-Smp` to every host CPU.

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
  ([`timer.cpp:228`](../arch/x86_64/timer.cpp:228)) and
  `dispatch_external_vector`
  ([`cpu_init.cpp:280`](../arch/x86_64/cpu_init.cpp:280)).
- **A shared ready queue under the BKL.** `pick_next_runnable()`
  ([`process.cpp:1147`](../kernel/process.cpp:1147)) must skip processes already
  running on another core, so `Process` grows an owning-CPU field. Without it,
  two cores resume the same `SavedContext` and the process forks in place.
  `has_runnable_non_idle()` ([`process.cpp:1138`](../kernel/process.cpp:1138))
  needs the same treatment, or an idle core spins waking itself for a process
  another core is already running.
- **A reschedule IPI.** `resched_pending`
  (per core since phase 1, in [`smp::Cpu`](../include/kernel/smp.hpp))
  currently hands the CPU to a
  woken process on the syscall return path. When the waker and the target are on
  different cores, that becomes an IPI to the core running something less
  urgent — or, in the first cut, nothing at all: the next tick will pick it up,
  at the cost of latency.
- **Release the BKL around the idle `hlt`**, or the idle core holds the lock and
  the machine stops.

Once user processes run on more than one core,
[asynchronous present](GRAPHICS_PERF.md#asynchronous-present) is ready to turn
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
  [`vmm.hpp`](../include/kernel/vmm.hpp): an address space is loaded on at most
  one core, a process switch reloads `CR3`, and only the space of the process
  running *here* or of one running *nowhere* is ever unmapped. Deferring the
  kill of a process that runs on another core (phase 2) is what keeps the last
  part true.
- **Proven at boot, not argued.** `smp::selftest_tlb()` has an AP read a kernel
  page, retargets that page to another physical page on the BSP
  (`vm::retarget_kernel_page`), and reads again: `smp: TLB perezosa ok (antes
  0xa1, sin sincronizar 0xa1, sincronizando 0xb2)`. The unsynchronized read
  returning the old byte is the stale translation, observed under both TCG and
  WHPX; the synchronized one is the fix.

**The rule this leaves for phase 4:** the moment any code touches unmappable
kernel memory without the lock, the lazy flush stops being enough, and the IPI
comes back — designed against cores that wait for locks with interrupts
enabled.

The original plan follows.

`map_page`/`unmap_page` invalidate only when the current `CR3` matches the space
being modified ([`vmm.cpp:473`](../kernel/vmm.cpp:473),
[`vmm.cpp:499`](../kernel/vmm.cpp:499)). Two different exposures:

- **User space: mostly safe by construction.** There are no threads, so an
  address space is loaded on at most one core at a time, and every unmap path
  runs on behalf of the process that owns the space while holding the BKL. A
  `CR3` reload flushes non-global entries, so a core that ran the process
  earlier does not keep stale entries.
- **Kernel space: a real requirement.** `create_address_space()` copies the
  kernel PML4 entries into every new space
  ([`vmm.cpp:406`](../kernel/vmm.cpp:406)) — those entries point at *shared*
  lower tables, so a kernel mapping change propagates to every space and every
  core. And `map_kernel_pages`/`unmap_kernel_pages`
  ([`vmm.cpp:698`](../kernel/vmm.cpp:698)) are called **at runtime**, not only
  during init: the GPU drivers map and unmap surface backing on every import and
  destroy ([`virtio_gpu.cpp:2269`](../kernel/virtio_gpu.cpp:2269),
  [`fb_gpu.cpp:667`](../kernel/fb_gpu.cpp:667)).

So: an invalidation IPI, broadcast on kernel-range changes, with the initiator
waiting for acknowledgement before it reuses the physical page. **Estimate: 2-4
days.**

## Phase 4 — dismantling the BKL

Open-ended, and deliberately last. The BKL is correct; it just does not scale.
Splitting it is a per-subsystem campaign, and each subsystem should only be
split once there is a measurement showing it is the contended one.

The order suggested by the global counts (`kernel/` and `arch/` hold roughly 450
mutable globals) is: `physical_memory` (`g_regions`), `heap` (`g_first_arena`),
the handle table in `object`, pipes, `vfs`/`sxfs`, `tty`/`console`, `clipboard`,
`net`, then the drivers.

**The drivers are the bulk of the work, not the core kernel.**
[`virtio_gpu.cpp`](../kernel/virtio_gpu.cpp) is 3876 lines with 47 globals, a
hand-rolled `AtomicSectionGuard` whose spin limits have never actually contended
([`virtio_gpu.cpp:577`](../kernel/virtio_gpu.cpp:577)) and a recovery path that
assumes it is alone. [`ps2.cpp`](../kernel/ps2.cpp) has 40 globals.
[`rtl8139.cpp:182`](../kernel/rtl8139.cpp:182) states outright that its critical
section is only atomic on a single core. The AC'97 driver is polling with no IRQ
of its own, and its feed loop is written against a scheduler that cannot preempt
it.

Also in this phase: real spinlocks and mutexes for uACPI
([`uacpi_glue.cpp:428`](../kernel/uacpi_glue.cpp:428)), and IOAPIC routing that
picks a destination CPU instead of pinning to the BSP
([`ioapic.cpp:262`](../kernel/ioapic.cpp:262)) — including logical destination
mode, since an x2APIC ID does not necessarily fit in the 8 bits the current
write uses.

## Verification

The existing smoke suite runs on one core and will therefore keep passing while
being blind to every race introduced. Phases 0-3 need `-smp` variants:

- `build.ps1` gains an `-Smp <n>` switch that reaches the QEMU argument list,
  defaulting to 1 so nothing existing changes. **Done in phase 0**; it feeds
  both the interactive and the smoke invocations.
- At least `smoke`, `windowd-smoke` and `sxfs-smoke` get an `-smp 4` run. The
  filesystem and compositor smokes are the ones that actually exercise shared
  state from several processes at once. **Done in phase 2**, with the caveat
  about host CPUs above; `smoke` also carries `smptest`, which fails when the
  cores reported as scheduling do not actually run processes at the same time.
- The automated smokes honour `-Accel`, so the same suite runs under WHPX or
  KVM. **Done in phase 2.**
- TCG stays the accelerator for the smokes, for determinism — but note that TCG
  serializes far more than real hardware does, so a clean TCG run is weak
  evidence. A KVM run (`-Accel kvm`) is where memory-ordering bugs surface.

## Known traps

- **VirtualBox.** The APIC there has already cost one round of debugging (the
  I/O APIC masking LINT0, which required xAPIC-over-MMIO support and a restored
  virtual wire). AP bring-up under VirtualBox should be treated as its own
  investigation, not as a footnote to the QEMU work.
- **The RAM budget.** A kernel stack, a GDT and a TSS per CPU are small, but the
  machine runs in 256 MiB with roughly 133 MiB usable, and an idle process per
  core costs a real process slot each.
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
  ([numbers](GRAPHICS_PERF.md#what-that-bought)). If `windowd` and
  `compositord` land on different cores and the wakeup waits for the next tick,
  that regression comes back. Implement the IPI in the same phase, or keep the
  two on one core until it exists, and rerun
  [the `spin` scenario](GRAPHICS_PERF.md#which-workload-measures-what) to check.
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
  WHPX the same test takes 15 ms on one core and 16 ms on four. So wakeups keep
  the one-core behaviour and nothing else; if a real workload ever shows the
  displaced waker waiting for a tick, that is the measurement to start from.

## Is it worth it?

Honestly: not yet, and the reason is worth writing down.

**There are no threads.** A process is one context
([`process.cpp`](../kernel/process.cpp) has no thread concept), so SMP
parallelizes *distinct processes* and nothing else. The heaviest consumer in the
system, Doom, is single-threaded — it does not get faster on four cores. What
would genuinely improve is latency across the desktop: the compositor painting
on one core while an application computes on another, instead of taking turns
through a single round-robin queue.

That makes phases 0-3 worth doing on their own terms — roughly **3-5 weeks** of
focused work for a correct multi-core kernel with a big lock — while phase 4
should wait for either threads or a measurement proving that a specific
subsystem is the bottleneck. Splitting locks before there is contention to split
is how a kernel acquires races it cannot reproduce.
