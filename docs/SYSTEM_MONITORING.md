# System monitoring

How SavanXP answers "what is this machine doing right now" and "what machine is
this", which are two different questions with two different windows behind them.

The split is deliberate:

- **Task Manager** (`/bin/taskmgr`) shows *state*: what is running, what it
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

Three fields carry the whole scheme (`savanxp/syscall.h`):

| Field | Where | What it is |
| --- | --- | --- |
| `savanxp_process_info.cpu_ticks` | per process | timer ticks that found this process running, since it started |
| `savanxp_system_info.cpu_ticks_total` | system | the timer tick counter |
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

The headless self-test (`build.ps1 taskmgr-smoke`) checks both directions,
because only both together prove the counters land on the right process: after
`sleep_ms`, system usage must be near zero (the idle counter is advancing);
after a busy loop, both system usage and the test's own process must be above
half (the caller's counter is advancing).

### Ticks are guest time, not wall time

`cpu_ticks_total` and `uptime_ms` come from the timer interrupt, so they count
interrupts *delivered*, which under emulation is not the same as time passing.
`build.ps1 clock-smoke` measures both kernel clocks against the one reference
that does not depend on either — the RTC — while spinning and while idle:

| | reference | `uptime_ms` | `monotonic_ns` |
| --- | --- | --- | --- |
| QEMU/TCG, spinning | 12.0 s | 7.9 s (65%) | 11.5 s (95%) |
| QEMU/TCG, idle | 12.0 s | 8.0 s (66%) | 12.1 s (101%) |
| VirtualBox, spinning | 12.0 s | 11.1 s (92%) | 11.2 s (93%) |
| VirtualBox, idle | 12.0 s | 12.0 s (100%) | 11.8 s (98%) |

Two things to take from it. **Under TCG `uptime_ms` runs about a third slow**,
because QEMU cannot hand a slowly-emulated guest a thousand interrupts a
second; the TSC tracks reality. On hardware virtualization both clocks are
right. And in every case **the numbers do not change between the two phases**:
halting costs neither clock any time, which is what the test asserts on. The
absolute skew is reported as a warning rather than a failure, because it
belongs to the emulator and not to the kernel.

The corollary for anything timed against `uptime_ms` — Doom's game clock is the
obvious one — is that under TCG it runs slow against the wall while staying
self-consistent: a frame counter divided by guest time still reads its nominal
rate. That is a property of the harness, not a bug in the program.

`monotonic_ns` has its own failure mode worth knowing: it is calibrated once at
boot against the PIT over a 10 ms window, and a bad calibration poisons it for
the whole session. One VirtualBox boot was measured reporting roughly 6.8x real
time before a later boot came back correct. When a timing number looks absurd,
re-run `clock-smoke` before believing it.

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
  retransmission clock ([`NETWORKING.md`](NETWORKING.md)), has no periodic
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

`arch::x86_64::query_cpu_identity()` reads the vendor string (leaf 0), the
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

## What the Task Manager does not have

Two tabs of the original are missing, and both are missing for the same kind of
reason — the system does not have the thing they would list, not the UI to list
it:

- **Applications.** Would list *windows*, not processes, and the WM does not
  publish its window list to clients: the protocol in
  [`WM_SUBSYSTEM.md`](WM_SUBSYSTEM.md) is a fixed set of descriptors per client,
  with no channel to ask the WM anything. Adding one also costs a descriptor per
  client, and the budget there is already tight.
- **Users.** Would need user accounts, which do not exist. Every process runs
  with the same authority; `savanxp_kill` has no owner check, which is why End
  Process can end anything except the idle process.

Both are absent tabs, not empty ones. Adding either starts with the subsystem,
not with the window.
