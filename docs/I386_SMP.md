# i386 SMP startup and scheduling

Milestone 6.2 adapts the SMP design of SavanXP v0.4.0, commit
`8ece07f4dae90812155a541951cbf8b3701bd92d`. Its first checkpoint implements
AP startup, private TSS/stacks and repeated directed IPIs. The next checkpoint
adds preemptive user scheduling on available APs under the reference global
kernel lock. `--smp N` runs this path for 1–32 CPUs. Physical-machine validation
remains reserved for v0.4.2.

## Discovery and protected-mode entry

The existing ACPI table walker supplies a checksummed MADT through the native
RAM alias. The topology parser validates table and entry bounds, enabled CPU
flags, duplicate IDs and LAPIC address overrides. Disabled online-capable CPUs
are not started: hotplug is separate work. The BSP is dense CPU index zero
even if firmware lists it later. The reference capacity is 32 CPUs; the parser
reports the enabled count separately from the bounded startup list. Native
xAPIC delivery requires IDs below 256 and device addresses below 4 GiB.
Malformed/unsupported topology falls back to the BSP without starting APs.

Limine supplied the reference APs already in long mode on usable stacks.
GRUB2 does not do that. The native adaptation copies a bounded real-mode
trampoline to physical `0x7000`, checks that E820 describes that page as usable
and that no Multiboot information/module overlaps it, then sends INIT followed
by SIPI. The page is already excluded from allocation with the reserved low
kernel range. If unavailable, APs are not started and the BSP keeps booting.

The trampoline installs flat protected-mode segments, enables PSE, loads the
shared kernel CR3 and enables paging/write protection. It clears INIT's CD/NW
bits so AP caches match the BSP's normal memory behavior. The stack table is
indexed by the AP's own initial APIC ID and populated before any launch. This
avoids a mutable per-launch mailbox: a late AP still finds its own stack.
Four contiguous allocator pages per AP provide its 16 KiB supervisor stack.
Stacks remain reserved even after a startup timeout, so a delayed AP cannot
enter on memory reused by a process or another CPU.

Each AP loads the already-built GDT/IDT, its own TSS selector, and x87/SSE
control state. Only then does it enable local interrupts and publish online.
Before scheduling is released, AP code touches only its own slot, registers
and startup probe state. It does not allocate, mount devices or access mutable
subsystems concurrently with boot. Scheduling starts only after userland and
all per-CPU idle processes are initialized, with the kernel lock held.

## Local APIC and interrupts

The local xAPIC registers use an uncached shared supervisor MMIO view. The BSP
owns the single LINT0 ExtINT route for the existing PIC; AP LINT inputs remain
masked. The BSP calibrates a divide-by-16 local timer against the existing
monotonic TSC clock. Each scheduling AP starts a periodic 10 ms timer on vector
65. Vector 66 requests rescheduling; the BSP retains PIT scheduling and uptime. The PIC/PIT driver and its IRQ numbering are unchanged.
External vectors have separate saved-frame stubs and explicit local-APIC EOI;
vector 255 is a dedicated spurious gate and sends no EOI.

The startup selftest sends two fixed-vector IPIs to each AP, waiting on that
AP's acknowledgement counter. It also reads the AP's observed LAPIC ID and
hardware `STR` result. Repeated delivery checks EOI as well as the first ICR
write; checking an individual slot prevents a broadcast from passing as a
directed message. With four CPUs the log reports three distinct selectors
`0x30`, `0x38`, `0x40` and six acknowledged pings.

The reference virtio GPU can now arm MSI-X vector 49 on the BSP. Its existing
handler and deferred completion path remain; the PIC INTx fallback is retained
when a local APIC is unavailable. Device tests require actual IRQ notifications
in addition to the MSI-X boot marker. x2APIC initialization and CPU hotplug
remain outside the supported native path.

The reference I/O APIC driver takes its MADT from the native ACPI table lookup
and maps the controller through an uncached supervisor view. It resolves ISA
overrides for PS/2 and routes the FADT SCI as a level-triggered, active-low GSI.
Each GSI uses one of vectors 50–63 with local APIC EOI after chained handlers.
The BSP installs routes before interrupts are enabled. The PIC entries for
successfully routed devices stay masked. PS/2 and SCI fall back to the PIC if
their route is unavailable. PIT and BIOS-assigned PCI INTx still use ExtINT.

