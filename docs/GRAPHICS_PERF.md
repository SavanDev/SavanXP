# Measuring the graphics pipeline

Every performance note about the compositor written before this document ended
with the same sentence: *still to be measured live*. That was not laziness — it
was that `windowd` never read a clock, so the only way to judge a change to the
compose path was to look at the screen and form an opinion.

This document defines what is measured, how to read it, and what it does not
yet answer.

## The pipeline

A pixel drawn by an application reaches the display through four stages:

1. **The client draws** into its shared surface. Applications render directly
   into the section `windowd` handed them, so there is no copy here — but also
   no overlap: `gfx_present` waits for the compositor to consume the previous
   frame *before* the client may touch the buffer again.
2. **`windowd` composes** the visible layers into the display surface. One pass
   per layer, clipped to `damage ∩ bounds − (opaque layers in front)`.
3. **`windowd` presents**, which is a *synchronous RPC to `compositord`* — a
   write plus a blocking read on the reply — and `compositord` in turn issues
   the GPU work.
4. **The backend** puts it on the screen: `virtio_gpu` with
   `TRANSFER_TO_HOST_2D` + `RESOURCE_FLUSH` per dirty rectangle, or `fb_gpu`
   blitting into the VRAM aperture.

## What `windowd-stats` reports

`windowd` emits one line every two seconds *in which at least one frame was
composed*. A still desktop emits nothing rather than filling the log with
zeroes, which is why `window_ms` is reported and not assumed.

```
windowd-stats: window_ms=2000 frames=4631 fps=2315.3 compose_us=74/280 \
               sync_us=38/334 present_us=253/1301 blk_us=292 ipc_us=197 svc_us=1 \
               gpu_us=54 tl_us=1 damage_px=9984 bounds_px=9984 rects=1.0
```

That is one two-second window of the `spin` scenario on KVM with virtio,
synchronous present.

| field | meaning |
|---|---|
| `window_ms` | wall time this line covers. Not a fixed 2000: the report only fires on a frame, so an idle stretch stretches the window. |
| `frames` | frames composed **and** presented in that window. |
| `fps` | `frames / window_ms`, in tenths. Integers truncated to `0` exactly where it mattered — under TCG a frame can take seconds. |
| `compose_us` | average / worst microseconds inside `windowd_draw_desktop`. |
| `sync_us` | average / worst microseconds in the sync step that runs before composing: the `SYNC_PRESENT` round trip and, with asynchronous present, draining the outstanding `PRESENT` reply. It can run on loop iterations that produce no frame; those are charged to the next frame that does. |
| `present_us` | average / worst microseconds inside `present_frame`. With synchronous present this is the whole submit round trip; with asynchronous present it is only the send. |
| `blk_us` | `sync_us + present_us`: the time `windowd` spends blocked on `compositord` per frame. **The figure to compare between synchronous and asynchronous present** -- the asynchronous one moves the wait from present into sync, and only this sum shows whether it also shrank. |
| `ipc_us` | `present_us` minus what `compositord` reports having taken, clamped at zero. With synchronous present: pipe transport plus the cost of getting the other process scheduled. With asynchronous present it drops to zero, because the service no longer happens inside the present. |
| `svc_us` | `compositord` outside its own syscalls. |
| `gpu_us` | the `gpu_present_surface_batch` syscall: kernel plus backend. **This is the driver.** |
| `tl_us` | the `gpu_get_present_timeline` ioctl that precedes it. |
| `damage_px` | pixels actually repainted and presented per frame. |
| `bounds_px` | pixels the bounding box of that damage would have covered — what was presented before damage became an exact region. The gap between the two is what the region buys. |
| `rects` | dirty rectangles per frame, in tenths. Rises as `damage_px` falls: an exact region is smaller but split into more pieces, and each piece costs a command. |

### Why `/dev/serial` and not stderr

`console::write_char` writes to the serial port **and** to the framebuffer text
console. A process printing to stderr while the desktop is up paints text cells
on top of what was composed. `/dev/serial` is write-only and touches nothing but
the port, so measurements do not corrupt the thing being measured.

If `/dev/serial` cannot be opened, the whole instrumentation turns into a no-op.
Measuring must never be the reason the desktop fails to start.

### Cost of measuring

Five `monotonic_ns` syscalls per frame — two around the sync step, one at the
start of compose, one between compose and present, one at the end — plus two
for every loop iteration that syncs without producing a frame. `monotonic_ns`
is TSC-based and calibrated in `uacpi_glue::bringup` before userland exists, so
it is always available; a `0` return is treated as "no clock" and the frame is
discarded rather than counted as noise.

