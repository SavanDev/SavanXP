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

`cpu_ticks_total` and `uptime_ms` come from the timer interrupt, so they measure
the *emulated* machine. Under TCG a minute of wall time is a handful of guest
seconds. Neither number is wrong; they are just not host numbers.

### A process waiting in `poll()` counts as running

The first thing the Task Manager reported was `windowd` at 97-99%, in QEMU and
in VirtualBox alike. That number is correct and the machine is **not** saturated:
what it exposes is that `poll_fds()` in `kernel/process.cpp` does not block its
caller.

Instead of parking the process, the syscall spins in the caller's context —
re-checking the descriptors, calling `device::service_background()`,
`input::poll()` and `net::poll()`, and going back to `hlt` — until a descriptor
is ready or the timeout expires. The process state never leaves `running`, so
every timer tick during the wait lands on the caller, and the idle process,
which is the only thing that makes idleness visible, barely runs.

The compositor's own instrumentation is what settles it. From a VirtualBox
session at 60 fps: `compose_us=54 present_us=113`, so windowd's real work is
about 170 µs per frame, roughly 1% of a second — and `blk_us=455` is time
*waiting* on `compositord`, through a second `poll()` that does not block
either. Under TCG the absolute numbers change and the conclusion does not.

There is a smaller, real cost underneath the accounting one: the loop re-polls
the whole device layer once per timer tick (1000 Hz) rather than once per
timeout, and some of those calls touch I/O ports, which are VM exits.

Both halves have the same fix: make `poll_fds()` block the caller with the
machinery `wait_many` already uses — `State::blocked_wait` plus `wake_tick`,
woken by `wake_wait_timeouts` — instead of spinning. Until that happens, read
any percentage next to a process that polls as "this process is waiting", not
as "this process is busy".

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
