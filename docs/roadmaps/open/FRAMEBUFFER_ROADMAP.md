# Framebuffer reliability and real hardware roadmap

> **Status: planned, 2026-11-16.** This document records the implementation
> review and the work still required. No phase below is complete merely because
> the current desktop boots or a QEMU scenario passes.

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
| Validation | `fb_gpu::initialize` narrows geometry and buffer size to 32 bits. `ready()` checks address, 32 bpp and nonzero size, rather than validating the complete descriptor. Console and splash have separate gates. |
| Copies | `fb_gpu` copies dirty rectangles from imported surfaces; contiguous rows can use one copy. The x86-64 kernel `memcpy` uses `rep movsq` and a byte tail. |
| Bochs path | DISPI supplies mode changes, an aperture size and panning. Double buffering tracks the previous damage as well as current damage. Some older comments describe superseded full-copy or front-buffer-only behavior. |
| Physical GPU fallback | Uses the firmware's fixed mode and visible framebuffer. No generic page flip, vertical blank notification or hardware cursor is exposed. |
| Cache attributes | Aperture remapping prefers a WC PAT entry on the DISPI path. The firmware alias remains and the alias probe accesses both views. Other hardware is logged as `tipo del firmware`, which does not identify its actual memory type. |
| Completion | The framebuffer backend retires presents synchronously; `wait_for_idle` is empty and `flush` checks readiness. There is no monitor refresh completion signal. |
| Emergency output | Panic enables the console at the framebuffer base. Session release resets DISPI panning, but panic does not take that normal release path. Visibility with the second buffer selected needs a regression test. |

Recorded physical runs include the
[Vega 56](../../hardware/RYZEN5_1600_VEGA56.md) and
[HD Graphics 4600](../../hardware/CORE_I5_4440.md) on UEFI firmware framebuffers.
Those records establish usable output, not a new cache audit, refresh guarantee
or benchmark. This roadmap's baseline is a static review, not another hardware
verification run.

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

All phases are **open**. The last column is a dependency, not a completion claim.

| Phase | Priority and result | Prerequisites |
| --- | --- | --- |
| FB1 | High: validated boot descriptor and usable diagnostics | None |
| FB2 | High: measured, coherent framebuffer memory attributes | FB1 |
| FB3 | High: defined presentation completion and ordering | FB1; coordinate with FB2 |
| FB4 | High, small: visible emergency output | FB1; agree emergency contract with FB3 |
| FB5 | Medium: physical pixel format conversion | FB1; FB2/FB3 for physical validation |
| FB6 | Medium: measured copy and scheduling improvements | FB2, FB3, FB5 |
| FB7 | Conditional: one native display driver | FB1-FB6 and a demonstrated hardware need |

FB4 can land before FB2/FB3 finish. RGB/BGR correctness in FB5 precedes optional
24-bit or RGB565 coverage. FB7 is a separately accepted target project; it must
not delay closing the firmware fallback milestone, FB1-FB6.

## FB1 — validate once and describe what boot supplied

Carry the memory model and channel masks through the boot descriptor. Establish
one checked description before splash, console or the display backend writes
pixels. Validate nonzero dimensions, supported format, row capacity, pitch,
checked size arithmetic, address-range overflow and every ABI narrowing.
Check the mapped span against the visible span without silently making an
inconsistent descriptor valid by increasing its claimed capacity.

Log geometry, pitch, depth, masks, visible bytes and established mapped bytes.
Record the physical address through a justified translation, not an unverified
subtraction from an arbitrary virtual address. Diagnose rejected descriptors
through serial output without touching their memory. Keep unsupported-format
rejection explicit until FB5 supports that format.

**Acceptance:** host cases cover zero dimensions, undersized pitch/span,
overflow, narrowing, padding and invalid masks. Guarded buffers show no writes
on rejection. Existing 32-bit boots keep their output; a rejected descriptor
leaves serial usable. BIOS and UEFI logs identify the actual accepted layout.

## FB2 — audit cache attributes before changing them

Resolve the discrepancy between HW-11's historical WC assumption and the
write-through comment in `kernel/vmm.cpp`. Inspect the current mapping's leaf
bits, including large-page PAT encoding, PAT entry and relevant MTRR rules.
Distinguish a requested PAT type from the effective memory type. Log unknown
or unverified information honestly; `tipo del firmware` is not proof of UC.
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

## FB5 — convert physical pixel formats at the final copy

Keep the compositor's canonical 32-bit surface format. Add a direct-copy fast
path for an exact match and bounded conversion paths for supported alternate
32-bit channel orders. Share the format description with splash and console,
including panic output, rather than teaching each caller different mask rules.

Add packed 24-bit and RGB565 only as explicit, separately tested increments
when a target or reproducible fixture needs them. Respect destination pitch
and pixel size; the source surface pitch remains independent. Unsupported
layouts remain an explained rejection, not a guessed color order.

**Acceptance:** deterministic color bars and channel ramps compare against
expected bytes for every supported format. Guard bytes catch row overruns;
tests include odd widths, padded pitch, clipped damage and full-frame output.
RGB/BGR correctness is required for the core milestone. Optional depths have
separate status and require physical evidence before being called verified on
hardware. Existing application surface ABIs and screenshots remain compatible.

## FB6 — measure bandwidth, then improve copy and frame cadence

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

Write the target-specific plan before implementation: resource ownership,
firmware handoff, timings/EDID or panel tables, PLL/pipe/plane programming,
interrupts, vblank, buffer lifetime, cursor plane and failure recovery. Bind
through `display::Driver`; integrate common inventory/lifecycle as those APIs
become available. Roll back partial initialization before falling back, so an
unsuccessful probe does not leave the firmware scanout unusable.

**Acceptance:** repeated physical boots and mode changes, failure injection,
session exit and emergency output succeed on the named target. Vblank completion
is verified independently of submit completion; synchronized flips and cursor
behavior have explicit tests. Native mode setting does not imply 3D acceleration.
Broader GPU families and connectors require their own evidence and scope.

## Verification and progress records

For each implementation increment, run `./build.sh build`, inspect its successful
output, then run `./build.sh test`. Follow the additional gates in `AGENTS.md`:
kernel changes require `smoke`; native API changes require `native-test`;
compositor changes require `windowd-smoke` and `taskbar-smoke`; drawing changes
require `sxgui-smoke`. Run applicable cursor/effects scenarios from
`./build.sh smoke --list` and exercise both framebuffer and virtio backends.
Register new integration assertions in `tools/smoke_catalog.py` when needed.

Inspect visible changes with `./tools/shoot.sh`, using a short output directory.
Keep tests and screenshots outside tracked documentation. Changes touching the
persistent image/build/SDK also require `./tools/verify_doom_persistence.sh`.
Never use a passing scenario against a previous binary as evidence for a failed
build, or weaken an assertion to close a phase.

Record each phase's implementation commit, exact commands/results, hardware,
boot mode, firmware, resolution, cache audit and remaining limitations. Mark
unavailable physical tests as pending, and missing optional fixtures as skipped
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