## How to capture it

The instrumentation lives in the interactive session, not in the self-tests, so
`windowd --selftest` does not exercise it. Anything that boots the real desktop
with the serial port redirected to a file will:

```powershell
.\build.ps1 taskbar-smoke                              # default hardware, TCG
.\tools\shoot.ps1 -Scenario taskbar -Virtio -Accel whpx
```

Then read `build/shots/<scenario>/shoot-serial.log`. `shoot.ps1` takes `-Virtio`
(the paravirtualized device set, same as `build.ps1 -Virtio`) and `-Accel`
(`tcg`/`kvm`/`whpx`), which is how the table further down was produced.

With `-Virtio` the pointer is a `virtio-tablet`, which is **absolute**: the trick
of shoving the cursor into a corner to find the origin only works for the
relative PS/2 mouse, so `shoot_session.py` gets `--abs-pointer` and positions the
cursor directly. The scenario's pixel assertions still fail partway through on
the virtio path -- a click does not land on the taskbar button -- which is a
real difference worth chasing but not one that blocks measuring: the stats line
is emitted per frame, so a partial run still yields data.

### Which workload measures what

`shoot.ps1` has three scenarios built for `windowd-stats`. All three drive Gfx
Demo (the Diagnostics group), which moves a 96-pixel box and presents only that
region:

| scenario | what drives Gfx Demo | what its frame rate is actually bounded by |
|---|---|---|
| `bench` | one arrow key per QMP command | the harness: one frame per key |
| `saturate` | sixteen arrow keys per QMP command | still the harness: Gfx Demo folds a burst into one frame, so one frame per QMP round trip |
| `spin` | `S` puts Gfx Demo in auto-motion, and the box moves every frame without input | **the display path**: `gfx_present_region` blocks until the compositor has consumed the previous frame |

The first two were built expecting to saturate the pipeline, and did not.
Measured with synchronous present:

| scenario | configuration | fps | `windowd` busy |
|---|---|---|---|
| `bench` | TCG + base | 18.6 | 3.9% |
| `bench` | KVM + virtio | 41.7 | 2.1% |
| `saturate` | TCG + base | 66.9 | 15.2% |
| `saturate` | KVM + virtio | 68.4 | 6.5% |
| `spin` | TCG + base | 490.8 | 90.4% |
| `spin` | KVM + virtio | 2 331.8 | 84.9% |

*`windowd` busy* is `blk_us + compose_us` over the frame period, `1 / fps`.

Two very different machines landing on the same ~68 fps under `saturate`, with
`windowd` idle for most of every frame, is the tell: that figure is the QMP
command rate, not SavanXP. **Only `spin` measures the display path.** Its
ceiling -- about 2 300 frames per second on KVM with virtio -- is also the
number to keep in mind for interactive use: no input-driven load comes close.

## What the numbers said

Captured under TCG, QEMU standard VGA (the default hardware — `fb_gpu`, not
virtio), while `shoot.ps1` drove the desktop headless.

**A frame with small damage** — moving the pointer:

```
compose_us=198  present_us=205953  ipc_us=205772 svc_us=16 gpu_us=127 tl_us=35
damage_px=672
```

**A frame with large damage** — repainting half the screen:

```
compose_us=7554 present_us=61806  ipc_us=34721 svc_us=32 gpu_us=27006 tl_us=45
damage_px=522609
```

Three things fall out, and they point somewhere very specific:

1. **Compose is not the bottleneck on any path measured.** 198 µs against a
   frame costing 200 ms.
2. **`gpu_us` scales with damage and is cheap**: 127 µs for 672 px, 27 000 µs
   for 522 609 px — roughly 0.05 µs per pixel, consistently. The driver and the
   backend are doing their job.
3. **`ipc_us` does not scale with anything.** It is 205 ms on a frame that
   changed 672 pixels — 1 600× the actual GPU work for that frame. The cost of
   getting a composed frame onto the screen is dominated by the round trip to
   `compositord`, not by any graphics work.

### Why the round trip cost that much

`complete_blocked_wait` set `g_resched_pending`, which makes `handle_syscall`
hand the CPU straight to the woken waiter instead of leaving it for the next
timer tick. `complete_blocked_read` and `complete_blocked_write` did not.

The compositor RPC is a pipe write followed by a blocking pipe read. So when
`compositord` wrote the reply, `windowd` was marked runnable and then waited for
the round-robin scheduler to come back around to it. The preemptive wakeup added
for events had never reached pipes, and the whole display path runs on pipes.

All three completions now request the wakeup, in `kernel/process.cpp`.