`smoke power-smoke --smp 4` checks route programming, guest reset and shutdown,
and shutdown from the QEMU power button. It also boots with local APIC support
hidden, types a shell command through the PIC keyboard path and repeats the
power-button check. `smoke sxgui-smoke --smp 4` exercises QEMU PS/2 keystrokes,
pointer movement and wheel input against the desktop.

## TSS layout and port permissions

A protected-mode TSS is 104 bytes. Its `esp0` is at offset 4 and the 16-bit I/O
bitmap offset is at byte 102. The old native structure appended extra selector
words and placed that field at byte 108; hardware instead read zero from byte
102 and treated part of the zeroed TSS as an I/O permission bitmap. An observed
control boot with the previous kernel allowed a ring-3 `IN` from port `0x80`.

The corrected layout has compile-time size/offset assertions and puts the
bitmap beyond the descriptor limit, denying user port access. The added
`multitest32 fault-io` case must terminate with #GP while its parent/sibling
survive and pages are reclaimed. The same diagnostic on the earlier kernel
completed the instruction and exited 99; on the corrected kernel it produced
#GP. The test therefore detects the original permission defect independently
of merely compiling a structure with the expected layout.

## Verification

```sh
./build.sh iso
./build.sh smoke smp-boot --smp 4
./build.sh smoke smp-boot --smp 32 --virtio
./build.sh smoke virtio-smoke --smp 4
./build.sh smoke sxgui-smoke --virtio --smp 4
./build.sh smoke power-smoke --virtio --smp 4
./build.sh smoke
./build.sh smoke memory
./build.sh smoke shared-memory
./build.sh test
```

The SMP harness compares software reports with QMP's actual CPU list and
register contexts. Each CPU must have its own 104-byte TSS and expected
selector; APs may now be executing user code, servicing an interrupt or idle. It
requires paging, write protection, PSE and FPU controls, and rejects disabled
caches. Artifacts retain serial logs, `cpus.json` and `registers.txt`.

Four-CPU and 32-CPU bring-up passed, including 62 targeted IPIs at the limit.
The virtio service/capture suite and all six SxGUI glyph/input captures passed
with four CPUs started and GPU MSI-X armed. ACPI reboot, shutdown and the
power-button SCI also passed in that profile. An isolated optimized build
without diagnostic applications passed four-CPU bring-up and desktop startup.
The 20 host tests include malformed MADT entries, duplicate/sparse IDs,
capacity truncation and a BSP listed after the retained CPU bound.

The default full smoke and the unchanged 32 MiB memory/shared-library gates
passed, including the new ring-3 I/O fault. Doom rebuild persistence and POSIX
rebuild/cold-boot persistence passed on isolated images. Extraction after the
normal rebuild matched the built diagnostic/UI binaries and preserved the
installed Doom/Ccleste executables and IWAD byte for byte.

## Scheduling and synchronization

Native per-CPU state holds the current and idle process pointers and local
tick/quantum counters. Idle processes use the normal per-process 16 KiB
kernel stacks, user frames, page directories and x87/SSE state. The process table and round-robin cursor stay
shared under the reference ticket lock. The scheduler excludes processes owned
by another CPU even when their state has changed while a kernel continuation
waits. The outgoing FPU context is saved before publication and the incoming
CR3, FPU state and that CPU's TSS.esp0 are installed before returning to userland.

All syscall, PIC IRQ, ordinary external-vector and exception entries acquire the
lock before calling C++. Assembly remembers whether this entry acquired it,
changes to the returned frame's stack, then releases it. Releasing on the old
stack would let another CPU resume or recycle a process still executing there.
A nested IRQ while the same CPU holds the lock leaves ownership with the outer
entry. Ping and spurious IPIs bypass it; startup waits for their acknowledgement.

A retained kernel wait releases the lock around `sti; hlt; cli` and reacquires
it before continuing. A timer can suspend a continuation in that exact unlocked
wait window; its ordinary runnable state is qualified by `in_syscall`, which
keeps it pinned to its original CPU until the outer syscall finishes. Ordinary
asynchronous waits still retain a request/frame rather than a C++ stack.

Protected mode supplies only EIP/CS/EFLAGS on a same-privilege interrupt. The
ring-0 frame is 64 bytes, versus 72 bytes for a ring-3 frame including ESP/SS.
The scheduler retains the actual pointer and assembly restores it in place;
it does not copy a partial frame as a full user context. The named assembly
wait window (`i386_wait_interrupt_resume`) makes the saved EIP an explicit
eligibility check. Code with the lock held remains non-preemptible.

