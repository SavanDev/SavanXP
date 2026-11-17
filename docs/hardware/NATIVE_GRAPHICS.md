# Native graphics: what each path would cost

Research of 2026-11-17 into replacing the firmware framebuffer with native
graphics drivers on the [real-hardware targets](REAL_HARDWARE.md): porting a
BSD's DRM driver (`amdgpu`, `i915`, `radeon`), or writing drivers of our own,
and how far each path gets on each GPU family. Nothing here is implemented.
The work that follows from it is planned in two roadmaps:
[NATIVE_DISPLAY_ROADMAP.md](../roadmaps/open/NATIVE_DISPLAY_ROADMAP.md), the
path recommended, and [DRM_PORT_ROADMAP.md](../roadmaps/open/DRM_PORT_ROADMAP.md),
the path deferred. Both sit under phase FB7 of
[FRAMEBUFFER_ROADMAP.md](../roadmaps/open/FRAMEBUFFER_ROADMAP.md).

## The short version

- **None of the BSDs has a driver of its own for these GPUs.** OpenBSD,
  FreeBSD and NetBSD carry Linux's DRM drivers nearly unchanged, on a layer
  that emulates the Linux kernel's internal interfaces. Porting "the BSD driver" means porting
  Linux's, and writing that layer for SavanXP first.
- **The layer is the expensive part, and it is the same for every vendor.**
  It needs kernel facilities SavanXP does not have yet: worker contexts that
  may sleep, workqueues, sleeping locks, driver interrupts, a DMA API and
  firmware loading.
- **A driver of our own can reach two levels without any of that.**
  - *Level 1:* keep the mode the firmware set and drive only the scanout
    address and the cursor. That gives tear-free page flips, a vertical blank
    to wait for and a hardware cursor. It is possible on Intel and on every
    AMD generation, **the Vega 56 included**.
  - *Level 2:* set modes. Plausible on Intel and on the AtomBIOS Radeons; not
    realistic on Vega or RDNA graphics, discrete or integrated.
- **No path gives 3D on metal by itself.** That takes a kernel driver with
  command submission (the DRM port) plus a userland driver from Mesa.

## What the BSDs carry

| System | Where | What it is |
| --- | --- | --- |
| OpenBSD | `sys/dev/pci/drm`, in the base tree | Linux's DRM core, TTM, the scheduler, `amdgpu`, `radeon` and `i915` with OpenBSD's patches, on 280 headers under `include/linux/` and `drm_linux.c`. It follows Linux's stable series: 6.6.12 on 2024-01-16, 6.12.12 on 2025-02-07, 6.18.16 on 2026-03-09 |
| FreeBSD | LinuxKPI (`sys/compat/linuxkpi`) in base; the DRM drivers in the `drm-kmod` port, outside base | The same Linux code |
| NetBSD | `sys/external/bsd/drm2` | An older import. Its `amdgpu` module clears `DRIVER_ATOMIC`, and a 2023 report has its framebuffer never attaching |

OpenBSD's is the one to start from if the port is ever done: it lives in
the base tree, builds with the kernel, its compatibility code is ISC
(`drm_linux.c`, Jonathan Gray and Mark Kettenis), the AMD and Intel files
carry MIT-style notices, and the repository already pins OpenBSD code
(`vendor/openbsd/azalia/`). Its layer does not compile here as it is: the
headers map Linux's interfaces onto OpenBSD's own (uvm, `bus_dma`, taskq,
rwlock, `timeout(9)`), and each mapping would be rewritten onto SavanXP's.

## How big it is

Measured on `openbsd/src` at `09db4a01d52c` (2026-11-17), tree `2bab8720`
of `sys/dev/pci/drm`, from the file sizes the GitHub tree API reports.
Lines are estimated at 35 bytes each: SavanXP's own `kernel/` and
`include/` (`.cpp` and `.hpp`) are 2.07 MB in 55,523 lines, 37 bytes a line.