### What that bought

Same scenario, three runs before and three after, matched by `damage_px` (which
is deterministic per stage) and reported as medians. **`gpu_us` is the control
variable**: the change touches scheduling and nothing else, so if it moved, the
comparison would be measuring machine load instead of the fix.

| `damage_px` | `ipc_us` before | after | factor | `gpu_us` before | after |
|---|---|---|---|---|---|
| 654 975 | 305 097 | 16 988 | 18× | 22 398 | 22 349 |
| 522 609 | 260 857 | 8 598 | 30× | 24 694 | 26 386 |
| 365 226 | 251 654 | 520 | 484× | 13 191 | 14 657 |
| 265 816 | 299 690 | 457 | 656× | 47 669 | 48 698 |

`gpu_us` does not move anywhere. `ipc_us` collapses.

The pointer-movement stage is the one to look at, because it is the interactive
case and the one where there is almost no graphics work to hide behind:

```
before:  frames=3   fps=1.4   present_us=238464   ipc_us=238196   gpu_us=174
after:   frames=14  fps=5.8   present_us=504      ipc_us=237      gpu_us=238
```

Present drops ~470×, and the same 2.4-second window fits **14 frames instead of
3**. Note that after the change `ipc_us` and `gpu_us` are the same order of
magnitude — which is what a healthy split looks like.

The thrashing this risked — every pipe completion yielding the CPU, two
processes bouncing the quantum between them — did not appear: the full harness
battery passes, including `net-smoke`, `kbd-smoke` and `ffmpeg-smoke`, which are
the throughput-heavy pipe users. Frame rate went up, not down.

### Across backends and accelerators

The split above is from the default hardware (`fb_gpu` over standard VGA) under
TCG. Running the same scenario on other configurations separates what belongs to
the emulator from what belongs to the display backend. All figures are
microseconds per frame, medians, on the stage that repaints ~522 000 pixels:

| configuration | compose | ipc | gpu | present | gpu share |
|---|---|---|---|---|---|
| TCG + base (Windows) | 6 126 | 8 598 | 26 386 | 35 027 | 75% |
| TCG + virtio (Windows) | 5 297 | 10 420 | 409 | 11 030 | 3.7% |
| WHPX + virtio (Windows) | 1 326 | 6 490 | 151 | 6 647 | 2.3% |
| KVM + base (WSL2) | 1 520 | 1 845 | 763 | 2 727 | 28% |
| KVM + virtio (WSL2) | 1 360 | 2 518 | 85 | 2 599 | 3.3% |

Read one axis at a time, and only within the same host:

- **The display backend.** KVM + base against KVM + virtio, same host and the
  same QEMU binary, changes nothing but the device: `gpu_us` goes 763 -> 85,
  **9x cheaper**, while `compose_us` barely moves. Under TCG the same swap is
  64x (26 386 -> 409), inflated because TCG makes `fb_gpu`'s CPU blit into the
  VRAM aperture far more expensive than it is on real hardware. Either way the
  direction is the same: `virtio-gpu` only queues a descriptor and lets the host
  do the copy.
- **The accelerator.** TCG + virtio against WHPX + virtio changes nothing but
  the accelerator: `compose_us` drops 4x, which is guest CPU work no longer
  being interpreted.

Do not read across hosts. WHPX and KVM are different hypervisors on different
machines, so their `ipc_us` figures say nothing about each other.

**The conclusion that matters.** On the best configuration measured -- KVM with
paravirtualized devices -- the GPU work is **3.3% of a frame**: 85 microseconds
out of 2 599. `ipc_us` is the other 97%. Every remaining idea aimed at the
graphics path (blob resources, double-buffered client planes, VirtualBox
HGSMI) is competing for that 3%.

### Reproducing the table

Arch packages QEMU's virtio display devices as separate modules, so a stock
`qemu-system-x86` has no `virtio-vga`. They link against QEMU internals and must
match the installed binary exactly -- pulling a single module against a stale
package database gets a version that segfaults QEMU. Install them in the same
transaction as the upgrade, so the versions match by construction:

```bash
pacman -Syu --needed qemu-hw-display-virtio-vga qemu-hw-display-virtio-gpu-pci
```

### Caveats

- **TCG inflates everything, unevenly.** Across runs of the same scenario
  `present_us` moved between ~27 000 and ~266 000 while `compose_us` stayed
  flat, because the harness also drives QMP and takes screenshots. The ratios
  are the robust part; the absolute numbers are not.
- **Run counts are small** -- one to three per cell. Under hardware
  acceleration the spread is narrow enough that the ordering is not in doubt,
  but treat single-digit differences as noise.
