# System monitoring

How SavanXP answers "what is this machine doing right now" and "what machine is
this", which are two different questions with two different windows behind them.

The split is deliberate:

- **Task Manager** (`/bin/taskmgr.sxe`, a SUN program) shows *state*: what is running, what it
  costs, how full the machine is. Everything it shows changes while you look at
  it.
- **System Properties** (`/bin/aboutapp`) shows *identity*: which system this
  is, where it is installed, what hardware it found. Nothing it shows changes
  while the machine is on.

Having one number in both windows means having two versions of it, so each
number lives in exactly one of them.

## The kernel counts, userland divides

The kernel exports **counters**, never percentages. A percentage is a property
of an interval, and the kernel does not know which interval the caller cares
about.

Three fields carry the whole scheme (`include/abi/savanxp_abi.h`):

| Field | Where | What it is |
| --- | --- | --- |
| `savanxp_process_info.cpu_ticks` | per process | timer ticks that found this process running, since it started |
| `savanxp_system_info.cpu_ticks_total` | system | timer ticks that found a process running, on any core |
| `savanxp_process_info.memory_bytes` | per process | user pages mapped **right now** |

`cpu_ticks` is incremented in `process::handle_timer_tick` for whatever process
was current when the tick arrived. That is the cheapest accounting that can
exist: one increment, on a path that already runs, with no extra bookkeeping to
keep in sync.

It also makes the arithmetic exact. **One tick increments exactly one process**,
so between two samples the per-process increments sum to the global increment.
The usage of a process over an interval is its share of that pie:

```
percent = 100 * (proc.cpu_ticks_now - proc.cpu_ticks_before)
              / (info.cpu_ticks_total_now - info.cpu_ticks_total_before)
```

Both halves must come from the **same pair of samples**. A single reading of
`cpu_ticks` says how much CPU a process has used since it started, which is
almost never the question being asked.

### The idle process is the denominator

The scheduler always has an idle process to fall back to, and it is counted like
any other (`SAVANXP_PROC_FLAG_IDLE` marks it). That is what makes system-wide
usage computable at all: **what the idle process did not take is what everything
else took.**

```
system_percent = 100 * (total_delta - idle_delta) / total_delta
```

