# Intel Core 2 Duo + chipset graphics

A 2006-2010 Intel desktop or laptop: a Core-microarchitecture processor, an
ICH south bridge and Intel GMA graphics in the north bridge. It runs
`master`. The shared contract and the numbered work packages (HW-n) are in
[REAL_HARDWARE.md](REAL_HARDWARE.md).

**Nothing in this document has been run.** The unit has not been inventoried.
Everything about devices is **expected** from the platform, and the last
section lists what to read from the machine first.

## Which processor this is

The owner confirmed on 2026-11-12 that the unit is a **Core 2 Duo**, the
first row below; an earlier description, "Intel Pentium 2 Core Duo", was a
typo. The exact model is not known yet: the boot log's first `cpu:` line
gives it. The table stays because the names are easy to confuse, and one of
them cannot run `master`:

| Name on the case | Core | Long mode | Branch |
| --- | --- | --- | --- |
| Core 2 Duo (E4xxx, E6xxx, E7xxx, E8xxx; T5xxx to T9xxx, P8xxx) | Conroe, Wolfdale, Merom, Penryn | Yes | `master` |
| Pentium Dual-Core (E2xxx, E5xxx, E6xxx; T23xx, T24xx, T3xxx, T4xxx) | The same cores with less cache | Yes | `master` |
| **Core Duo** (T2xxx, L2xxx), and Pentium Dual-Core T20xx, T21xx | **Yonah** | **No** | `legacy`, like the [Pentium 4](PENTIUM4_EVO_N1020V.md) |

On a Core Duo, Limine stops with *"This CPU does not support 64-bit mode."*

## Verdict

The CPU meets every hard requirement. The one firmware setting that could make
it look as if it does not, the XD bit, the kernel now undoes (HW-10). The ISO should reach the desktop
through Limine's BIOS path at a VBE mode, with PS/2 input -- the built-in
keyboard on a laptop -- and USB through EHCI and UHCI, which have only run in
QEMU. It will most likely have no network, and on the most common desktop
south bridge of the period no AHCI either. Of the
three older targets this is the one where native graphics mode setting is
most within reach.

## The processor

| Feature | Core 2 | Effect on `master` |
| --- | --- | --- |
| Long mode | Yes | Boots |
| XD (NX) | Yes, switchable in the firmware | Switched back on by the kernel if the firmware turned it off (HW-10) |
| SSE3, SSSE3, `CMPXCHG16B` | Yes | Unused; the baseline is SSE2 |
| TSC | Constant rate across frequency changes; stops only in deep idle states the kernel never requests. The CPUID invariant-TSC bit is not set | Sound as a wall clock. A future selection by that CPUID bit (HW-9) must not demote this family |
| Local APIC timer | Runs in C1, stops in C3 and deeper | Safe: the kernel idles with `hlt` |
| x2APIC, SMEP, SMAP, `RDRAND` | No | xAPIC over MMIO; `cpu: SMEP unavailable on this processor`; fallback entropy |
| Cores | 2 | Started by Limine |

### The XD bit

The kernel clears the firmware's "XD disable" bit on every core before it
looks for NX (HW-10) and logs `cpu: XD was disabled by the firmware;
re-enabled` when it did. That has only run in QEMU: if the first boot still
panics with `cpu: NX/W^X is required`, turn "Execute Disable Bit" (or "XD",
"No-Execute Memory Protection") on in the firmware and record which it was.

## Chipset graphics

| Chipset | Graphics | PCI ID | Typical board |
| --- | --- | --- | --- |
| 945G | GMA 950 | `8086:2772` | Early desktops |
| Q965, G965 | GMA 3000, X3000 | `8086:2992`, `8086:29a2` | Business desktops |
| G31, G33, Q35 | GMA 3100 | `8086:29c2`, `8086:29b2` | Budget desktops |
| G35 | GMA X3500 | `8086:2982` | |
| G41 | GMA X4500 | `8086:2e32` | The commonest late board |
| G43, G45, Q45 | GMA X4500, X4500HD | `8086:2e22`, `8086:2e12` | |
| 945GM | GMA 950 | `8086:27a2` | Laptops, 2006-2007 |
| GM965, GL960 | GMA X3100 | `8086:2a02` | Laptops, 2007-2008 |
| GM45, GS45 | GMA 4500MHD | `8086:2a42` | Laptops, 2008-2010 |

**Today:** VBE through Limine (HW-11) at the monitor's preferred mode when the
video BIOS lists it; the **SavanXP (1024x768)** entry forces `1024x768x32`,
which every one of these video BIOSes lists. There is a defect particular to
Intel. The
video BIOS of these chipsets carries a fixed mode table that usually lacks
the widescreen sizes -- 1280x800, 1366x768, 1440x900, 1680x1050 -- even on a
laptop whose own panel is one of them. Limine then falls back to a 4:3 mode
and the panel scales it. The mode must also fit in the few megabytes of
"stolen" memory the firmware reserved, which on GMA 950 can exclude 32 bpp at
the largest sizes.

