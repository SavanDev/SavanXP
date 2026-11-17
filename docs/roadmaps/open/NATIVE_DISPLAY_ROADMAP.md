# Native display drivers

> **Status: planned, not started (2026-11-17).** Every phase here is FB7
> work in [FRAMEBUFFER_ROADMAP.md](FRAMEBUFFER_ROADMAP.md#fb7--decide-on-one-native-display-target)
> and starts under FB7's conditions: FB1 to FB6 code done, a physical run
> that shows the need, and a machine available for repeated tests. The
> research behind it, with the measurements and the register evidence, is
> [NATIVE_GRAPHICS.md](../../hardware/NATIVE_GRAPHICS.md).

Display drivers of our own, one GPU family at a time, that improve on the
firmware framebuffer without Linux's DRM stack. First page flips and a
hardware cursor on the mode the firmware set (**level 1**), then mode
setting where the hardware allows it (**level 2**). Porting a DRM driver
instead is [DRM_PORT_ROADMAP.md](DRM_PORT_ROADMAP.md), and is deferred.

## Decisions

- **No Linux DRM code and no compatibility layer.** Each driver is SavanXP
  code. MIT-licensed sources (Haiku's `intel_extreme` and `radeon_hd`, the
  Intel and AMD files of `i915`, `radeon` and Display Core, `radeon`'s
  `atom.c`) may be selectively ported with their notices; coreboot's
  `libgfxinit` (GPL-2.0) is read, never copied. Every adoption is recorded in
  [THIRD_PARTY_PROVENANCE.md](../../third-party/THIRD_PARTY_PROVENANCE.md).
- **Level 1 before level 2, family by family.** Level 1 writes only the
  scanout address, the cursor, and the registers that report their state. It
  never writes a PLL, link, encoder, clock or power register, so a mistake
  shows the wrong buffer instead of a dark monitor.
- **No new API.** The display contract already has the hooks, and
  `compositord` already uses them with `virtio_gpu`, falling back to a
  software cursor when the plane refuses an image:
  - `set_cursor` and `move_cursor`, advertised with
    `SAVANXP_GPU_INFO_FLAG_CURSOR_PLANE` and
    `SAVANXP_GPU_CONNECTOR_FLAG_CURSOR_PLANE`;
  - `get_present_timeline`, `wait_present` and `create_present_event`, with
    the `ASYNC_PRESENT_EVENTS` flags;
  - `set_mode`, advertised with
    `SAVANXP_GPU_CONNECTOR_FLAG_MUTABLE_MODE_SETTING`, for level 2.

  A driver sets a flag only once the feature behind it works on its named
  machine.
- **Polling first.** Flip completion and vertical blank are read from status
  registers. Interrupts come when a measurement asks for them and the kernel
  has a generic MSI path
  ([DRIVER_FRAMEWORK_ROADMAP.md](DRIVER_FRAMEWORK_ROADMAP.md#interrupts-and-polling)).
- **A family is supported where it was verified.** The sequence of a
  neighbouring generation is not evidence. A driver binds only to the PCI IDs
  it has run on, and refuses the rest with a log line.
- **The hardware is left as found.** Session release, a failed probe and the
  panic path restore the firmware's scanout address and turn the cursor plane
  off.
- **`fb_gpu` stays the fallback** on every machine. A native driver that does
  not bind, or binds and fails, leaves `fb_gpu` in charge of the firmware's
  mode.

## What exists to build on

- The `display::Driver` registry (`include/kernel/display.hpp`):
  `kernel_main` registers `virtio_gpu` and `fb_gpu`, and `bind_best` keeps
  the first, by priority, that claims the hardware.
- `fb_gpu`'s double buffering by panning on Bochs' DISPI adapter
  (`kernel/fb_gpu.cpp`): present into the back buffer, flip, and keep the
  union of the previous damage. Level 1 has the same shape, with a latch at
  vertical blank that DISPI does not promise.
- The keep-or-revert flow of Display Properties and the rollback at each
  layer of `set_mode`
  ([DISPLAY_PROPERTIES.md](../../desktop/DISPLAY_PROPERTIES.md#changing-the-resolution)).
- Host tests that link driver code against a simulated device, as Azalia's
  do ([AZALIA.md](../../kernel/AZALIA.md)).

## Prerequisites shared by every phase

- **FB1**: the validated descriptor, including the framebuffer's physical
  address. Every phase finds the pipe it drives by matching that address.
- **FB2**: memory attributes for the second buffer and the cursor image, and
  an uncached mapping for the register BAR.
- **FB3**: completion semantics. Level 1 is the first backend on metal for
  which "displayed at vertical blank" can be true; FB3 defines how that is
  reported.
- **FB4**: emergency output that selects a visible buffer, which DISPI
  already needs.
- **The GPU behind the framebuffer**: the PCI function one of whose BARs
  contains the framebuffer's physical address. Without it, no phase binds.

## Phase order

Each phase has the two states of
[FRAMEBUFFER_ROADMAP.md](FRAMEBUFFER_ROADMAP.md#phase-order): **code done**
(host tests and QEMU scenarios pass, commit recorded) and **metal verified**
(the physical acceptance run recorded in the machine's document). QEMU
emulates none of these GPUs, so its scenarios only prove the other backends
are untouched. Sizes use the scale of
[REAL_HARDWARE.md](../../hardware/REAL_HARDWARE.md#shared-work-packages).

| Phase | Level | Family | Machine | Size | Prerequisites | Code | Metal |
| --- | --- | --- | --- | --- | --- | --- | --- |
| ND1 | 1 | Intel Haswell, HD Graphics 4600 | [Core i5-4440](../../hardware/CORE_I5_4440.md) | M | FB1–FB4 | open | open |
| ND2 | 1 | AMD DCE 12, Vega 10 | [Vega 56](../../hardware/RYZEN5_1600_VEGA56.md), the development host | M | ND1 | open | open |
| ND3 | 1 | Intel Skylake, HD Graphics 520 | [Compaq 21N2F7AR](../../hardware/COMPAQ_21N2F7AR.md) | S–M | ND1 | open | open |
| ND4 | 1 | AMD AVIVO chipset graphics, RS780 | [FX-4100](../../hardware/AMD_FX_4100.md), Radeon 3000 | S | ND2 | open | open |
| ND5 | 2 | AMD AtomBIOS mode setting, RS780 and RS880 | FX-4100 | L | ND4 | open | open |
| ND6 | 2 | Intel GMA: 945, 965, G4x | A [Core 2](../../hardware/CORE2_DUO.md) unit; none recorded yet | L per generation | ND1; a unit | open | open |
| ND7 | 2 | Intel DDI: Haswell, then Skylake | Core i5-4440, then the Compaq | L–XL each | ND1; ND3 for Skylake | open | open |

**First increment:** ND1. Intel documents the hardware, and the i5-4440 is a
desktop on UEFI already verified at 1920x1080 from GOP. If that machine is
not available for repeated tests when the work starts, ND2 goes first: the
development host always is, at the price of working from Linux's register
headers instead of a manual.

## ND1 — Intel Haswell, level 1

- **Bind** to `8086:0412` only. Map BAR0 (`GTTMMADR`: the registers and the
  GGTT) uncached. Find the pipe and primary plane whose `DSPSURF` holds the
  firmware framebuffer's offset in the graphics aperture (BAR2, `GMADR`).
- **A second buffer** of the same size and pitch: the rest of stolen memory
  if enough is left, or new GGTT entries pointing at RAM. Written through the
  aperture mapped write-combining; the display engine does not snoop the
  CPU's caches. Record every GGTT entry written, and clear them on release.
- **Flip** by writing `DSPSURF`, which the hardware latches at the start of
  vertical blank. Completion is the live surface register showing the new
  address, or the pipe's frame counter moving. Presents copy into the back
  buffer and flip, keeping `fb_gpu`'s damage tracking.
- **Cursor**: a 64x64 ARGB image in GGTT memory, through `CURCNTR`, `CURBASE`
  and `CURPOS`. Read the watermarks the firmware left before enabling it, and
  watch the pipe's FIFO underrun status afterwards.
- **Release, failed probe and panic**: `DSPSURF` back to the firmware's
  address, the cursor off, the GGTT entries cleared.

**Acceptance:** host tests run the bind, flip and release sequences against a
simulated register file, including a probe that fails halfway. On the
machine: twenty boots; a moving-pattern test watched on the monitor (or on
camera) shows no tear line, recorded as an observation, as FB3 requires, not
as a screenshot; the frame counter shows at most one completion per refresh;
`compositord` uses the cursor plane and draws no software cursor; panic text
is visible with either buffer selected; `compositord`'s exit returns the
firmware's buffer; the FIFO underrun status stays clear for a 30-minute
session. The QEMU scenarios for `fb_gpu` and `virtio_gpu` still pass.

## ND2 — AMD DCE 12 (Vega 56), level 1

- **Bind** to `1002:687f` only. Map the register BAR uncached. A register's
  address is the DCE block's base from `vega10_ip_offset.h` plus its offset in
  `dce_12_0_offset.h`.
- **Find the display pipe** (DCP) whose `GRPH_PRIMARY_SURFACE_ADDRESS` (and
  `_HIGH`) holds the GOP framebuffer's GPU address: its offset into the VRAM
  BAR plus the base of VRAM that the memory controller reports.
- **A second buffer and the cursor** in VRAM inside the BAR the CPU sees,
  outside the ranges the video BIOS reserves (atomfirmware's VRAM usage
  table) and away from the firmware framebuffer. If the BAR has no room, the
  driver refuses to bind and says so.
- **Flip**: set `GRPH_UPDATE_LOCK`, write the address, clear the lock, with
  `GRPH_SURFACE_UPDATE_H_RETRACE_EN` cleared so the switch waits for vertical
  blank. Completion is `GRPH_SURFACE_UPDATE_PENDING` clearing. The sequence
  is Display Core's `dce_mi_program_surface_flip_and_addr`.
- **Cursor** through the DCP's `CUR_*` registers, `CUR_SURFACE_ADDRESS` first.
- **Check** whether CPU writes through the BAR need a host data path flush
  before a flip. The firmware framebuffer shows CPU writes without one today.
- **Record** in [RYZEN5_1600_VEGA56.md](../../hardware/RYZEN5_1600_VEGA56.md):
  the BAR size, the reserved ranges and the GOP framebuffer's GPU address.

**Acceptance:** as ND1, with `GRPH_SURFACE_UPDATE_PENDING` as the completion
evidence, plus a boot with too little BAR left over showing the refusal and
`fb_gpu` keeping the display. No SMU, PSP or firmware file is touched.

## ND3 — Intel Skylake, level 1

- The same driver as ND1 with Skylake's universal planes: `PLANE_SURF` and
  `PLANE_CTL` for the primary, its own cursor registers, `8086:1916` only.
- **The cursor costs more than on Haswell.** From Skylake every plane, the
  cursor included, needs its own DDB allocation and watermarks, which the
  firmware most likely left at zero. Flips alone are S; the cursor makes the
  phase M.
- **The panel is left alone**: no panel power, backlight or link register is
  written.

**Acceptance:** as ND1, on the laptop, with the FIFO underrun status clear
while the cursor moves across the whole panel.

## ND4 — AMD AVIVO chipset graphics, level 1

- **Bind** to `1002:9616` (Radeon 3000, RS780C). Registers from AMD's RV630
  and M76 register guides and `radeon`'s `rs600_page_flip`, which the RS690,
  RS780 and RS880 share.
- **Flip** with `D1GRPH_UPDATE_LOCK`, `D1GRPH_PRIMARY_SURFACE_ADDRESS` and
  `D1GRPH_SURFACE_UPDATE_PENDING`; **cursor** with the `D1CUR_*` registers.
- **The second buffer** in the memory the board lends the graphics core (UMA)
  or in SidePort memory. The firmware's share-memory setting decides whether
  there is room.

**Acceptance:** as ND1. The other RS780 and RS880 IDs in
[ATHLON_II.md](../../hardware/ATHLON_II.md#chipset-graphics) are added only
with a run on each.

## ND5 — AtomBIOS mode setting, RS780 and RS880

- **The video BIOS image**: the legacy image on a BIOS boot, the PCI ROM, or
  the ACPI `VFCT` table on UEFI.
- **The interpreter**: a selective port of `radeon`'s `atom.c` (AMD, MIT),
  with its notice, recorded in the provenance registry.
- **The outputs**: connectors and encoders from the BIOS's object tables;
  EDID over DDC, through the chip's I2C engine or the BIOS's I2C table.
- **A mode**: the command tables in the order `atombios_crtc.c` and
  `atombios_encoders.c` call them, on the output the firmware lit, then the
  `D1GRPH_*` size and pitch. `set_mode` with the all-or-nothing rollback of
  DISPI and `virtio_gpu`, real `max_width`, `max_height` and `max_bytes` in
  the connector properties, and `MUTABLE_MODE_SETTING` once it works.
- **Fixtures**: a board's video BIOS is the vendor's binary and stays out of
  the tree, like the Win32 fixtures; host tests use tables written for them,
  and a test against a real image runs only when an environment variable
  points at one.

**Acceptance:** FB7's: the resolution slider in Display Properties works with
the keep-or-revert countdown, without changes to the application; repeated
mode changes; a command table failing halfway restores the previous mode;
panic text is visible after a mode change; level 1's flips still work at
every mode.

## ND6 — Intel GMA, level 2

The package [CORE2_DUO.md](../../hardware/CORE2_DUO.md#chipset-graphics)
describes: the panel's timings from EDID, or from the video BIOS's table on a
laptop's internal panel; the DPLL, the pipe and the plane; the VGA or LVDS
output and the panel fitter. One generation per phase (945, 965, G4x), each
on a unit, after that document's facts have been collected. Its point is
the widescreen panels whose native mode the video BIOS does not list.
SDVO outputs are left out until a board needs them.

**Acceptance:** FB7's, on the unit, at the panel's native mode.

## ND7 — Intel DDI, level 2

- **Haswell:** DDI buffers, the WRPLL, SPLL and LCPLL clocks, transcoders,
  DisplayPort link training over AUX, HDMI; VGA through the PCH.
- **Skylake:** Skylake's DPLLs, CDCLK, power wells, DDB and watermarks for
  every plane, and on the laptop the eDP panel's power sequencing and
  backlight, with the VBT read from the ACPI OpRegion.

Both machines already get their native resolution from the firmware, so what
this phase buys is changing it; it waits until something needs that.

**Acceptance:** FB7's, on each machine, through the output its monitor or
panel uses.

## Not in this roadmap

- Mode setting on Vega and RDNA graphics, discrete or integrated: outside
  `amdgpu` it is not realistic
  ([NATIVE_GRAPHICS.md](../../hardware/NATIVE_GRAPHICS.md#level-2-mode-setting)).
  Their level 1 is possible; a phase is written when a machine with a DCN APU
  is at hand.
- Several outputs at once, hotplug, display power management, and HDMI or
  DisplayPort audio.
- NVIDIA and VIA graphics.
- 3D. SxGL stays a software rasterizer on metal.

## Open questions

- **Where level 1 lives.** It shares `fb_gpu`'s descriptor, copy path and
  damage tracking, which suggests a per-family flip provider inside `fb_gpu`,
  next to the DISPI code; level 2 replaces the mode and probably wants a
  `display::Driver` of its own. To be decided in the first phase that lands.
- **Whether level 1 needs all of FB1 to FB6.** It converts no pixel format
  (FB5), and FB6's measurements are its baseline rather than a prerequisite.
  Relaxing FB7's gate for level 1 is a decision for
  [FRAMEBUFFER_ROADMAP.md](FRAMEBUFFER_ROADMAP.md), not one this roadmap
  takes.
- **Who owns the device.** `fb_gpu` claims no PCI function today. Until the
  exclusive claim of D3 in [DRIVERS_ROADMAP.md](DRIVERS_ROADMAP.md) exists,
  the native driver and `fb_gpu` must not both believe they own the scanout.