After resumption, the wait wrapper reacquires the global lock and synchronizes
the kernel TLB before checking the caller, stack marker, wait depth, assigned
CPU and an actual volatile hardware STR read. FPU state is
saved/restored by the same scheduler path used for user processes.

Killing a remote owner or a suspended kernel continuation records a pending
status and requests rescheduling. A continuation remains alive even while
runnable with no CPU owner: its stack, CR3, section views and handles are not
destroyed by the requester. It resumes on its CPU and exits when its syscall
returns. Kill does not cancel the network operation immediately; a retained
operation may still take its original timeout. User-mode timer/IPI exits keep
the existing direct cleanup path.

Shared kernel-view map/unmap/retarget operations advance a generation under
the lock. Each CPU reloads CR3 when acquiring the lock if its generation is
stale. Code outside the lock accesses only permanent kernel mappings, its own
reserved stack/state and APIC registers. A startup probe makes an AP cache a
view, retargets it on the BSP, reads it without synchronization and then after
synchronization. The first/last values must be `0xa1`/`0xb2`; the intermediate
value is reported because retention of a stale TLB entry is not guaranteed.

`system_info.cpu_count` reports enabled firmware CPUs. `cpu_online` now reports
scheduling CPUs, matching v0.4.0 and the number of exported idle processes; the
earlier online-only meaning is superseded. The second boot marker
`smp: scheduling=N` records that same scheduling count. An AP needs startup,
successful targeted IPIs recorded for its own slot and a calibrated local timer. A delayed AP that
missed those checks stays parked even if it later publishes online. It also
needs an allocated idle process. Unsupported topology or failed readiness
keeps the BSP path; idle allocation failure parks the affected AP. CPU accounting
sums per-CPU timer ticks; uptime advances only from BSP PIT ticks.

## Reference idle processes and accounting

The reference nine-byte `MOV EAX,YIELD; INT 80; JMP` loop runs in ring 3 in each
idle's private address space. Native code is placed at `0x08000000`, inside the
i386 user window, rather than the reference long-mode address. A read-only code
page and one writable stack page account for exactly 8 KiB of user memory.
Idle processes have root CWD, no parent, no handles and `SAVANXP_PROC_FLAG_IDLE`.
They occupy the shared 64-slot process table and never migrate or enter the
ordinary runnable search. When no normal process is runnable, the CPU selects
its own idle as the fallback. No separate synthetic ring-0 idle frame remains.

Idle `yield` follows v0.4.0: if no work is ready, it calls the shared interrupt
wait with the kernel lock released. That kernel continuation remains on its
CPU. The BSP tick services deadlines before returning to it, even when the
saved interrupt frame is ring 0; otherwise sleeping processes could stop
waking when every CPU is idle. The first completed yield checks the actual
user frame, CPU owner and TSS.esp0 before logging `idle: ring3 cpu=N pid=P`.

Idles are allocated before init, so init's PID is discovered dynamically and
used for orphan adoption and protection. Killing an idle or init returns
`EACCES`. Process counts include zombies until reaping, matching reference
enumeration. A failed idle allocation releases partially mapped pages; the BSP
idle is required and a failing AP remains parked.

Each timer tick charges exactly the current process and the system total,
including ticks spent halted inside idle's syscall. Task Manager retains its
idle-complement usage model and original layout. Its samples enumerate counters
separately; with 32 CPUs and a 100 Hz native timer, skew could make idle deltas
exceed the sampled total and display 0% despite measured work. The estimator
now keeps at least the observed non-idle work and bounds percentages to 100%.
The raw counters and API layout are unchanged. This is an approximation for
non-atomic samples, not a new atomic snapshot guarantee.

## Syscall-return wakeup handoff

Like v0.4.0's `note_woken`, completing a blocked I/O, object wait or poll marks
`resched_pending` on the CPU performing the wakeup. The current caller finishes
its C++ work and publishes its user frame before the scheduler hands that CPU
to the ordinary runnable search. An already-blocked caller schedules anyway. This is a scheduling
opportunity, not a priority boost or a guarantee that a particular waiter wins
against every other runnable process. Timers and completed scheduling decisions
consume their own wakeup notifications, so unrelated syscalls do not inherit a
stale request. No wake IPI assigns the waiter to an idle CPU.

