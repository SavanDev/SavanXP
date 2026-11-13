# AMD Athlon II + chipset graphics

An AM3-era desktop: a K10 processor on an AMD 7- or 8-series board whose
graphics live in the north bridge. It runs `master`. The shared contract and
the numbered work packages (HW-n) are in [REAL_HARDWARE.md](REAL_HARDWARE.md).

**Nothing in this document has been run.** The unit has not been inventoried:
the processor family is known, the board is not. Everything about devices is
**expected** from the platform, and the last section lists what to read from
the machine before trusting any of it.

The board side has since been seen once: the FX-4100 in
[AMD_FX_4100.md](AMD_FX_4100.md) sits on an ASRock 760G + SB7x0 board, the
same chipset this document expects. Its run on 2026-11-13 confirmed the EHCI
and OHCI rows below with a low-speed mouse, showed that "set SATA to AHCI" is
not the whole story (its disk answered on the legacy ATA ports, capped at
128 GiB), and that a PS/2 keyboard is **not** a fallback today (HW-8). The
processor and its C1E check are still unrun.

## Verdict

The CPU meets every hard requirement, and the instruction set is not a
concern. From a DVD or a USB stick the ISO should reach the desktop through
Limine's BIOS path, at a VBE mode, with a PS/2 keyboard and mouse, and USB
through the EHCI and OHCI drivers, which have only run in QEMU. What it will
not have: a network, and disks if the firmware's SATA mode is IDE. One defect can make the machine unusable before any of that
matters -- the local APIC timer under C1E -- and it is the first thing to
check.

## The processor

Athlon II is family 10h without an L3 cache: Regor (X2), Rana (X3) and Propus
(X4) on socket AM3, 2009 to 2011, plus the mobile parts of the same family.

| Feature | Athlon II | Effect on `master` |
| --- | --- | --- |
| Long mode, NX | Yes | Boots |
| SSE3, SSE4a, `POPCNT`, `CMPXCHG16B` | Yes | Unused; the baseline is SSE2 |
| Invariant TSC | Yes (`CPUID.80000007H:EDX[8]`) | The TSC is a sound wall clock |
| x2APIC | No | xAPIC over MMIO, as on the Ryzen host |
| SMEP, SMAP | No | Boots with `cpu: SMEP unavailable on this processor` |
| `RDRAND` | No | The TSC-mixed fallback seeds ASLR |
| Cores | 2 to 4 | Started by Limine; well inside `kMaxCpus` |
| C1E | Enabled by most firmware | Stops the local APIC timer; the kernel switches it off on every core (HW-9) |

### C1E is the first check

Family 10h is in the range of AMD's erratum 400: with C1E enabled, once every
core has executed `hlt` the processor enters a state where the local APIC
timer stops. `master` takes its tick from that timer and idles with `hlt`. An
idle desktop would then stand still until something else interrupts it, and
move again in bursts.

The kernel now clears the C1E bits on every core and before every idle halt
(HW-9), and says what it found: `cpu: AMD erratum 400 range, C1E on halt was
enabled by the firmware; disabled`. That has only run in QEMU. On the
machine, leave it idle on the desktop and read `timer-stats:` in Log Viewer:
`perdido` should stay in single-digit percent. If it does not, turn "C1E
support" off in the firmware and compare.

## Chipset graphics

"Integrated graphics" on this platform is one of these, by board:

| Chipset | Graphics | PCI ID |
| --- | --- | --- |
| AMD 760G | Radeon 3000 | `1002:9616` |
| AMD 780G | Radeon HD 3200 | `1002:9610` |
| AMD 785G | Radeon HD 4200 | `1002:9710` |
| AMD 880G | Radeon HD 4250 | `1002:9715` |
| AMD 890GX | Radeon HD 4290 | `1002:9714` |
| NVIDIA MCP61, MCP68, MCP78 | GeForce 6150SE, 7025, 8200 | `10de:*` |

**Today:** the board's video BIOS sets a VBE mode for Limine and the kernel
draws into it (HW-11). The default entry asks Limine for the monitor's
preferred mode (EDID); a VBIOS of this generation offers widescreen modes
only sometimes, and Limine falls back to another mode when it lacks it. The
**SavanXP (1024x768)** entry forces the mode it offers reliably. The framebuffer comes out of system RAM the firmware lends to
the graphics core (and SidePort memory on boards that have it), so the mode
list also depends on the "share memory" setting.