| Part | Size | About |
| --- | --- | --- |
| `amd/display` (Display Core) | 18.5 MB | 530k lines |
| `amd/amdgpu` | 11.7 MB | 335k lines |
| `amd/pm` (powerplay, swsmu) | 6.2 MB | 175k lines |
| `amd/include` other than `asic_reg` | 5.5 MB | headers |
| `amd/include/asic_reg` | **482 MB** | generated register headers |
| `amd/amdkfd` (compute) | 1.5 MB | 43k lines |
| `i915` | 11.8 MB | 335k lines |
| `radeon` | 6.8 MB | 195k lines |
| DRM core, TTM, scheduler, `include/`, the compatibility code | 5.5 MB | 155k lines |
| **SavanXP's kernel, for scale** | **2.07 MB** | **55.5k lines, counted** |

The AMD code alone is about 1.2 million lines, twenty times this kernel.
Cut down to what a Vega 10 build compiles, it is still large: the
`amd/amdgpu` C files Vega 10 uses (gfx v9, sdma v4, gmc v9, psp v3.1,
nbio v6.1, df v1.7, uvd v7, vce v4, soc15 and the shared `amdgpu_*` core)
are 3.6 MB, Display Core without its DCN, DML and DMUB code 6.0 MB, and
powerplay for Vega 10 with its common code 1.1 MB: about 300k lines, plus
32 MB of Vega 10 register headers and the 155k lines of DRM core underneath.
`i915` covers Gen2 to Gen12 in its 335k lines.

## What a DRM port needs from the kernel