- **Boot stages are excluded.** The first two report lines of every run cover
  the session still launching its processes, where `ipc_us` is dominated by
  contention and reaches hundreds of milliseconds. They are not steady state
  and none of the figures above include them.

## Asynchronous present

> **Status: measured and reverted -- it is not in the tree.** What follows
> documents the design as it was implemented and measured, so it can be
> re-applied when SMP phase 2 gives `compositord` a core of its own. The
> verdict is at the end.

`windowd` sends `PRESENT` and blocks on the reply before returning to its
loop. The asynchronous version sent it, went back to servicing input and
clients, and only collected the reply when something needed it.

### The rules that keep it correct

- **At most one reply in flight.** Sending a `PRESENT` first collects the
  previous one. A reply is ~150 bytes against an 8 KiB pipe, so with one
  outstanding `compositord` can never block writing it -- the same class of
  hang the WM already hit once with input backpressure.
- **Collect before any other RPC.** Replies come back in order on one pipe
  and `compositor_rpc` checks serial and type, so an uncollected `PRESENT`
  reply would be read as the answer to the next request and drop the
  connection.
- **Collect before touching the display surface again.** The reply carries
  the present sequence the sync step waits on. `sync_pending_present` used to
  return early when no sequence was pending; with the reply uncollected the
  sequence is still unknown, and that early return would let `windowd`
  compose over a surface `compositord` had not presented yet. The
  implementation checked `windowd_compositor_present_outstanding()` instead:
  true for an uncollected reply as well as for an unretired sequence.
- **A dead `compositord` forgets its reply.** `reap_daemon` clears the
  outstanding flag: the reply is never coming.

Two behaviours changed. A failed present surfaced one frame later, when its
reply was collected, instead of on the call that sent it. And a hardware
cursor move -- `MOVE_CURSOR`, one RPC per pointer event on virtio -- collected
any outstanding present first, so it absorbed that wait instead of the frame.

### Why it cannot help on one core

SavanXP schedules on a single core: the application processors boot and park
([SMP_ROADMAP.md](SMP_ROADMAP.md), phase 0). On one core, making an RPC
asynchronous does not let `compositord`'s work overlap anyone else's -- it
changes *when* that work runs, not *whether* it is paid. Two details make it
concrete:

- The preemptive wakeup hands the CPU to `compositord` as soon as `windowd`'s
  `write` completes its blocked read, so even an asynchronous send does not
  return until `compositord` has served the request. Under TCG `present_us`
  stays around 1.1 ms with asynchronous present, send-only or not.
- The present already ran *after* the client was released:
  `signal_composed_batches` comes before `present_frame`, so waiting on the
  reply was already overlapped with the client drawing its next frame.

On `spin`, the one scenario bounded by the display path:

| configuration | fps sync -> async | `blk_us` sync -> async | `windowd` busy | `gpu_us` (control) |
|---|---|---|---|---|
| TCG + base | 490.8 -> 484.6 | 1 466 -> 1 467 | 90% -> 90% | 503 -> 510 |
| KVM + virtio | 2 331.8 -> 2 313.0 | 295 -> 223 | 85% -> 65% | 54 -> 53 |

On KVM `windowd` is blocked 72 µs less per frame and its busy share drops by
twenty points, but the freed time does not become frames: on one core it is
spent running `compositord` somewhere else in the same loop. That makes
asynchronous present the natural consumer of SMP phase 2 -- *all cores run
user processes* -- where `compositord` on another core turns the reordering
into real overlap.

### Verdict

One cell looked like a win and was not. `bench` under TCG went from 18.6 to
35.0 fps across the original captures, but those were taken hours apart and
the synchronous one never recorded its input rate. Re-run back to back, same
harness, same host:

| `bench`, TCG + base | fps | input keys/s | keys per frame | `blk_us` |
|---|---|---|---|---|
| synchronous | 34.5 | 198 | 5.7 | 1 242 |
| asynchronous | 35.4 | 199 | 5.6 | 1 191 |

3% on a workload bounded by its input, with identical input: noise. The 18.6
was the host being slower when that capture ran -- the same synchronous
binary does 34.5 in the re-run. Two rules came out of it: an A/B is only
valid back to back, and a workload bounded by its input has to record that
input.

Across every scenario and both accelerators, asynchronous present bought no
frames. It was correct -- `windowd-smoke`, including the test that kills
`compositord` with a present in flight, passed with it -- but it moved a
failure one frame later and made hardware-cursor moves absorb the wait, in
exchange for nothing measurable. So it was reverted, and this section is
where it waits for phase 2.