**Native mode setting** is plausible for the AMD chipsets and not for the
NVIDIA ones. RS780 and RS880 are R600-family display hardware driven through
AtomBIOS: the video BIOS carries command tables that set a mode, and a driver
is an interpreter for those tables plus the code that picks an output and its
timings from EDID. That is an L package, it buys the monitor's native
resolution, mode changes from Display Properties and a page flip, and it buys
no acceleration. It should wait until the machine has been run on VBE and
found wanting.

The graphics core also exposes its own HD Audio function for HDMI
(`1002:960f`, `1002:970f`). Like the Vega's, it is a second controller that
must not be chosen over the analog one, and it produces no sound without a
native display driver.

## The rest of the board

| Device | Expected | Today | Package |
| --- | --- | --- | --- |
| Firmware | Legacy BIOS; UEFI only on the last 8- and 9-series boards | Limine BIOS, DVD or `dd`-written stick | — |
| SATA | SB700, SB710, SB750 or SB850. One controller with a firmware setting: IDE (`1002:4390`), AHCI (`1002:4391`) or RAID | **Set it to AHCI** and the existing driver should find the disks. In native IDE mode the legacy-port ATA driver does not reach them | HW-3, HW-4 |
| PATA | One channel on the south bridge (`1002:439c`) | Not driven unless the firmware leaves it at the legacy ports | HW-3 |
| USB | OHCI companions (`1002:4397`, `4398`, `4399`) and EHCI (`1002:4396`). xHCI only as a separate chip on some late boards | EHCI and OHCI drivers. **Verified** on an SB7x0 board (FX-4100, 2026-11-13) with a low-speed mouse after the firmware handoff. A PS/2 keyboard is not a fallback until HW-8 | HW-2, HW-8 |
| Analog audio | SB7x0 HD Audio `1002:4383` with a Realtek codec: ALC662, ALC887, ALC888, ALC892 | The controller attaches without the SBx00 snooping quirk; sound is unverified. A board with an ALC892 has the one codec that has been heard | HW-6 |
| Ethernet | Realtek RTL8111 (`10ec:8168`) on most boards; RTL8101/8102 (`10ec:8136`) or Atheros on some; NVIDIA's own MAC on nForce boards | No network | HW-1, HW-15 |
| Input | PS/2 port, often one shared keyboard/mouse connector | Works with the existing driver | — |
| Timers | HPET and the ACPI PM timer present | HPET unused. The wall clock is the PM timer if 32 bits wide, the TSC otherwise; both are sound here | HW-9 |
| Memory | 2 to 8 GiB of DDR2 or DDR3 | Enough for the LiveCD | — |

A stopgap with hardware that costs almost nothing, not verified: a PCI
RTL8139 card is a network with the driver that exists.

## Work for this target

1. **Run it as it is.** Boot log on screen, PS/2 input, SATA set to AHCI.
   Record the `boot:`, `cpu:`, `pci:`, `timer:`, `smp:`, `fb_gpu:`, `ahci:`
   and `azalia:` lines and the `timer-stats:` behaviour idle
   ([reading the log on the machine](REAL_HARDWARE.md#reading-the-log-on-the-machine)).
2. **HW-9**, the rest of it, if the tick is still lost with C1E cleared.
3. **HW-6.** The SBx00 row of `azalia_configure_pci`, then the codec.
4. **HW-1.** Most likely the same Realtek family as the development host, so
   this arrives with the work done there.
5. **HW-2** on metal: the `ehci:` and `ohci:` lines, a USB keyboard (likely
   low speed, which QEMU never showed), a stick at high speed.
6. **HW-3**, for firmware that cannot be switched to AHCI and for the PATA
   channel.
7. **HW-11**, the mode choice first; AtomBIOS mode setting only if the VBE
   mode list is the thing in the way.

## Facts to collect

- `lscpu`: the exact model and stepping.
- `lspci -nn`: north bridge and graphics ID, south bridge, the SATA function's
  ID in the current firmware mode, the USB controllers, the NIC.
- `/proc/asound/card*/codec*`: the codec and its pin configuration.
- Firmware settings: SATA mode, C1E, Cool'n'Quiet, USB legacy support, the
  memory lent to graphics.
- `dmesg | grep -i -E 'c1e|hpet|pm-timer|tsc'` from Linux, for what another
  kernel concluded about the timers.
- From the SavanXP boot log: the VBE mode Limine got, the wall clock source,
  the cores online, and whether a USB keyboard works at all through the
  firmware's PS/2 emulation.