Pipe reads, writes and final endpoint releases mark pending readiness changes.
At syscall return, the native code drains blocked pipe readers/writers and
rechecks polls until those changes settle. This includes EOF, `EPIPE`, `dup2`
replacement of the last endpoint and partial-write progress. Rechecks happen
after publishing the caller's outer frame: they may complete the caller's own
newly blocked operation. Iteration replaces recursive reader/writer wakeups;
it preserves the reference's return-time behavior without growing a 16 KiB
kernel stack or polling device drivers after every syscall. Device servicing
continues on its existing timer/scheduling paths. Object-wait completion also
rechecks polls, so an event observed with `poll` gets the same handoff as
`wait_one`/`wait_many`.

The `wakeup` scenario runs `wakeup32` on 1–32 CPUs. Fourteen cases repeat 16
times: auto/manual events, wait-all, semaphores, event polling, pipe data/space,
EOF/hangup, broken readers, descriptor replacement and partial writes. A shared
section records whether the waiter executed before the triggering syscall's
caller continued, with no intervening syscall. Only the one-CPU run asserts
that order; SMP runs check delivery and results because other CPUs can race
the observation. An unrelated-call control discards samples containing a PIT
tick, and exact process/free-page counts must recover after all children and
views are released. This checks local wakeup policy without using a latency
threshold as evidence.

## Functional gates and remaining parity

```sh
./build.sh smoke wakeup
./build.sh smoke wakeup --smp 4
./build.sh smoke kernel-preempt
./build.sh smoke kernel-preempt --smp 4
./build.sh smoke idle
./build.sh smoke idle --smp 4
./build.sh smoke smp --smp 4
./build.sh smoke smp-boot --smp 32 --virtio
./build.sh smoke virtio-smoke --smp 4
./build.sh smoke sxgui-smoke --smp 4 --virtio
```

The `kernel-preempt` scenario (1–8 CPUs) runs `kpreempt32`. The existing TCP
fault injector drops every sequence-consuming packet, so CONNECT remains in
the kernel until timeout without contacting an external TCP server. More
waiters than CPUs force actual ring-0 suspension; a non-cooperating sibling
must accrue CPU ticks during short probes while those calls remain pending.
The shared assembly probe verifies GPRs, segments, flags, x87/SSE registers and
control words across the call. Three kills require deferred cleanup, original
waiter resources and exact free-page recovery; the runtime-discovered socket
capacity must return to its baseline after normal exits and every kill; on one CPU all victims must
actually be suspended rather than owned by another CPU. The harness checks
balanced suspend/resume records and CPU/stack validation. The eight-CPU bound
is a diagnostic workload bound, not a system scheduling limit.

The `idle` scenario (1–32 CPUs) derives a console ISO and runs `idletest` and
the reference Task Manager selftest. It checks idle identity/protection, init
protection, zombie counts and exact page reclamation. Sandwiching process
counter reads between global samples gives bounds for tick conservation;
sleep must accrue idle time and a busy process must accrue roughly one CPU's
share. The harness requires a verified ring-3 idle entry on every CPU.

The `smp` scenario includes those checks, then runs the reference `smptest`: another child must be running while the parent enumerates
processes inside a syscall, and a CPUID sample must show the parent or a child on an AP.
Sixteen termination/reap cycles exercise deferred cleanup; the harness must
observe a remote request and actual reschedule-vector delivery. A timer may
safely finish termination before the queued IPI runs. Two thousand pipe round trips
exercise cross-CPU wakeups. The existing `multitest32` then checks integer,
x87/SSE, private memory, faults, slot reuse and page reclamation under SMP.
At 32 CPUs, `smptest` limits spinners to the remaining process slots after
accounting for idles/init/shell/test, preserving the reference capacity of 64. The separate `smp-boot`
scenario boots the normal desktop, including virtio.
Both use QEMU disk snapshots and retain serial/QMP artifacts.

These checkpoints cover idle accounting, timer-preemptible kernel waits,
syscall-return wakeup handoff and I/O APIC routing for PS/2 and SCI. AML-based
PCI INTx, BSP timer parity, ASLR/signals and the performance/visual review
remain open. v0.4.0 deliberately keeps idle
wakeups on timer boundaries instead of adding wake IPIs; that policy alone is
not a parity gap. The historical study's earlier wake-IPI suggestions are
superseded by the pinned-reference review.
The historical startup evidence above describes the earlier parked stage;
[the study](I386_PORT_STUDY.md#17-preempting-retained-kernel-waits-without-migrating-them)
records the transition and subsequent test results.