| Linux interface the drivers use | SavanXP today | Where it would come from |
| --- | --- | --- |
| Kernel threads, workqueues, delayed work (`drm_sched`, hotplug, fence timeouts) | None. uACPI's deferred work runs synchronously (`kernel/uacpi_glue.cpp`) | SDF's deferred work is non-blocking only ([DRIVER_FRAMEWORK_ROADMAP.md](../roadmaps/open/DRIVER_FRAMEWORK_ROADMAP.md#time-deferred-work-and-memory)) |
| Sleeping locks and waits: `mutex`, `ww_mutex` (`dma_resv`), `rwsem`, `wait_event` with timeouts, completions, RCU | Kernel code runs under one global lock and cannot sleep on its system-call stack | [SMP_ROADMAP.md](../roadmaps/open/SMP_ROADMAP.md) phase 5; SDF makes blocking a declared capability the kernel host lacks |
| MSI and MSI-X with per-device handlers | Drivers poll (NVMe, Azalia); MSI-X exists only inside `kernel/virtio_pci.cpp` | SDF's interrupts ([DRIVER_FRAMEWORK_ROADMAP.md](../roadmaps/open/DRIVER_FRAMEWORK_ROADMAP.md#interrupts-and-polling)) |
| Coherent DMA, scatter-gather lists, page pools | The physical page allocator (`include/kernel/physical_memory.hpp`) | D4 in [DRIVERS_ROADMAP.md](../roadmaps/open/DRIVERS_ROADMAP.md), SDF's DMA |
| `ioremap_wc` with a known PAT state | Not audited | FB2 |
| `request_firmware` | None | F4: `/system/firmware/` |
| `kernel_fpu_begin`/`end`: Display Core's DML for DCN computes in floating point | The kernel uses general registers only | Needs a state-management design (FB6 notes the same for SIMD) |
| Mapping buffer objects into processes, filled on page faults (GEM, TTM) | Sections and views ([SECTIONS.md](../kernel/SECTIONS.md)); no driver supplies pages on a fault | Open |
| ACPI methods (Intel OpRegion, AMD ATIF/ATCS, VFCT) | uACPI | Available |
| I2C for DDC, backlight, PCI BAR resizing | None: there is no I2C layer | — |

Three facts make it harder than the NVMe and Azalia ports, which were
selective, polled and small:

- **SDF's model rules it out of the kernel for now.** A DRM driver is the
  "driver written around sleeping" SDF describes: it cannot be built as a TKD
  until the kernel has worker contexts that may sleep, and display is not
  planned as an IUD because it is on the boot path
  ([DRIVER_FRAMEWORK_ROADMAP.md](../roadmaps/open/DRIVER_FRAMEWORK_ROADMAP.md#class-interfaces)).
- **QEMU emulates neither Intel nor AMD GPUs.** No smoke can cover it. Every
  run is on metal, or under VFIO passthrough, which needs a second GPU in a
  Linux host; the development host has only the Vega.
- **A hang is a black screen.** Once a driver takes the display, the firmware
  framebuffer is gone. The development host has no confirmed serial header,
  and its Ethernet (HW-1) has not run on metal, so there is nowhere else to
  read the log.

## The levels a native driver can reach

### Level 1: flips and a cursor on the firmware's mode

Display engines latch the scanout address at vertical blank. A driver writes
the address of the next buffer, the hardware switches to it at the start of
the next blank, and a status bit says when it has. Nothing else changes:
no PLL, link, encoder, clock or power register is written, so a mistake
shows the wrong buffer, and writing one address back restores the
firmware's picture. The registers exist on every family the targets carry:

| Family | Scanout address | Done when | Cursor |
| --- | --- | --- | --- |
| Intel before Gen4 / Gen4 to Gen8 / Gen9 on | `DSPADDR` / `DSPSURF` / `PLANE_SURF` | The pipe's frame counter, or the live surface register, moves | `CURBASE`, `CURPOS` |
| AMD AVIVO display: R500 and R600 families, RS690, RS780, RS880 | `D1GRPH_PRIMARY_SURFACE_ADDRESS` under `D1GRPH_UPDATE_LOCK` | `D1GRPH_SURFACE_UPDATE_PENDING` clears | `D1CUR_*` |
| AMD DCE through DCE 12 (Vega 10) | `GRPH_PRIMARY_SURFACE_ADDRESS` under `GRPH_UPDATE_LOCK` | `GRPH_SURFACE_UPDATE_PENDING` clears | `CUR_SURFACE_ADDRESS` |
| AMD DCN (Raven and later APUs) | `DCSURF_PRIMARY_SURFACE_ADDRESS` in the HUBP | The HUBP's flip-pending status clears | HUBP cursor |

Where that was checked: `radeon`'s `rs600_page_flip` and
`rs600_page_flip_pending`, which `rs690_asic` and `rs780_asic` use; Display
Core's `dce_mi_program_surface_flip_and_addr` and `dce_mi_is_flip_pending` in
`dce_mem_input.c`, which DCE 12 reaches through `dce120_mem_input_construct`,
with the registers at `mmDCP0_GRPH_PRIMARY_SURFACE_ADDRESS` (`0x055e`),
`mmDCP0_GRPH_UPDATE` (`0x056a`), `mmDCP0_GRPH_FLIP_CONTROL` (`0x056b`) and
`mmDCP0_CUR_SURFACE_ADDRESS` (`0x05aa`) of `dce_12_0_offset.h`; and
`hubp1_is_flip_pending` in `dcn10_hubp.c`. The Intel registers are the ones
`i915` programs and Intel's manuals document.

What level 1 still has to solve, per family:

- **Which pipe is showing the firmware framebuffer.** Read each enabled
  plane's address and match it against the framebuffer Limine handed over.
  On AMD the plane holds a GPU address: the framebuffer's offset into the BAR
  plus the base of VRAM in the GPU's address space. On SOC15 parts (Vega and
  later) a register's address is its IP block's base, from
  `vega10_ip_offset.h` or the IP discovery table, plus the offset in the
  generated header.
- **Where the second buffer lives.** Intel: stolen memory, or GGTT entries
  pointing at RAM. AMD chipsets and APUs: the memory the firmware lends the
  graphics core (UMA or SidePort). AMD discrete cards: VRAM inside the BAR
  the CPU can see, outside what the firmware reserved. The display engine
  must see what the CPU wrote, so the mapping or a flush has to account for
  caches (FB2).
- **Turning on a plane the firmware left off.** The cursor may need FIFO or
  watermark space the firmware did not give it; on Intel from Skylake every
  plane, the cursor included, needs its own DDB allocation and watermarks.
- **Leaving the hardware as found**: the firmware's scanout address and
  cursor state restored on session release, on a failed probe and before a
  panic draws.

### Level 2: mode setting

| Family | Machine here | What setting a mode involves | Size |
| --- | --- | --- | --- |
| Intel GMA: 945, 965, G4x | A Core 2 class, no unit recorded ([CORE2_DUO.md](CORE2_DUO.md)) | DPLL, pipe timings, plane, VGA DAC or LVDS and its panel fitter; SDVO on some desktop boards; the panel's timings from EDID or the video BIOS | L per generation |
| Intel Haswell (HD Graphics 4600) | [Core i5-4440](CORE_I5_4440.md) | DDI, the WRPLL/SPLL/LCPLL clocks, transcoders, DisplayPort link training over AUX, HDMI; VGA through the PCH | L–XL |
| Intel Skylake (HD Graphics 520) | [Compaq 21N2F7AR](COMPAQ_21N2F7AR.md), an eDP panel | DDI and Skylake's DPLLs, CDCLK, power wells, DDB and watermarks, eDP panel power sequencing and backlight, the VBT from the ACPI OpRegion | L–XL |
| AMD chipset graphics: 690G (RS690, R500-family display), 760G to 890GX (RS780, RS880, R600-family display) | [FX-4100](AMD_FX_4100.md) (Radeon 3000); the [Athlon II](ATHLON_II.md) class | The video BIOS's AtomBIOS command tables set PLLs, encoders, transmitters and the CRTC; the driver interprets them and picks the output and its timings from EDID | L per family |
| AMD pre-GCN APUs (Llano, Trinity, Richland, Brazos) and GCN APUs before Vega (Kaveri, Kabini, Carrizo, Stoney, Bristol Ridge) | None: the [A6-3500](AMD_A6_3500.md) drives a GeForce | Classic AtomBIOS too, with more registers and DisplayPort or eDP | L–XL |
| AMD Vega 10, discrete (DCE 12) | The [Vega 56](RYZEN5_1600_VEGA56.md) | The video BIOS is atomfirmware, with fewer command tables; Display Core programs most of the hardware itself and asks the SMU for clocks | Not realistic outside `amdgpu` |
| AMD Vega and RDNA APUs (DCN) | None | All of the above, plus DML's floating point; from Renoir on, DMCUB firmware that Display Core loads (`renoir_dmcub.bin`, `green_sardine_dmcub.bin`, `yellow_carp_dmcub.bin`, `dcn_3_1_4_dmcub.bin`) | Not realistic outside `amdgpu` |
| NVIDIA chipsets and cards, VIA | Several targets | No public documentation, as the target documents record | Not planned |

Sizes use the scale of [REAL_HARDWARE.md](REAL_HARDWARE.md): S days, M a
week or two, L several weeks, XL a project.

`amdgpu` still carries display code that does not go through Display Core
for DCE 6, 8 and 10 (`dce_v6_0.c`, `dce_v8_0.c`, `dce_v10_0.c`), driven by
AtomBIOS; Vega's DCE 12 has none. On a UEFI boot there may be no video BIOS
image at `0xc0000`: Linux and Haiku read it from the PCI ROM or from the
ACPI `VFCT` table.

### Level 3

Several outputs at once, hotplug, display power management and HDMI or
DisplayPort audio (the codec needs the GPU side to enable audio and publish
the monitor's capabilities) are a further step on each family, and not
planned.

## Porting the DRM stack instead

- **Cost order:** the layer and the prerequisites first, the same for all;
  then `i915` (335k lines; on the targets' generations the display needs no
  firmware: the DMC, from Skylake on, only enables power-saving states, and
  GuC and HuC are optional), `radeon` (195k lines), `amdgpu` last (1.2M
  lines, about 300k for Vega 10).
- **Firmware:** Vega 10 needs sixteen files from `linux-firmware`, loaded
  through the security processor: `vega10_acg_smc`, `asd`, `ce`, `gpu_info`,
  `ip_discovery`, `me`, `mec`, `mec2`, `pfp`, `rlc`, `sdma`, `sdma1`, `smc`,
  `sos`, `uvd` and `vce` (`.bin`). They are redistributable under AMD's
  license, not MIT, so shipping them is a decision of its own.
- **Upkeep:** OpenBSD moved its tree three times in 26 months
  (6.6.12, 6.12.12, 6.18.16). A port either follows, or freezes on one
  version and gives up the fixes.
- **What it buys that a native driver does not:** the path to hardware 3D
  on metal, and every display feature on families where level 2 is out of
  reach. That is the condition for reconsidering it.

## 3D

None of the native levels accelerates anything; SxGL stays a software
rasterizer on metal ([SXGL.md](../desktop/SXGL.md)). Hardware 3D on metal
needs a kernel driver with memory management and command submission, and a
userland driver: Mesa's `crocus` (Intel Gen4 to Gen7.5), `iris` (Gen8 on),
`r600` (R600 to Cayman) or `radeonsi` (GCN on), with `libdrm`. Virtual
machines have their own path, [VIRGL_ROADMAP.md](../roadmaps/open/VIRGL_ROADMAP.md).

## References and their licenses

Recorded so an adoption starts from the right category in
[THIRD_PARTY_ADOPTION.md](../third-party/THIRD_PARTY_ADOPTION.md). Nothing has
been adopted.

| Source | What it offers | License | Usable as |
| --- | --- | --- | --- |
| Intel's Programmer's Reference Manuals | Registers and sequences for Gen4 onward | Documentation | Reference |
| AMD's register reference guides (RV630, M76, RS690, M56), [xorg.freedesktop.org/docs/AMD/old](https://xorg.freedesktop.org/docs/AMD/old/) | R5xx and R6xx-era display registers. The later documents there cover 3D and instruction sets, not display | Documentation | Reference |
| OpenBSD `drm_linux.c`, `include/linux/` | The list of Linux interfaces the drivers use | ISC | Selective port |
| `radeon`'s `atom.c` (AMD), `atombios_crtc.c`, `atombios_encoders.c` | An AtomBIOS interpreter of about a thousand lines, and the order the tables are called in | MIT-style | Selective port |
| `i915`, `amdgpu`, Display Core | Every sequence, as code | MIT-style per file; check each notice | Reference, selective port |
| Haiku `intel_extreme` | Mode setting only, Gen2 to Gen9.5 and partly later; Haiku calls Sandy Bridge and older well supported and newer ones hit or miss | MIT | Reference, selective port |
| Haiku `radeon_hd` | AtomBIOS mode setting, R600 to Evergreen and partly later; reads the video BIOS from ACPI when needed; called work in progress in Haiku's forum | MIT | Reference, selective port |
| coreboot `libgfxinit` | Mode setting in SPARK: G45, GM45, Arrandale to Coffee Lake, Apollo Lake | GPL-2.0 | Reading only |
| SerenityOS | — | BSD-2 | Its GPU backends are `Do not adopt` by policy |

## Facts still to collect

- **Vega 56:** the BAR the CPU sees (usually 256 MB unless Resizable BAR is
  on; not recorded here yet), the VRAM the firmware reserves, and the GPU
  address of the GOP framebuffer.
- **Core i5-4440:** which output the monitor is on, and the DDI behind it.
- **FX-4100:** where its video BIOS image is on the boot path it uses, and
  how much memory the board lends the graphics core.

## Sources

- [OpenBSD's tree on GitHub](https://github.com/openbsd/src), for every
  measurement above and the commit history of `sys/dev/pci/drm`.
- [NetBSD `amdgpu_module.c`](https://src.rcs.uwaterloo.ca/xref/netbsd-src/sys/external/bsd/drm2/amdgpu/amdgpu_module.c?a=true)
  and [the 2023 report](https://mail-index.netbsd.org/netbsd-bugs/2023/07/25/msg079117.html).
- [`linux-firmware`](https://gitlab.com/kernel-firmware/linux-firmware), the
  `amdgpu/` directory.
- [Haiku's Intel generations](https://haiku-os.org/docs/develop/drivers/intel_extreme/generations.html),
  [Intel hardware status](https://dev.haiku-os.org/wiki/HardwareInfo/video/Intel),
  [`radeon_hd` status, 2011](https://haiku-os.org/blog/kallisti5/2011-10-18_radeon_hd_driver_status_update)
  and [the Polaris thread](https://discuss.haiku-os.org/t/limited-to-vesa-res-on-wx-5100/18892).
- [coreboot's libgfxinit](https://doc.coreboot.org/gfx/libgfxinit.html).
