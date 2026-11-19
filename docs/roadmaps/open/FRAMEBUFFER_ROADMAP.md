# Framebuffer reliability and real hardware roadmap

> **Status: planned, 2026-11-16; revised 2026-11-17.** This document records
> the implementation review and the work still required. No phase below is
> complete merely because the current desktop boots or a QEMU scenario passes.

Make the firmware framebuffer a predictable fallback on physical machines:
validated geometry and color, known memory attributes, explicit presentation
semantics, useful diagnostics and a visible emergency console. Optimize it from
measurements, then add native display support for a specific demonstrated need.

This expands [HW-11](../../hardware/REAL_HARDWARE.md#hw-11--video-on-firmware-framebuffers).
Related contracts are [desktop architecture](../../desktop/DESKTOP_ARCHITECTURE.md),
[graphics measurements](../../desktop/GRAPHICS_PERF.md),
[Display Properties](../../desktop/DISPLAY_PROPERTIES.md),
[architecture boundaries](../../kernel/ARCH.md) and
[driver lifecycle](DRIVERS_ROADMAP.md). GPU 3D acceleration belongs to
[VIRGL_ROADMAP.md](VIRGL_ROADMAP.md); it is not required by this work.

## Implementation baseline

The normal path is application surfaces in shared memory, composition by
`compositord`, the `display::Backend` interface and either `virtio_gpu` or
`fb_gpu`. Keep this separation and the existing graphics session ownership.

| Area | Current behavior and review finding |
| --- | --- |
| Boot descriptor | `arch/x86_64/entry.cpp` retains address, dimensions, pitch and bpp from Limine, but not its color masks. `boot::FramebufferInfo` also carries mapped bytes. |
| Validation | Since FB1: `framebuffer::adopt` checks the boot descriptor once in `build_boot_info`, and `available` is the only gate splash, console, `ui` and both display backends read. DISPI modes set at runtime pass the same check. Before FB1, `ready()` and each writer had a partial gate of its own. |
| Copies | `fb_gpu` copies dirty rectangles from imported surfaces; contiguous rows can use one copy. The x86-64 kernel `memcpy` uses `rep movsq` and a byte tail. |
| Bochs path | DISPI supplies mode changes, an aperture size and panning. Double buffering tracks the previous damage as well as current damage. Some older comments describe superseded full-copy or front-buffer-only behavior. |
| Physical GPU fallback | Uses the firmware's fixed mode and visible framebuffer. No generic page flip, vertical blank notification or hardware cursor is exposed. |
| Cache attributes | Aperture remapping prefers a WC PAT entry on the DISPI path. The firmware alias remains and the alias probe accesses both views. Other hardware is logged as `firmware type`, which does not identify its actual memory type. |
| Completion | The framebuffer backend retires presents synchronously; `wait_for_idle` is empty and `flush` checks readiness. There is no monitor refresh completion signal. |
| Emergency output | Since FB4: `panic()` calls `display::emergency_console()` before drawing. `fb_gpu` reads the mode back from the DISPI registers, pans to the console's buffer and hands the console that geometry. Before FB4, a panic with the second buffer shown drew where nobody looked. |

Recorded physical runs include the
[Vega 56](../../hardware/RYZEN5_1600_VEGA56.md) and
[HD Graphics 4600](../../hardware/CORE_I5_4440.md) on UEFI firmware framebuffers.
Those records establish usable output, not a new cache audit, refresh guarantee
or benchmark. This roadmap's baseline is a static review, not another hardware
verification run.

## Scope against HW-11

HW-11 lists five gaps. This roadmap takes four of them: the log line (FB1), the
cache type (FB2), tearing and the missing vertical sync (FB3, FB6) and the
32 bpp gate (FB5). The fifth, **choosing the mode**, stays outside the firmware
fallback milestone on purpose:

- Limine picks the mode once, before the kernel starts, from EDID or the
  forced `1024x768x32` boot entry. The fallback works with whatever mode it
  received; it never sets one.
- FB1 makes the choice observable: the accepted mode, and the fact that it came
  from the bootloader, appear in the boot log.
- Changing the mode after boot already works end to end on a backend that
  can set modes: `GPU_IOC_SET_MODE` reaches `display::Backend::set_mode`,
  `compositord` re-imports its display section and `windowd` lays the session
  out again, with rollback at each layer
  ([Display Properties](../../desktop/DISPLAY_PROPERTIES.md#changing-the-resolution)).
  The backend advertises it with
  `SAVANXP_GPU_CONNECTOR_FLAG_MUTABLE_MODE_SETTING`. `fb_gpu` sets that flag
  only on Bochs' DISPI adapter, and `virtio_gpu` always does, so today only
  QEMU can change mode. On physical hardware the missing piece is a backend
  that implements `set_mode` (FB7), not the API above it. FB1-FB6 cannot close
  this gap.
- Choosing a different firmware mode at boot, such as a boot menu entry per
  resolution or a mode stored on disk, would be bootloader configuration
  work. If that need appears, it gets its own item in
  [REAL_HARDWARE.md](../../hardware/REAL_HARDWARE.md) rather than a phase here.

## Rules and boundaries

- Preserve a usable firmware fallback and serial diagnostics. A failed optional
  optimization must not prevent the desktop from starting.
- Keep composition in ordinary RAM. The final copy owns physical-format
  conversion; applications should not need a different format per GPU.
- Do not infer framebuffer capacity from total VRAM, or an addressable aperture
  from resolution alone. Track the established physical range and mapped span.
- Do not call a CPU copy, a completed device command or a panning write
  "displayed at vblank" unless the backend actually provides that guarantee.
- Never expand WC mappings over device registers. Memory-type changes must
  account for aliases, page size, TLBs, cache maintenance and all active CPUs.
- Add capabilities through versioned or otherwise ABI-compatible contracts.
  Preserve SUN/POSIX feature parity. A new diagnostic program must use SUN;
  a new native API slice ships with its consumer and `nativetest` assertion.
- No blanket GPU driver, multi-monitor, hotplug, suspend/resume or 3D promise is
  part of the firmware fallback milestone. Coordinate future lifecycle work
  with the existing driver roadmaps instead of creating a parallel framework.

## Phase order

Each phase has two states, tracked separately:

- **Code done:** implemented, host tests and QEMU scenarios pass, and the
  commit is recorded here.
- **Metal verified:** the physical part of its acceptance has been run and
  recorded in the matching hardware document.

A phase can be code done while its physical run is pending. Later phases may
build on code-done prerequisites; only the milestone needs every phase metal
verified. Physical tests queue behind available machines, so an increment that
waits for hardware should not hold back the code that follows it.

Sizes use the scale of [REAL_HARDWARE.md](../../hardware/REAL_HARDWARE.md):
S is a few commits, M a short series, L a project of its own.

| Phase | Priority and result | Size | Prerequisites | Code | Metal |
| --- | --- | --- | --- | --- | --- |
| FB1a | High: masks and a checked descriptor, host tests | S | None | done, 2026-11-17 | n/a |
| FB1b | High: one gate for splash/console/backend, honest boot log | S | FB1a | done, 2026-11-17 | open: BIOS and UEFI boot logs |
| FB2 | High: measured, coherent framebuffer memory attributes | M | FB1b | open | open |
| FB3 | High: defined presentation completion and ordering | M | FB1a; coordinate with FB2 | open | open |
| FB4 | High: visible emergency output | S | FB1a; agree emergency contract with FB3 | done, 2026-11-17 | open: a panic on a disposable boot |
| FB5 | Medium: physical pixel format conversion | M | FB1a; FB2/FB3 for physical validation | open | open |
| FB6 | Medium: measured copy and scheduling improvements | L | FB2, FB3, FB5 | open | open |
| FB7 | Conditional: one native display driver | L | FB1-FB6 and a demonstrated hardware need | open | open |

**FB1 code is done.** FB1a: the boot descriptor carries the masks and
`framebuffer::validate` (`kernel/framebuffer_layout.cpp`) checks it, with
`framebuffer-layout-test` on the host. FB1b: `framebuffer::adopt` applies it
before anything draws, every writer reads the one `available` gate, DISPI
modes are checked before `publish_mode`, and the boot log has two
`framebuffer:` lines (plus a warning for BGR). Its metal check is pending: a
BIOS and a UEFI boot log naming the accepted layout.

**FB4 code is done.** See its section for what it covers and what it does
not. **Next:** FB2 and FB3, which need physical runs to close.

FB4 can land before FB2/FB3 finish. RGB/BGR correctness in FB5 precedes other
depths, unless a target's mode survey promotes 24-bit (see FB5). FB7 is a
separately accepted target project; it must not delay closing the firmware
fallback milestone, FB1-FB6.

## FB1 — validate once and describe what boot supplied

FB1 lands in two increments:

- **FB1a** adds the memory model and channel masks to `boot::FramebufferInfo`
  and a pure validation function with its host tests. No caller changes yet.
- **FB1b** routes splash, console and `fb_gpu` through that single result,
  replaces their separate gates, and writes the boot log line. The same check
  covers modes set at runtime: a mode programmed through DISPI `set_mode` is
  validated before `publish_mode` hands its geometry to the console, so the
  boot descriptor is not the only one that is trusted. A 32-bit BGR layout
  boots as it does today, with red and blue swapped, plus a warning in the
  boot log naming the format and FB5; it is not demoted to serial output.

Carry the memory model and channel masks through the boot descriptor. Establish
one checked description before splash, console or the display backend writes
pixels. Validate nonzero dimensions, supported format, row capacity, pitch,
checked size arithmetic, address-range overflow and every ABI narrowing.
Check the mapped span against the visible span without silently making an
inconsistent descriptor valid by increasing its claimed capacity.

Log geometry, pitch, depth, masks, visible bytes, established mapped bytes and
that the mode came from the bootloader.
Record the physical address through a justified translation, not an unverified
subtraction from an arbitrary virtual address. Diagnose rejected descriptors
through serial output without touching their memory. Keep unsupported-format
rejection explicit until FB5 supports that format, with one exception decided
on 2026-11-17: 32-bit BGR (`xbgr8888`) is accepted and reported as such,
because rejecting it would take the desktop away from machines that have one
today, only with swapped colors. FB1b turns that into a log warning; FB5 makes
the colors correct.

**Acceptance:** host cases cover zero dimensions, undersized pitch/span,
overflow, narrowing, padding and invalid masks. Guarded buffers show no writes
on rejection. Existing 32-bit boots keep their output; a rejected descriptor
leaves serial usable. BIOS and UEFI logs identify the actual accepted layout.

## FB2 — audit cache attributes before changing them

Resolve the discrepancy between HW-11's historical WC assumption and the
write-through comment in `kernel/vmm.cpp`. Inspect the current mapping's leaf
bits, including large-page PAT encoding, PAT entry and relevant MTRR rules.
Distinguish a requested PAT type from the effective memory type. Log unknown
or unverified information honestly; `firmware type` is not proof of UC.
Check whether all CPUs that can execute display copies use compatible PAT state.

Keep a valid existing WC mapping. If a change is necessary, define ownership
of the physical range and compatible attributes for every alias, including the
boot/HHDM view. Specify cache maintenance and TLB shootdown before publishing a
replacement mapping. Remove the need for a write/read probe through conflicting
memory types. Do not resize or probe live PCI BARs merely to discover capacity.
Failure must retain a valid conservative mapping and report its limitations.

**Acceptance:** tests exercise 4 KiB and large-page decoding and attribute
selection. Mapping failure preserves console and desktop access. Physical logs
on at least one UEFI and one BIOS machine record the cache audit, with a copy
benchmark before and after any change. SMP execution produces consistent
results. QEMU success alone does not close this phase.

## FB3 — define completion, visibility and ordering

Document which event permits the producer to reuse its source, when CPU stores
have been ordered for device consumption, and whether a backend can report a
scanout/vblank event. Preserve existing caller safety when extending the
timeline; do not silently reinterpret an existing wait as a different event.

Audit full, partial and imported-surface presents, flushes and DISPI flips.
Place the architecture's required ordering operation at the presentation
boundary, before notifying the device or publishing the promised completion.
Verify the requirements of port I/O as well as memory stores; do not add a
fence per pixel or assume an incidental instruction supplies the contract.

Expose only supported capabilities. Firmware output remains a synchronous copy
without vblank; DISPI panning alone is not evidence of refresh synchronization.
Update comments that promise stronger behavior or describe obsolete damage
handling. Preserve the previous-damage union and invalidate it on surface or
mode changes and presentations outside that tracking path.

**Acceptance:** tests cover full/partial transitions, alternating disjoint
damage, surface replacement, session release and sequence/wait behavior. A
backend test verifies operation ordering. QEMU visual checks catch stale pixels
across flips; physical moving-pattern tests document remaining tearing without
mistaking a screenshot or a high submit rate for refresh synchronization.

## FB4 — recover a visible emergency console

Add a minimal backend emergency operation that selects a known visible buffer
or redirects console writes to the active one. Invoke it before panic text is
drawn. It must not allocate, wait for compositor IPC, depend on interrupts or
take locks that the failed execution context may hold. Keep ordinary session
release and mode restoration separate from this bounded emergency path.

Use the validated descriptor and supported physical-format writer. Preserve
serial output if framebuffer recovery is unavailable. Define behavior after a
partial mode change rather than trusting stale console geometry.

**Acceptance:** an isolated panic scenario selects each DISPI buffer in turn,
then verifies visible panic text and serial output. Also cover a single-buffer
fallback and compositor exit/session cleanup. Run the physical emergency test
on a disposable boot session; do not claim physical verification from QEMU.

**As built.** `display::Backend::emergency_console` is optional; `panic()`
calls it through `display::emergency_console()` after stopping the other
cores and before the console is shown. `fb_gpu` implements it with port I/O
and plain stores only:

- It reads the mode the adapter is really scanning out from the DISPI
  registers, not from `g_fb_info`, and runs it through the FB1 check.
- When that mode is usable, it pans to offset 0, where the console writes,
  drops double buffering and hands the console that geometry.
- When the registers describe no usable mode (a panic in the middle of
  `apply_dispi_mode`), it programs the firmware's mode again, single
  buffered. If even that does not read back, `console::detach_framebuffer()`
  leaves serial output alone.

Without DISPI the firmware's scanout never moves, so there is nothing to do.
`virtio_gpu` leaves the operation null and keeps its previous behavior; it is
outside this milestone.

`panictest --display VARIANT` takes the graphics session, shows a magenta
frame and panics. Four scenarios run it, and `tools/panic_display_smoke.py`
checks the screenshot: no magenta left, the console's background and text
visible, and the screen the size of the scanned-out mode.

| Scenario | State at the panic |
| --- | --- |
| `panic-display-front` | double buffered, first buffer shown |
| `panic-display-back` | double buffered, second buffer shown; failed before FB4 |
| `panic-display-single` | a mode too large for two buffers (2560x1638 under QEMU) |
| `panic-display-released` | session released after a flip, as when the desktop exits |

Not covered by a scenario: the reprogramming path after a half-applied
mode, which needs a panic injected inside `apply_dispi_mode`. The physical
run is pending.

## FB5 — convert physical pixel formats at the final copy

Keep the compositor's canonical 32-bit surface format. Add a direct-copy fast
path for an exact match and bounded conversion paths for supported alternate
32-bit channel orders. Share the format description with splash and console,
including panic output, rather than teaching each caller different mask rules.

Packed 24-bit and RGB565 are explicit, separately tested increments. Whether
24-bit is core or optional depends on evidence that has not been collected yet.
Before FB5 starts, record the mode list of the VBE machines whose panels are at
risk: [Pentium 4 Evo N1020v](../../hardware/PENTIUM4_EVO_N1020V.md) (GRUB
`videoinfo`) and [Core 2 Duo](../../hardware/CORE2_DUO.md) (`hwinfo
--framebuffer`, where stolen memory can rule out 32 bpp at large sizes). If
either offers its panel's size, or a usable size, only at 24 bpp, 24-bit
becomes part of the core milestone. Otherwise it stays optional until a target
or reproducible fixture needs it. RGB565 stays optional either way.

Other depths are handled at the final copy. `set_mode` keeps accepting only
32 bpp requests, and `compositord` keeps asking for 32. A backend whose
physical format differs reports its 32-bit logical mode and converts, so
`GPU_IOC_SET_MODE` callers never have to know the scanout's depth.

Respect destination pitch
and pixel size; the source surface pitch remains independent. Unsupported
layouts remain an explained rejection, not a guessed color order. BGR is
the exception already accepted since FB1 (see there); FB5 replaces its
warning with a correct conversion.

**Acceptance:** deterministic color bars and channel ramps compare against
expected bytes for every supported format. Guard bytes catch row overruns;
tests include odd widths, padded pitch, clipped damage and full-frame output.
RGB/BGR correctness is required for the core milestone. Optional depths have
separate status and require physical evidence before being called verified on
hardware. Existing application surface ABIs and screenshots remain compatible.

## FB6 — measure bandwidth, then improve copy and frame cadence

The measurements in [GRAPHICS_PERF.md](../../desktop/GRAPHICS_PERF.md) are from
QEMU under TCG, where the `fb_gpu` copy cost is inflated and unrelated to
physical memory types. There is no physical baseline yet. FB6's first
deliverable is therefore a measurement, not an optimization: the workloads
below run on at least one physical machine with the current code and the
results are recorded. The regression limits are chosen from that baseline.

Extend existing graphics measurements with actual bytes copied, including
previous damage, conversion time, rectangle count, full-copy fallbacks and
latency distribution. Separate composition, copy/submit and actual display
events. Document CPU, backend, mode, cache type and workload with each result.

Compare isolated pointer motion, sustained window dragging, scrolling, sparse
animation, full-screen animation and an idle desktop. As a bandwidth reference,
1920x1080x4x60 is 497,664,000 bytes/s of destination writes alone; 3840x2160 at
the same depth and rate is 1,990,656,000 bytes/s. These are arithmetic budgets,
not measured throughput or a promise that every display runs at 60 Hz.

Measure rectangle merging and a threshold for full copies against the existing
path. Compare copy strategies for the actual memory type and CPU. Preserve the
instruction-set baseline: the kernel uses general registers only, so SIMD needs
an explicit state-management design before it can become a candidate.

Evaluate bounded frame coalescing with a configurable fallback cadence when
there is no refresh signal. Preserve low input latency and source reuse/commit
semantics; avoid busy waiting, extra idle redraws and an unbounded queue. A
timer-based cadence saves work but cannot guarantee elimination of tearing.

**Acceptance:** record before/after CPU time, bandwidth, input-to-present
latency and worst/tail frame times on the same physical workloads. Select
numeric regression limits from the baseline before accepting an optimization.
Idle stays idle; sparse damage stays cheap; full-screen throughput improves
without visual corruption or a latency regression. Publish reproducible results
in `GRAPHICS_PERF.md`, not performance claims derived from QEMU FPS alone.

## FB7 — decide on one native display target

Start only after a physical run demonstrates an unmet need such as native panel
resolution or synchronized presentation, and a machine is available for repeated
tests. Select an exact GPU generation, PCI ID, connector and monitor/panel.
[Intel GMA](../../hardware/CORE2_DUO.md#chipset-graphics) is a documented
candidate, not a commitment to support every GMA or a claim of tested hardware.

The candidates, what each GPU family allows and a plan per family are in
[NATIVE_DISPLAY_ROADMAP.md](NATIVE_DISPLAY_ROADMAP.md), from the research in
[NATIVE_GRAPHICS.md](../../hardware/NATIVE_GRAPHICS.md). It splits the work
into two levels: page flips and a hardware cursor on the firmware's mode
(level 1), possible on Intel and on every AMD generation including the
Vega 56, then mode setting (level 2). The acceptance below is level 2's;
level 1 does not change the mode, and its phases carry their own.

Write the target-specific plan before implementation: resource ownership,
firmware handoff, timings/EDID or panel tables, PLL/pipe/plane programming,
interrupts, vblank, buffer lifetime, cursor plane and failure recovery. Bind
through `display::Driver`; integrate common inventory/lifecycle as those APIs
become available. Mode setting plugs into the existing contract rather than a
new API: implement `set_mode` with the same all-or-nothing rollback as DISPI
and virtio, report real `max_width`/`max_height`/`max_bytes` in the connector
properties, and set `SAVANXP_GPU_CONNECTOR_FLAG_MUTABLE_MODE_SETTING` only
once that works. Roll back partial initialization before falling back, so an
unsuccessful probe does not leave the firmware scanout unusable.

**Acceptance:** the resolution slider in Display Properties works on the
named target, including the keep-or-revert countdown, without changes to the
application. Repeated physical boots and mode changes, failure injection,
session exit and emergency output succeed on the named target. Vblank completion
is verified independently of submit completion; synchronized flips and cursor
behavior have explicit tests. Native mode setting does not imply 3D acceleration.
Broader GPU families and connectors require their own evidence and scope.

## Verification and progress records

Each increment follows the verification table in `AGENTS.md` ("Minimum
verification for any change"). On top of it, framebuffer work exercises both
the `fb_gpu` and `virtio_gpu` backends, runs the applicable cursor and effects
scenarios from `./build.sh smoke --list`, and checks visible changes with
`./tools/shoot.sh`. New integration assertions are registered in
`tools/smoke_catalog.py`.

Record each phase's implementation commit in the Code column and its physical
run in the Metal column of the phase table, with exact commands/results,
hardware, boot mode, firmware, resolution, cache audit and remaining
limitations. Mark unavailable physical tests as pending, and missing optional fixtures as skipped
with their reason. Store physical run summaries in the matching hardware
document and measurements in `GRAPHICS_PERF.md`; link them here when closing a
phase. Screenshots validate pixels, not absence of tearing or correct cache
attributes. Document any optional depth or FB7 deferral explicitly before moving
this roadmap to `done/`.

## Reference material

- [UEFI Graphics Output Protocol](https://uefi.org/specs/UEFI/2.10/12_Protocols_Console_Support.html):
  physical pixel formats, masks and scanline layout.
- [Linux x86 PAT documentation](https://docs.kernel.org/arch/x86/pat.html):
  memory types, aliases and interaction with MTRRs; an engineering reference,
  not an API to transplant into SavanXP.
- [Intel architecture manuals](https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html):
  consult the applicable memory-type, cache/TLB maintenance and ordering rules
  when implementing FB2/FB3, including the target CPU's vendor documentation.