**Native mode setting** is the realistic fix here, and this is the best
documented of the graphics families in this directory: Intel published the
register reference for these generations, and independent drivers exist that
do only mode setting. The work is reading the panel timings (EDID, or the
video BIOS table on a laptop's internal panel), programming the PLL, the pipe
and the plane, and the LVDS or analog output. An L package per generation
(945, 965, G4x), giving the native resolution, mode changes and a page flip.
No acceleration.

## The rest of the machine

| Device | Expected | Today | Package |
| --- | --- | --- | --- |
| Firmware | Legacy BIOS | Limine BIOS, DVD or `dd`-written stick | — |
| SATA | ICH7 (945, G31, G41): IDE mode only on the plain desktop part (`8086:27c0`); AHCI on ICH7R and ICH7-M (`27c1`, `27c5`). ICH8, ICH9, ICH10: AHCI on most variants (`2821`, `2922`, `3a22`), often shipped set to IDE | In AHCI mode the existing driver should work. In IDE mode the disks answer at the legacy ports only if the firmware chose "compatible" or "combined", and then in PIO | HW-3 |
| PATA | ICH7 has one channel; later ones need an extra chip | As above | HW-3 |
| USB | Four to six UHCI controllers and one or two EHCI | EHCI and UHCI drivers, run only in QEMU on this very layout (ICH9: an EHCI and three UHCI at `00:1d`). Low-speed keyboards and mice and the firmware handoff are first seen here. A laptop's built-in keyboard is PS/2 either way | HW-2 |
| Audio | HD Audio on ICH7 and later (`8086:27d8`, `284b`, `293e`, `3a3e`). Codecs by vendor: Realtek ALC66x/88x on retail boards, IDT/SigmaTel on Dell and HP, Analog Devices on ThinkPads, Conexant on many laptops. AC'97 (`8086:27de`) on some 945 boards | The controller attaches without Intel's traffic-class quirk; no codec of these has been heard | HW-6, HW-7 |
| Ethernet | Intel 82566/82567 or 8257x on Intel and business boards; Realtek RTL8111 (`10ec:8168`) or RTL8101 (`10ec:8136`) on consumer boards and laptops; Broadcom on Dell and HP; Marvell or Atheros on some | No network | HW-1, HW-15 |
| Pointer, laptop | Synaptics or ALPS pad on the 8042's second port | Unverified; a pad that rejects the init sequence leaves no pointer | HW-8 |
| Timers | HPET on ICH7 and later, sometimes left disabled by old firmware. The PM timer is expected to be 24 bits wide | Wall clock on the TSC, which is sound on this CPU | — |
| Battery, lid, brightness, fan policy | Embedded controller through ACPI | None. The fan keeps its firmware default | HW-14 |
| Memory | 1 to 8 GiB | The LiveCD wants 1 GiB at the least | HW-12 |

The stopgap of the other desktops applies to a desktop here too: a PCI
RTL8139 card for a network. A laptop has no slot for it.

## Work for this target

1. **Run it as it is** with the boot log on screen.
   Record the `boot:`, `cpu:`, `pci:`, `timer:`, `smp:`, `fb_gpu:`, `ata:`,
   `ahci:` and `azalia:` lines
   ([reading the log on the machine](REAL_HARDWARE.md#reading-the-log-on-the-machine)).
2. **HW-10** on metal: whether `cpu: XD was disabled by the firmware` appeared,
   with the firmware setting it was booted with.
3. **HW-3.** On an ICH7 desktop this is the only way to a disk.
4. **HW-2** on metal: `ehci:` and `uhci:` lines, a USB keyboard (likely low
   speed, which QEMU never showed), a stick at high speed.
5. **HW-6**: Intel's row of `azalia_configure_pci`, then the codec the unit
   has.
6. **HW-1 or HW-15**, whichever controller the unit has.
7. **HW-11**: the mode choice first. If the VBIOS has no mode for the panel,
   native mode setting for that chipset generation is the next step, and this
   target is where it would be written first.
8. **HW-8 and HW-14**, if the unit is a laptop.

## Facts to collect

- `lscpu`: the model name and whether `lm` and `nx` are among the flags.
- `lspci -nn`: the graphics ID (it names the chipset generation), the SATA
  function's ID in the current firmware mode, the USB controllers, audio and
  NIC.
- `/proc/asound/card*/codec*`: the codec.
- Whether it is a desktop or a laptop, and the panel's native resolution.
- Firmware settings: Execute Disable Bit, SATA mode, HPET, USB legacy support.
- From Linux, `sudo hwinfo --framebuffer`: the VBE modes the video BIOS
  really offers at 32 bpp.
