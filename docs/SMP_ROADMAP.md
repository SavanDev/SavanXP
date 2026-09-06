# SMP — running SavanXP on more than one core

> **Status: not started.** Nothing described here is implemented. This document
> is the measurement: what the kernel already has, what it is missing, and in
> what order to attack it so that every phase boots and can be verified on its
> own.

The short version: **bringing up the other cores is the cheap part.** The
kernel has around 450 mutable globals and no lock discipline at all, so the
expensive part is deciding what protects them — and the answer, for a long
while, should be "one lock".

## Two structural advantages worth naming first

Two decisions already in the tree make SMP far cheaper here than in a typical
hobby kernel. Both should be defended rather than traded away.

**1. The kernel is already non-preemptible.** Every IDT gate is an interrupt
gate ([`cpu_init.cpp:17`](../arch/x86_64/cpu_init.cpp:17)), the syscall gate at
vector `0x80` included, and nothing in the syscall path re-enables interrupts.
`IF` is 0 from the moment a process enters the kernel until it leaves. That is
a big kernel lock, implicitly held, with no code written to implement it. Phase
2 is not introducing a new concurrency model; it is writing down the one that
already exists and making it hold across cores.

**2. Syscalls never block on the kernel stack.** A syscall that cannot complete
records a state and returns `kBlockedResult`
([`process.cpp:46`](../kernel/process.cpp:46)); the waker then **retries the
operation on the sleeper's behalf** and completes it — see
`wake_blocked_readers_for_pipe()`
([`process.cpp:1358`](../kernel/process.cpp:1358)), which re-runs `try_pipe_read`
for every process parked on that pipe. There are no kernel threads halted
halfway down a call stack, no sleep queues holding kernel context, and no
stack-switching context switch: the scheduler picks a `SavedContext` and
`iretq`s into it (`pick_next_runnable()`,
[`process.cpp:1142`](../kernel/process.cpp:1142)). A second core therefore needs
no second copy of anything the scheduler owns — only its own idea of which
process it is running.

## Inventory

### Already there

- **Local APIC**, in both modes: xAPIC through MMIO and x2APIC through MSRs,
  behind one pair of accessors
  ([`cpu_init.cpp:179`](../arch/x86_64/cpu_init.cpp:179)). `local_apic_id()` is
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

## Phase 0 — bring the APs up

**Goal:** the other cores execute kernel code and park in `hlt`. Nothing else
changes.

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

## Phase 1 — per-CPU state

**Goal:** each core can hold its own current process, without any core yet
being allowed to schedule.

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

## Phase 2 — the big kernel lock and the multi-core scheduler

**Goal:** all cores run user processes. Correct, with essentially no scaling.

- **A real spinlock** in `include/kernel/spinlock.hpp`: a ticket lock that saves
  and restores `IF`, with `pause` in the spin body. Recursion is not needed if
  the acquisition points are the entry points and only the entry points.
- **Take the BKL on kernel entry, release it on exit.** There are exactly three
  doors, which is why this is tractable: `savanxp_handle_syscall`,
  `savanxp_handle_timer_interrupt`
  ([`timer.cpp:228`](../arch/x86_64/timer.cpp:228)) and
  `dispatch_external_vector`
  ([`cpu_init.cpp:268`](../arch/x86_64/cpu_init.cpp:268)).
- **A shared ready queue under the BKL.** `pick_next_runnable()`
  ([`process.cpp:1142`](../kernel/process.cpp:1142)) must skip processes already
  running on another core, so `Process` grows an owning-CPU field. Without it,
  two cores resume the same `SavedContext` and the process forks in place.
  `has_runnable_non_idle()` ([`process.cpp:1133`](../kernel/process.cpp:1133))
  needs the same treatment, or an idle core spins waking itself for a process
  another core is already running.
- **A reschedule IPI.** `g_resched_pending`
  ([`process.cpp:84`](../kernel/process.cpp:84)) currently hands the CPU to a
  woken process on the syscall return path. When the waker and the target are on
  different cores, that becomes an IPI to the core running something less
  urgent — or, in the first cut, nothing at all: the next tick will pick it up,
  at the cost of latency.
- **Release the BKL around the idle `hlt`**, or the idle core holds the lock and
  the machine stops.

This phase is where the debugging lives. **Estimate: 1-2 weeks.**

## Phase 3 — TLB shootdown

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
  defaulting to 1 so nothing existing changes.
- At least `smoke`, `windowd-smoke` and `sxfs-smoke` get an `-smp 4` run. The
  filesystem and compositor smokes are the ones that actually exercise shared
  state from several processes at once.
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