The headless self-test (`./build.sh smoke taskmgr-smoke`) checks both directions,
because only both together prove the counters land on the right process: after
`sleep_ms`, system usage must be near zero (the idle counter is advancing);
after a busy loop, both system usage and the test's own process must be above
half (the caller's counter is advancing).

### More than one core

With SMP every core has its own timer and its own idle process, and every tick
charges whatever that core was running. `cpu_ticks_total` counts those ticks
across all cores, so the invariant holds unchanged — **one tick increments
exactly one process** — and so does the arithmetic above. What changes is the
size of the pie: `cores × interval`.

- **System usage** subtracts *every* idle process, not one. `taskmgr` sums the
  deltas of all the processes flagged `SAVANXP_PROC_FLAG_IDLE`.
- **A process's percentage is of all cores together.** A program spinning on
  one core of four reads 25%, as in the classic Task Manager.
- `savanxp_system_info.cpu_online` is the number of cores that schedule, which
  is also the number of idle processes. A core that booted but cannot schedule
  (no LAPIC timer, IPIs not delivered) is not counted.

`cpu_ticks_total` is therefore no longer `timer::ticks()`. That one is still
the BSP's counter alone, because it is a clock: every AP adding to it would make
everything measured in ticks run N times too fast.

`taskmgr-smoke` scales its thresholds by `cpu_online`: it expects one idle
process per core, and a busy loop above `50 / cores` percent.

**One graph per CPU** (the default; View offers one graph for all) needs no
per-core counter. The idle processes are created in core order, so the n-th
idle in the snapshot is core n, and every core's timer runs at the same rate,
so each core's share of an interval is `ticks_delta / cores`. What core n's
idle did not take of that share is core n's usage. The smoke also expects the
busy loop to show on one core at 50% or more. `tools/shoot.sh --smp N` boots
the visual check with more than one core.

### Kernel time

`kernel_ticks` is the part of `cpu_ticks` whose tick found the process in
ring 0: inside a system call, or an interrupt it was charged for. View → Show
Kernel Times draws it in red under the total, in the meter and in the combined
graph, as the original does. The idle processes are left out of it: they halt
in ring 0, and that is not work. Kernel time is per process, not per core, so
the per-CPU graphs show none.

### The wall clock is a free-running counter; ticks only count CPU

Two clocks, two jobs, and mixing them up cost a long hunt:

- Timer ticks count **interrupts delivered**. They are the basis of
  `cpu_ticks` and `cpu_ticks_total`, and they are exactly right for that: one
  tick increments exactly one process.
- `process::now_ms()` — behind `uptime_ms` and every deadline in the kernel —
  comes from a **free-running counter**, because counting interrupts does not
  measure time. Which counter, and why it is not always the TSC, is its own
  story: [`TIME.md`](../kernel/TIME.md).

`./build.sh smoke clock-smoke` is what settled it, by measuring both against the one
reference that depends on neither: the RTC.

The finding that forced the split came from VirtualBox. With the machine
halting — which only started happening once `poll()` blocked — the APIC timer's
delivery turns **bursty**: `timer-stats` measured windows of 2000 ticks arriving
across 13.2 s (≈151 Hz) and other windows of 2000 ticks arriving in 533 ms
(≈4000 Hz), with the timer programmed at 1000 Hz. The handler was cheap
throughout (72-97 µs average), so it was not the kernel holding interrupts off;
the hypervisor was dropping them while the vCPU idled and replaying the backlog
when something woke it.

The total averages out — which is why a 12-second measurement had reported
`uptime_ms` at 92-100% of the RTC and looked fine — but the instantaneous rate
does not. Anything that paces itself on that clock stalls and then
fast-forwards. Doom, whose game clock and FPS counter both read `uptime_ms`,
crawled while its counter still read 36, and sped up whenever the mouse moved,
because any interrupt flushed the backlog.

With `uptime_ms` on the TSC, the same test under TCG went from 29% (spinning)
and 6% (idle) of real time to **94% and 100%**.

`timer-stats:` stays, printed every 2000 ticks, because interrupt delivery is
still worth watching even though timekeeping no longer depends on it:

```
timer-stats: real=2979 ms ticks=2000 ms perdido=979 ms (32%) handler avg=25 us max=1128 us
```

`real` is wall-clock time, `ticks` is what the interrupt counter saw, the difference is
what the hypervisor did not deliver, and `handler` is what this kernel spends
inside the interrupt — the part that is ours to keep small.

One more trap worth remembering: a VirtualBox boot was measured reporting
roughly 6.8x real time, and it was written off as a bad TSC calibration. It was
not — the TSC runs ahead there whenever the guest is busy, which is also why the
`perdido` column above read 52-62% where the truth was 2%. A measurement is only
as good as the clock that took it: when a timing number looks absurd, re-run
`clock-smoke` before believing it, and read [`TIME.md`](../kernel/TIME.md).

### Waiting is not running: what the first measurement found

The first thing the Task Manager reported was `windowd` at 97-99%, in QEMU and
in VirtualBox alike — while the compositor's own instrumentation said its real
work was about 170 µs per frame (`compose_us=54 present_us=113` at 60 fps in
VirtualBox), roughly 1% of a second. The machine was not saturated. What the
number exposed was that `poll_fds()` did not block its caller: it spun in the
caller's context, re-checking descriptors and re-polling the whole device layer
at timer-tick rate, halting in between, without ever leaving `State::running`.
Every tick of the wait therefore landed on whoever called `poll`, and the idle
process — the only thing that makes idleness visible — barely ran.

`poll()` now parks the caller (`WaitReason::poll` over `State::blocked_wait`)
and `wake_poll_waiters()` re-evaluates it from the tick, the same place
`wake_sleepers` and `wake_wait_timeouts` run. The wait granularity did not
change: the old loop did not look more often than that either, because between
checks it was halted. On the same idle desktop the report is now `idle 99%` and
`CPU Usage: 0%`, and average compose time dropped about 4x — the compositor had
been losing the CPU to processes that were only waiting for it.

Two details worth keeping:

- **The request is cached in the kernel.** `Process::poll_entries` holds a copy
  of the descriptor array, so re-evaluating a parked process costs no access to
  its user memory. Reading it there would mean switching `CR3` from the timer
  interrupt handler, a thousand times a second per waiter. User memory is
  touched once, on completion, to hand back the `revents`.
- **`net::poll()` moved with it.** The network pump, and with it the TCP
  retransmission clock ([`NETWORKING.md`](../net/NETWORKING.md)), has no periodic
  heartbeat of its own — the poll spin was what drove it. `wake_poll_waiters()`
  calls it once per tick and only while someone is waiting, which is exactly the
  cadence it had before: with nobody polling, nobody called it either.
  `device::service_background()` and `input::poll()` did not need moving;
  `timer::handle_interrupt()` already runs them every tick.

### An idle machine has to actually halt

Making `poll()` block exposed the other half. The idle process is a `yield` loop
in userland, and `hlt` is privileged, so it cannot halt itself. It used to
barely run — the poll spin held the CPU and halted inside the syscall — so
nothing showed. Once `poll()` parks its callers, idle runs whenever there is
nothing to do, and it was spinning through the syscall at full speed.

The `yield` syscall now halts on the idle process's behalf when no other process
is runnable, with the usual atomic `sti; hlt`. Without it the fix would only
have moved the spin from `windowd` to `idle`: the accounting would be honest and
the machine would still never rest.

## Memory is walked, not counted

`memory_bytes` is computed on demand by `vm::resident_user_bytes()`, which walks
the process's page tables and counts present user pages.

An incremental counter would be cheaper per query and would have to be
maintained in five places — `map_page`, `unmap_page`, the fork that copies page
tables directly, the section views, and `destroy_address_space` — any one of
which could drift. The walk cannot drift, and it only visits entries that are
present, so it costs what the process actually has mapped rather than the size
of the address space. The Task Manager asks once per process per refresh, at
most twice a second.

## Processor identity comes from `CPUID`

`arch::query_cpu_identity()` reads the vendor string (leaf 0), the
feature bits (leaves 1 and `0x80000001`) and the brand string (leaves
`0x80000002..4`), and `snapshot_system_info` exports them as `cpu_vendor`,
`cpu_brand`, `cpu_features` and `cpu_khz`.

Two details worth keeping:

- The brand string's padding is **leading**, not trailing (`"   Intel(R)
  Core(TM)..."`). It has to be assembled from all three leaves and trimmed once,
  at the end; trimming leaf by leaf cuts the name in half.
- `cpu_khz` is the TSC calibration from `timer::tsc_khz()`, which is the same
  calibration behind `monotonic_ns()`. It reports 0 when it never ran, and
  System Properties then omits the speed instead of printing `0.00 GHz`.

The feature bits are what fills the line where Windows XP printed "Physical
Address Extension": `SAVANXP_CPU_FEATURE_LONG_MODE`, `_PAE` and `_NX` are shown
by name, and `_HYPERVISOR` is why the window can say "virtualized".

## A network adapter can exist without a driver

`savanxp_system_info` carries two different network facts, and confusing them
misreports the machine:

- `net_present` — a driver claimed the adapter, so there is a usable `net0`.
- `net_hardware` (plus `net_hardware_vendor`/`net_hardware_device`) — a PCI
  device of class `0x02` is plugged in, whether or not anything drives it.

This is not hypothetical. VirtualBox's default adapter is an Intel PRO/1000
(`8086:100e`), and SavanXP only has drivers for `rtl8139` and `virtio-net`: a
freshly created VM boots with `nic: ningun driver reclamo el hardware` and no
network. Reporting that as "no adapter" sends whoever reads it to check the VM's
configuration, which is fine. Both windows say "adapter present, no driver"
instead, with the PCI id, because the id is the only thing that can be acted on.

The way to have network in VirtualBox today is to set the adapter type to
*Paravirtualized Network (virtio-net)*, which the existing driver claims.
Driving the PRO/1000 needs an `e1000` driver that does not exist yet.

## Ending a process

End Process asks `SXN_SYS_TERMINATE` to end the process and reads the
answer. The kernel refuses two cases in two different ways, and the window
treats them differently:

- **`EACCES`: nobody may.** The idle processes and `init` cannot be ended by
  anyone, elevated or not. The button is disabled on an idle row, and ending
  `init` shows *Unable to Terminate Process — Access is denied.*
- **`EPERM`: not yours.** `SXN_SYS_TERMINATE` follows kill's rule (slice 27
  in [`KERNEL_NATIVE_API.md`](../kernel/KERNEL_NATIVE_API.md)). Since the
  desktop runs as the user, the system's processes (`windowd`, `compositord`,
  the services) are not the user's to end
  ([`USERS_AND_PERMISSIONS.md`](../kernel/USERS_AND_PERMISSIONS.md#what-requires-elevation)).
  The Task Manager then asks the elevation broker (`sxn_elevate`) to run
  `/bin/taskmgr.sxe --end <pid>`, and that elevated half ends it. The window
  itself never gains the rights, which is the same split Add or Remove
  Programs uses for `appwiz --uninstall`. A *No* at the prompt is the user's
  decision and shows nothing.

`elevation-smoke` exercises the second path end to end: the user's kill of a
process the system started is `EPERM`, and the elevated `taskmgr --end` ends
it.

**End Process Tree** ends the process and everything it started, children
before their parent: a parent ended first leaves its children to `init`, and
they drop out of the tree half-way through. The tree is read from a fresh
`SXN_SYS_PROCESS_SNAPSHOT`, not from the list on screen, and the Task Manager
leaves itself out of it. The user's rights go first; if any process in the
tree answered `EPERM`, one elevation (`taskmgr --end-tree <pid>`) walks the
tree again and ends what is left.

## The process list

The columns are Image Name, User Name, PID, CPU, CPU Time, Mem Usage,
Threads and Handles, all from one `SXN_SYS_PROCESS_SNAPSHOT` record. User
Name maps the real uid: 0 is `SYSTEM` and 1000 is `user`, the one account
([`USERS_AND_PERMISSIONS.md`](../kernel/USERS_AND_PERMISSIONS.md#decided));
any other uid shows as its number. CPU Time is `cpu_ticks` at the timer's
rate (`timer_frequency_hz`), as H:MM:SS.

The list starts in the kernel's order, creation order. A click on a header
sorts by that column and a second click reverses it; ties fall back to the
PID so equal rows do not swap places on every refresh. The selection
follows the process, not the row, so a sort or a process ending never moves
End Process onto something else. The header click is the toolkit's
`SXGUI_COLUMN_CLICKABLE` and `SXGUI_ACTION_HEADER`, with
`sxgui_header_column()` saying which column; it does not add a field to
`sxgui_widget`, whose layout `libsxgui` shares with every program.

## What the Task Manager does not have

Two tabs of the original are missing. Neither is blocked by the kernel any
more; each needs a piece of plumbing that does not exist yet.

- **Applications.** It lists *windows*, not processes. The WM already keeps
  that list for the shell: a read-only shared section with each task's title,
  icon and `ACTIVE`/`MINIMIZED`/`NOT_RESPONDING` flags
  (`include/wm/wm_shell_protocol.h`), and the shell's requests already include
  activating a window. What is missing is a way for a client that is not the
  shell to receive that section, and a request to close a window for
  End Task.
- **Users.** There is one account. Who runs each process is the User Name
  column; a Users tab with one account has nothing else to show until there
  is a second one.

Both are absent tabs, not empty ones.
