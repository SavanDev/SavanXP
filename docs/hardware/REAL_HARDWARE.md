# Real hardware targets

SavanXP is developed in QEMU and VirtualBox. This directory records what it
takes to run it on physical machines: one document per target, and this one
for what they share — the contract `master` imposes on any machine, the driver
work several targets need, and how a run on metal is prepared and recorded.

Three kinds of statement appear in these documents, and they are not
interchangeable:

- **Verified** — seen on the machine, with the date and what was run.
- **In tree** — read from the code on the date given. It says what the kernel
  does, not that the device works.
- **Expected** — what the platform's public documentation says about a class
  of machine. The exact unit has not been inventoried; treat it as a list of
  things to confirm, not as facts about the hardware.

## The targets

| Target | Document | Branch | Status |
| --- | --- | --- | --- |
| AMD Ryzen 5 1600 + Radeon RX Vega 56 (ASRock B450M Pro4-F R2.0) | [RYZEN5_1600_VEGA56.md](RYZEN5_1600_VEGA56.md) | `master` | **Verified:** boots from USB under UEFI, desktop at 1920x1080 on the firmware framebuffer, all 12 processors, USB keyboard and mouse, four SATA disks read, analog audio in and out. No Ethernet, NVMe or native Vega driver |
| Intel Pentium 4, Compaq Evo N1020v | [PENTIUM4_EVO_N1020V.md](PENTIUM4_EVO_N1020V.md) | `legacy` (i386) | Cannot run `master`: no long mode, no NX. The i386 port on `legacy` boots, mounts SxFS and runs one ring 3 program in QEMU |
| AMD Athlon II + chipset graphics | [ATHLON_II.md](ATHLON_II.md) | `master` | Not run. **Expected** to reach the desktop; USB through EHCI and OHCI, run only in QEMU; no network |
| Intel Core 2 Duo + chipset graphics | [CORE2_DUO.md](CORE2_DUO.md) | `master` | Not run. **Expected** to reach the desktop; USB through EHCI and UHCI, run only in QEMU; no network |
| AMD Athlon 64 + chipset graphics | [ATHLON_64.md](ATHLON_64.md) | `master` | Not run. The oldest CPU `master` can boot; storage, USB and the wall clock all need work first |
| Intel Core i7-6500U, Compaq Presario 21N2F7AR | [COMPAQ_21N2F7AR.md](COMPAQ_21N2F7AR.md) | `master` | **Verified:** boots from USB through the legacy BIOS path, desktop at 1920x1080, built-in keyboard and touchpad, USB mouse, SATA disk, internal speakers, jack and microphone, on battery. No Ethernet (`10ec:8168`); UEFI not yet run |
| AMD A6-3500 + GeForce GT 610 (Gigabyte, A75) | [AMD_A6_3500.md](AMD_A6_3500.md) | `master` | **Verified** (2026-11-13): legacy BIOS boot from USB, desktop at 1024x768, low-speed USB keyboard and mouse through EHCI/OHCI. SATA in native IDE mode, so no disk (HW-3); steady 7% tick loss; no Ethernet; audio attached, not heard (no speakers) |
| AMD FX-4100 (ASRock, 760G + SB7x0) | [AMD_FX_4100.md](AMD_FX_4100.md) | `master` | **Verified** (2026-11-13): legacy BIOS boot from USB, desktop at 1280x1024, low-speed USB mouse through OHCI, a SATA disk on the legacy ATA ports capped at 128 GiB. The **PS/2 keyboard did not work** (HW-8); Atheros Ethernet with no driver; audio attached, not heard |
| Intel Core i5-4440 (MSI MS-7817, H81) | [CORE_I5_4440.md](CORE_I5_4440.md) | `master` | **Verified** (2026-11-13): UEFI boot from USB, desktop at 1920x1080, low-speed USB keyboard and mouse on xHCI, SATA SSD on AHCI. Steady 7% tick loss; no Ethernet (`10ec:8168`); audio attached, not heard |

Five machines have run `master`, the last runs all from v0.4.3 (Build 701);
the A6, FX and i5 runs are known from their boot logs only, with no Linux
inventory. The Athlon II, Core 2 Duo and Athlon 64 targets are known by CPU
family, which decides what the kernel may assume about the processor but not
which chipset, disk controller, NIC or audio codec is on the board. Each of
those documents ends with the facts to collect from the unit.

### What the 2026-11-13 runs showed across machines

- **Every machine reached the desktop**, under both boot paths, at the
  monitor's preferred mode where the firmware offered it.
- **EHCI, OHCI and low-speed devices work on metal** (HW-2): a Dell keyboard
  and two mice behind OHCI companions on the A75 and SB7x0, and low-speed
  devices on xHCI on the Haswell and Zen boards.
- **Four of the five `master` machines carry the `10ec:8168`** (HW-1): the
  Ryzen, the Compaq, the A6 and the i5. The FX has an Atheros AR8151 (HW-15).
- **Long reads under the global lock** cause the media player's micro-cuts,
  below.
- **A steady tick loss** on two machines, below in HW-9.

### Long reads under the global lock

On every machine where the media player ran, `smp-stats:` names POSIX syscall
#0, `read`, as the longest holder of the global lock while it plays:

| Machine | Windows naming `posix#0` | Longest hold |
| --- | --- | --- |
| Ryzen 5 1600 | 129 | 978 ms |
| A6-3500 | 68 | 55 ms |
| FX-4100 | 54 | 54 ms |
| i5-4440 | 29 | 47 ms |
| i7-6500U (player not run) | 1 | 10 µs |

During those windows the tick loses time and the player's `audiod` port
counts underruns by the tens of thousands; on the Ryzen, the only machine
with speakers in these runs, they are heard as micro-cuts. The 978 ms hold
also stopped the HD Audio stream (`azalia: DMA polling deadline missed`).
`windowd` reported the player as not responding once on the A6 and once on
the i5. The file was most likely on the USB stick, the only data volume
mounted in each run, which points at a block read done with the global lock
held; that is not confirmed. It is a kernel problem, not a hardware one, and
QEMU should be able to reproduce it with the same file on a USB disk.

## What `master` requires of a machine

Read from the tree on 2026-11-12. A machine that fails a **hard** row does not
boot; a machine that fails a soft one boots with something missing.

| Area | Requirement | Where |
| --- | --- | --- |
| CPU, hard | Long mode. Limine refuses a CPU without it before the kernel is loaded | [Limine protocol](https://github.com/limine-bootloader/limine-protocol/blob/trunk/PROTOCOL.md): *"Only 64-bit, Little Endian machines are supported"* |
| CPU, hard | NX. The BSP panics with `cpu: NX/W^X is required`; an AP without it halts. On Intel, XD switched off in the firmware is switched back on first (HW-10) | `enable_nx()` in `arch/x86_64/cpu_init.cpp`, [KERNEL_SECURITY.md](../kernel/KERNEL_SECURITY.md) |
| CPU, soft | Local APIC; xAPIC over MMIO or x2APIC over MSRs. Without one the PIT drives a single core | `initialize_local_apic()`, `arch/x86_64/timer.cpp` |
| CPU, soft | SMEP and SMAP, each enabled when present (`cpu: SMEP unavailable on this processor`, `cpu: SMAP unavailable on this processor` otherwise). `RDRAND`, with a TSC-mixed fallback | `enable_smep()`, `enable_smap()`, [KERNEL_SECURITY.md](../kernel/KERNEL_SECURITY.md) |
| CPU, limit | 32 logical processors (`arch::kMaxCpus`) | `include/arch/x86_64/cpu.hpp` |
| Instruction set | Plain `x86_64`: SSE2 and nothing later. No `-march`, no intrinsics, no hand-written assembly beyond the baseline; FFmpeg is built with `--disable-asm`. Syscalls are `int $0x80`, so nothing depends on `SYSCALL`/`SYSRET` | `CMakeLists.txt`, `tools/build-user.sh`, `ports/*/` |
| Firmware | Legacy BIOS with INT 13h extensions, or 64-bit UEFI. Limine 10.8.5; the ISO is hybrid (El Torito for both, and `dd`-able to a USB stick). The 32-bit UEFI loader is not staged | `boot/limine.conf`, `CMakeLists.txt` (`savanxp_iso`) |
| ACPI | An RSDP from the loader and a FADT. A MADT moves interrupts to the IOAPIC with its source overrides applied; without one the legacy PIC is used. uACPI resolves `_PRT` | `kernel/acpi.cpp`, `kernel/ioapic.cpp`, `kernel/uacpi_glue.cpp` |
| Clocks | A CMOS RTC: the LAPIC timer is calibrated across two RTC second edges. PIT channel 2: the TSC is calibrated against it. The wall clock is the ACPI PM timer when it is 32 bits wide and the **TSC otherwise** | [TIME.md](../kernel/TIME.md) |
| Video | A linear 32 bpp framebuffer from the firmware (VBE or GOP). The ISO's boot entries let Limine take the monitor's preferred mode from EDID, and a **SavanXP (1024x768)** entry forces the VESA mode every video BIOS offers; the development configuration keeps `1280x800x32`, which the QEMU scenarios click against. No mode setting after boot outside QEMU's adapters; all drawing is software | `kernel/fb_gpu.cpp`, `kernel/console.cpp` |
| Memory | The LiveCD root is a 256 MiB SxFS volume Limine loads into RAM as a module, next to the kernel and the initramfs. The floor has not been measured; 1 GiB is the least worth trying | `SAVANXP_ISO_SECTORS` in `CMakeLists.txt` |
| Storage | AHCI: the first HBA only, four disks. ATA: the two legacy channels at `0x1F0`/`0x170`, PIO, 28-bit LBA commands. USB mass storage behind xHCI. No NVMe, no ATAPI | `kernel/ahci.cpp`, `kernel/ata.cpp`, `kernel/usb/umass.cpp` |
| USB | xHCI, EHCI, UHCI and OHCI. Low speed and split transactions have not been run: QEMU cannot show them | [USB_HOST_CONTROLLERS.md](../kernel/USB_HOST_CONTROLLERS.md) |
| Input | PS/2 (8042) keyboard and mouse, or USB HID behind xHCI | `kernel/ps2.cpp`, [INPUT.md](../kernel/INPUT.md) |
| Network | Realtek RTL8139 (`10ec:8139`) and nothing else | `kernel/rtl8139.cpp` |
| Audio | HD Audio: any class `04/03` controller, with the PCI quirk of one (`1022:1457`). AC'97: any class `04/01` function, driven as an Intel ICH. No HDMI or DisplayPort audio | [AZALIA.md](../kernel/AZALIA.md), `kernel/ac97.cpp` |
| PCI | Configuration mechanism #1, every bus scanned, a flat table of 256 functions. Each one is logged as a `pci:` line; what does not fit is counted in `pci: table full` | `kernel/pci.cpp` |
| Power | Power button, S5 and reboot. No sleep, no CPU frequency or idle states, no thermal handling; every GPE is masked | `kernel/acpi.cpp`, `kernel/power.cpp` |

### The instruction-set baseline is a compatibility rule

Every program in the tree is compiled for the original AMD64 instruction set.
That is why an Athlon 64 from 2003 is a possible target at all: it has no
SSE3, no `CMPXCHG16B` and no `LAHF`/`SAHF` in long mode, which is exactly the
set other 64-bit systems started requiring when they dropped it.

Raising the baseline — a `-march=x86-64-v2`, an intrinsic, a port built with
its assembly enabled — retires targets. It must be a decision recorded here,
with the targets it removes, not a flag added to a build script. Code that
wants a newer instruction detects it at run time and keeps the plain path.

## How a machine boots

The same ISO serves every `master` target (`./build.sh iso`,
[BUILD_LINUX.md](../build/BUILD_LINUX.md)):

1. The firmware starts Limine from a DVD or from a USB stick the ISO was
   written to with `dd`. Limine reads the kernel, the initramfs and the
   256 MiB volume **through the firmware** and leaves them in RAM.
2. The kernel mounts that volume as the `livecd` ramdisk, unless a disk it can
   read already holds an SxFS volume: disks outrank the ramdisk, so an
   installed system wins ([SETUP_AND_UPDATES.md](../userland/SETUP_AND_UPDATES.md)).

Two consequences decide what "boots" means on an old machine. The kernel needs
**no driver for the boot medium**: a machine with no AHCI, no ATAPI and no USB
driver still reaches the desktop from a DVD or a stick, because the firmware
did the reading. And the system it reaches is throwaway: nothing written
survives a reboot until the kernel can drive a disk in that machine and setup
exists.

The menu has two more entries for metal, **boot log on screen** (`verbose`)
and **no USB** (`nousb`). On a machine without a serial port the first one is
the only way to read where the boot stopped.

## Shared work packages

Driver and platform work that more than one target needs, numbered so the
target documents can refer to it. Sizes are S (days), M (a week or two),
L (several weeks), XL (a project). "Reference" names the OpenBSD driver to
read or port under the [adoption policy](../third-party/THIRD_PARTY_ADOPTION.md),
as `azalia` and the xHCI quirks were.

| # | Work | Size | Ryzen | Athlon II | Core 2 | Athlon 64 | Pentium 4 |
| --- | --- | --- | --- | --- | --- | --- | --- |
| HW-1 | Realtek RTL8111/8168/8101 Ethernet | M | **needed** | likely | common | some | — |
| HW-2 | USB before xHCI: EHCI, then UHCI and OHCI | L | — | done in QEMU; run it | done in QEMU; run it | done in QEMU; run it | `legacy` has none |
| HW-3 | PCI IDE: native-mode BARs, bus-master DMA, 48-bit LBA | M | — | unless AHCI is selected | **needed** on ICH7 | **needed** | **needed** |
| HW-4 | AHCI: every HBA, more than four disks, controller quirks | S | at the limit | useful | useful | on SB600 | — |
| HW-5 | NVMe | M | **needed** to install | — | — | — | — |
| HW-6 | HD Audio: the other controllers' PCI quirks, more codecs | S + per codec | done for this board | **needed** | **needed** | on late boards | — |
| HW-7 | AC'97: bind by controller, not by class; non-Intel controllers | S–M | — | — | on 945 boards | **needed** | **needed** |
| HW-8 | PS/2 pointer on real pads | S | — | — | laptops | — | **needed** |
| HW-9 | Timers on old CPUs: C1E (done), NetBurst APIC timer, a wall clock that is not the TSC | S–M | — | C1E done; check `timer-stats:` | low risk | wall clock **needed** | **needed** |
| HW-10 | Intel XD bit disabled by firmware | XS | **done** | | | | |
| HW-11 | Video: choosing the firmware mode, reporting it, native mode setting | S, then L per GPU family | mode choice | mode choice | mode choice; native is plausible | mode choice | mode choice |
| HW-12 | Running from a disk: setup, and a LiveCD that fits small memory | roadmap | to install | to install | to install | **needed** below 1 GiB | **needed** |
| HW-13 | PCI table size and a truncation message | XS | **done**: 256, overflow logged | | | | |
| HW-14 | ACPI on real firmware: GPEs, EC, battery, lid, thermal, CPU frequency | L | later | later | laptops | later | **needed** for a laptop |
| HW-15 | Other Ethernet controllers | M each | — | per board | per board | per board | per unit |

### HW-1 — Realtek RTL8111/8168/8101

The gigabit controller of the development host (`10ec:8168`) and of most
consumer boards from 2007 on; `10ec:8136` is its 10/100 sibling on budget
boards and laptops, `10ec:8169` the PCI gigabit ancestor. One driver covers
the family. Reference: OpenBSD `re(4)` (`sys/dev/ic/re.c`, `if_re_pci.c`).
It plugs into `nic::register_driver` beside `rtl8139`.

This is the single package with the most reach: the development host and the
Compaq 21N2F7AR both have a `10ec:8168`, and it is what stands between them
and everything that needs a network on metal (`ntpsync`, Web
Viewer, the SUN sockets).

Until it exists, a PCI RTL8139 card is the one way to get a network on any
desktop with a PCI slot. The driver has only ever run in emulators as far as
this tree records, so that card is also its first test on metal.

### HW-2 — USB before xHCI

Every machine before about 2012 routes its ports to an EHCI controller for
high speed and to UHCI (Intel, VIA) or OHCI (AMD, ATI, NVIDIA, ALi, SiS)
companions for low and full speed. With xHCI alone such a machine had no USB
keyboard, mouse or stick once the kernel was running.

**Done in QEMU** (2026-11-12): EHCI, UHCI and OHCI drivers beside xHCI, all
polled, with EHCI handing low- and full-speed devices to its companions
([USB_HOST_CONTROLLERS.md](../kernel/USB_HOST_CONTROLLERS.md)). Keyboards
behind a hub, a stick, and hot plug pass on each (`uhci-smoke`,
`ohci-stick-smoke`, `ehci-hotplug-smoke` and the rest). Log:
`ehci: 0:1d.7 8086:293a, 6 ports, 3 companions`, `uhci: 0:1d.0 8086:2934 at
I/O 0x66a0, 2 ports`, `ohci: 0:3.0 106b:3f, revision 10, 4 ports`, and
`ehci: port 3: full-speed device, handed to the companion`.

**Run on metal** (2026-11-13): on the A75 ([AMD_A6_3500.md](AMD_A6_3500.md))
and the SB7x0 ([AMD_FX_4100.md](AMD_FX_4100.md)), EHCI handed low-speed
devices to OHCI and a Dell keyboard and two mice worked through it, after the
firmware's handoff. The H81 ([CORE_I5_4440.md](CORE_I5_4440.md)) brought up
both EHCI controllers with their rate-matching hubs, with nothing behind them.

**Left for the machines:** split transactions behind a high-speed hub (QEMU
has none; an Intel 7-series or later board with a device on an EHCI port is
the test), UHCI on any Intel ICH, and the other chipsets' quirks. A driver that takes a controller ends the firmware's PS/2
emulation of a USB keyboard on it; if USB then fails, the ISO's *no USB* entry
(`nousb`) leaves every controller to the firmware.

### HW-3 — PCI IDE

`kernel/ata.cpp` talks to the two legacy channels at fixed ports, in PIO, with
28-bit LBA commands, and never looks at PCI. That reaches a disk only when the
firmware left the controller in compatibility mode with its decode enabled.
What a real IDE-class controller needs:

- finding the function (class `01/01`, and the SATA controllers that present
  themselves as `01/01` or as RAID `01/04` in "IDE" or "native IDE" mode),
  reading per channel whether it is in native mode, taking the task-file ports
  from BARs 0–3 when it is, and enabling I/O decode and bus mastering in the
  command register;
- 48-bit LBA commands. `IDENTIFY` already records the full size, but a disk
  past 128 GiB cannot be addressed beyond that point;
- bus-master DMA through BAR 4. PIO moves every sector through the CPU;
  [AHCI.md](../kernel/AHCI.md) has what that costs;
- ATAPI, last and only for reading optical media after boot. Booting from a
  DVD does not need it.

Reference: OpenBSD `pciide(4)` and `wdc(4)`. This is the storage driver of
every pre-AHCI machine, and of later ones whose firmware is set to IDE mode.

Seen on metal on 2026-11-13. The A6-3500's A75 SATA was in native IDE mode
(class `01.01.8f`) and no disk was found. On the FX-4100's SB7x0 the SATA
function was in AHCI mode, yet the disk answered on the legacy primary
channel, probably through the south bridge's "IDE combined" ports, and was
reported as `268435455 sectors`: the 28-bit ceiling, 128 GiB of a 160 GB
disk. The first disk `ata.cpp` has found on metal, and the first to show the
limit.

### HW-4 — AHCI coverage

`find_hba()` keeps the first AHCI function and `kMaxDisks` is 4. The
development host has two HBAs, but all four of its disks are on the first
one (`02:00.1`), so today it sees every disk; a fifth, or one on the FCH
controller, would be invisible. The `ahci:` line should name the HBA.
Also wanted: ATAPI ports, and the per-vendor exceptions real HBAs need — the
ATI SB600 advertises 64-bit DMA and does not honour it.

### HW-5 — NVMe

Only the development host has one, and it is the disk the system would be
installed to. Reference: OpenBSD `nvme(4)`, which is polled at attach and
small. One `block::Driver` like the others.

### HW-6 — HD Audio beyond the development host

The port binds any class `04/03` controller but carries one row of OpenBSD's
`azalia_configure_pci`: PCIe snooping for `1022:1457`. The rows the other
targets need are in the vendored source and not compiled: ATI SB450/SBx00
(`1002:437b`, `1002:4383`) and NVIDIA MCP snooping, and Intel's traffic-class
select. Without them a controller may attach and play silence or noise.

On 2026-11-13 the SB7x0 controller (`1002:4383`, FX-4100) and the A75's
(`1022:780d`, A6-3500) both attached and found an output and a capture path,
with no quirk applied. Neither machine had speakers, so whether they play is
still open.

Codecs are a separate question. The graph walk is generic, the quirk table is
not compiled, and only the ALC892 and ALC269 have been heard. The ALC887
(A6, i5) and ALC662 (FX) attached and were not heard. Each new codec (ALC66x/88x,
AD198x, IDT 92HD, Conexant) is its own check, with the pin dump recorded in
the target document as it was for the ALC892 ([AZALIA.md](../kernel/AZALIA.md)).

### HW-7 — AC'97 controllers

`ac97.cpp` claims the first class `04/01` function and drives it with the
Intel ICH register layout through two I/O BARs. Intel ICH0–ICH7, NVIDIA
nForce and SiS 7012 follow that layout closely; ATI IXP (SB400), VIA VT823x
and ALi M5451 are different controllers that happen to carry an AC'97 codec.
On those, today's driver writes Intel registers into someone else's device.

First the binding becomes a list of controllers known to match, so an unknown
one is left alone. Then the two fixes real ICH hardware wants: ICH6 and ICH7
may have the legacy I/O BARs disabled in favour of the memory ones, and the
mixer needs a real delay after its reset, not a spin count. Other controllers
(`auixp`, `auvia`, `auacer` in OpenBSD) only when a target has one.

### HW-8 — PS/2 devices

**The keyboard failed on the first desktop that had one.** On the FX-4100
(2026-11-13) a PS/2 keyboard was plugged in, the log said
`ps2: keyboard reset failed`, and it did not type. `initialize_keyboard()`
gives up on the keyboard when the reset fails. `keyboard_reset_device()` waits
for the ACK and then the self-test result (`0xAA`) through `data_read()`,
which spins `kIoWaitSpinCount` (100000) status reads: at roughly a microsecond
per port read, about 100 ms. A keyboard may take up to several hundred
milliseconds to finish its self-test after a reset, so a real keyboard can
miss that window where an emulated one and the Compaq's embedded controller
did not. That is the likely cause, not a confirmed one. The fix is a deadline
in time rather than in spins, and not discarding a keyboard whose reset timed
out but which answers afterwards. Until then, a machine with only a PS/2
keyboard cannot be used.

`initialize_mouse()` requires the pad to acknowledge a reset, defaults and the
IntelliMouse sample-rate knock; a pad that does not gets
`ps2: mouse reset failed` and no pointer. The Compaq 21N2F7AR's pad answered;
Synaptics and ALPS pads answer in their own ways, and none has been tried.
Relative three-byte packets are enough for a first pointer; the absolute
modes are a later step.

### HW-9 — Timers on old CPUs

Three separate defects, all invisible in an emulator:

- **AMD C1E stops the local APIC timer** (erratum 400). On K8 from revision F
  and on family 10h, when the firmware enables C1E the timer stops once every
  core has halted, and the tick with it. Symptoms: a machine that only moves
  while something else interrupts it, and `timer-stats:` reporting lost time.
  **Done, not yet run on metal:** on those families, outside a hypervisor,
  every core clears the two "on compare halt" bits of MSR `C001_0055`
  (`SmiOnCmpHalt`, `C1eOnCmpHalt`) at start and again before every idle
  `hlt`, as FreeBSD does, since SMM code may set them back. The boot log says
  `cpu: AMD erratum 400 range, C1E on halt was enabled by the firmware;
  disabled` or `... is off`. If `timer-stats:` still loses time idle, switch
  "C1E support" off in the firmware and record the difference: the remaining
  fix is a tick that does not come from the local APIC.
- **A steady loss on two modern machines** (2026-11-13). The A6-3500 and the
  i5-4440 lost time in almost every `timer-stats:` window, idle included,
  mostly 7%, with a handler of 30 µs and 3 µs. The FX-4100, the Ryzen and
  the Compaq lose nothing idle. It is not erratum 400: family 12h is outside
  it, and Haswell's APIC timer runs through C-states. The two machines do not
  share a wall clock either (PM timer on the A6, TSC on the i5), and the i5's
  APIC timer calibrated to exactly 100 MHz, so neither ruler is obviously
  wrong. Candidates: SMIs from the firmware (USB legacy emulation, for one)
  keeping interrupts off, or the idle loop's `hlt`. To tell them apart, run
  with *no USB* and compare, and read SMI counts where the CPU has them
  (`MSR_SMI_COUNT` on the i5).
- **NetBurst local APIC timer erratum.** The divide-by-16 periodic
  configuration is the one Intel's Pentium 4 errata warn about. The PIT
  backend exists but is chosen only when there is no local APIC.
- **The wall clock falls back to the TSC** whenever the PM timer is 24 bits
  wide. That is right on a CPU with an invariant TSC and wrong on K8, whose
  counter follows the core's clock and drifts between the two cores of an X2,
  so time can step backwards between two reads. The fix is choosing by
  `CPUID.80000007H:EDX[8]`: without it, read the 24-bit PM timer often enough
  to count its wraps, or use the HPET, which the kernel does not use today.

### HW-10 — XD disabled by firmware

Intel firmware has an "Execute Disable Bit" setting. Off, it sets
`IA32_MISC_ENABLE[34]`, CPUID stops reporting NX and the kernel panics on a
CPU that has it.

**Done, not yet run on metal.** On Intel family 0Fh, and family 6 from model
0Dh, every core clears that bit before `enable_nx()` reads CPUID, together
with bit 22 ("Limit CPUID Maxval"), which hides the leaves from 3 up, as
Linux does. The boot log says `cpu: XD was disabled by the firmware;
re-enabled` when it acted. A CPU with no XD at all (a Core Duo, an early
Pentium 4) still panics: that is a missing feature, not a setting.

### HW-11 — Video on firmware framebuffers

Limine maps the framebuffer write-combining through the PAT on every CPU
here, so the old concern about uncached video memory does not apply. What is
left:

- **The mode is chosen once, by Limine, before the kernel starts.** The
  ISO's entries carry no `resolution:` line, so Limine asks the monitor for
  its preferred mode (EDID) and picks another when the firmware does not
  offer it. GOP offers what the monitor reports, so a UEFI machine gets its
  native resolution. A VBE BIOS from this era offers the VESA 4:3 list and
  often none of the widescreen modes its own panel needs. The
  **SavanXP (1024x768)** entry forces `1024x768x32`, the one VESA mode with
  32 bpp a video BIOS of any age can be expected to list, for a machine where
  the preferred mode does not light up. Display Properties still cannot
  change the mode afterwards.
- **`fb_gpu:` does not say what it got.** Its log line prints
  `tipo del firmware` for the cache type of any adapter that is not Bochs'.
  It should print the mapping's real type and the bit depth.
- **32 bpp is a hard gate** in the driver, the console and the splash. A
  firmware with only 24 bpp modes gives a serial console and nothing else.
- **No vertical sync.** Double buffering by panning exists for Bochs' adapter
  only, so the compositor tears on metal.
- **Native mode setting** is per GPU family and large. The target documents
  say which are plausible (Intel GMA, the AtomBIOS Radeons) and which are not
  (Vega, the NVIDIA and VIA chipsets). None gives acceleration: SxGL stays a
  software rasterizer ([SXGL.md](../desktop/SXGL.md)).

### HW-12 — Running from a disk

Setup and its phases are in
[SETUP_AND_UPDATES.md](../userland/SETUP_AND_UPDATES.md). What hardware adds:
the installer can only target a disk the kernel drives (HW-3, HW-4, HW-5), and
a machine with 512 MiB cannot hold the LiveCD's 256 MiB volume and a desktop
at once. A smaller live volume (`SAVANXP_ISO_SECTORS`) is the cheap way to
try such a machine before any of that exists.

### HW-13 — PCI table

`append_device()` returns silently at 64 functions. The development host
uses 41. A larger table, and a line when it overflows, before a machine with
more devices loses one without a trace.

### HW-14 — ACPI on real firmware

`uacpi_namespace_initialize()` is still deferred and every GPE is masked, so
there is no embedded controller, battery, lid, thermal zone or sleep, and the
CPU stays at the frequency the firmware left. Harmless on a desktop; on a
laptop it is the difference between a demonstration and a usable machine.
It also rules out any fix that depends on `_CST` or `_PSS`.

### HW-15 — Other Ethernet controllers

One driver per family, taken on when a target has it: Intel `em(4)` (82566,
82567, 8257x), NVIDIA `nfe(4)`, Broadcom `bge(4)`, Marvell `msk(4)`, VIA
`vr(4)`, SiS `sis(4)`, Atheros `alc(4)`. The FX-4100 has the first of these
seen on a target: an Atheros AR8151 (`1969:1083`), for `alc(4)`. Wireless is out of scope: it needs an
802.11 stack and vendor firmware.

## Order of work

What the packages above buy, most reach first:

1. **HW-1.** A network on the development host, where everything else is
   already tested, and on three of the four other machines run so far.
2. **The PS/2 keyboard (HW-8)** and **the steady tick loss (HW-9).** The
   first leaves a machine without a USB keyboard unusable; the second runs
   every clock that counts ticks 7% slow on two of five machines. The long
   `read` under the global lock is kernel work outside this list, and as
   urgent: it is what is heard on the one machine with speakers.
3. **The rest of HW-9, and HW-10.** Small, and they are the ones that can stop an old
   machine from being usable at all.
4. **HW-6, HW-13, HW-4.** Small widenings of drivers that exist.
5. **HW-3.** Disks on every machine older than the development host.
6. **HW-2.** UHCI and split transactions on metal; EHCI and OHCI already ran.
7. **HW-11**, the mode choice and the log line; native mode setting only
   after a target has been run on its firmware framebuffer and found wanting.
8. **HW-5, HW-12.** Installing.

HW-7, HW-14 and HW-15 wait for a machine that needs them.

## Collecting a machine's facts

Before writing code for a target, from any live Linux on it:

```bash
lscpu                                 # family, model, stepping, flags
lspci -nn                             # every PCI function with its IDs
lspci -nn -k | grep -A2 -E 'IDE|SATA|USB|Audio|Ethernet|VGA'
cat /proc/asound/card*/codec* | head  # HD Audio codec and its pins
dmesg | grep -i -E 'hpet|pm-timer|tsc|c1e|x2apic|efi'
sudo dmidecode -t bios -t baseboard -t memory
```

Then boot the SavanXP ISO with **boot log on screen** and photograph the
lines beginning `cpu:`, `timer:`, `smp:`, `fb_gpu:`, `ahci:`, `ata:`, `xhci:`,
`ps2:`, `azalia:` and `acpi:`. With a serial port, capture COM1 (`0x3F8`,
38400 8N1) instead.

### Reading the log on the machine

When the desktop comes up, **Log Viewer** (System menu,
[LOG_VIEWER.md](../userland/LOG_VIEWER.md)) has the whole boot, with no
cable: the first 512 lines are never overwritten
([SYSTEM_LOG.md](../kernel/SYSTEM_LOG.md)), so it is all still there however
long the machine has run. Its kernel categories group the lines by prefix;
the ones a run is read for:

| Prefix | What it says |
| --- | --- |
| `boot:` | The firmware (`x86 BIOS`, `UEFI 64`) and the loader with its version |
| `cpu:` | Vendor, family, model, stepping and brand string; the features the kernel cares about (`nx`, `x2apic`, `smep`, `invtsc`...); what the XD and C1E quirks did |
| `pci:` | Every function: `bus:slot.function vendor:device [subsystem] class.subclass.prog_if rev irq` -- the `lspci -nn` of the run |
| `timer:`, `timer-stats:` | Which clocks were chosen; lost ticks every two seconds |
| `fb_gpu:`, `boot ready:` | The video mode Limine got; usable memory |
| `ehci:`, `uhci:`, `ohci:`, `xhci:`, `usb:` | Each USB controller started, ports handed to a companion, and every device with its controller, port and speed (`usb: uhci1 port 2 ... low-speed`) |

Problems gathers the lines that read like errors. To take the log away, File >
Save Log... writes it to any volume that is mounted; the LiveCD's own volume
is lost at reboot, so mount a FAT partition from Disk Management first (on a
disk the kernel can drive: AHCI, or a USB stick). Otherwise photograph the
screen. Without a desktop, from any shell on the text console, `logview
--print` prints the same, and `logview --print pci` only one prefix. If the
boot stops before either, the **boot log on screen** entry is what is left.

## Recording a run

A target document gains a **Verified** entry only with: the date, the commit
or ISO build, the firmware settings that matter (boot mode, SATA mode, C1E,
XD), how it was booted, and what was actually exercised. "It booted" is
recorded as that, not as support for the devices the run never touched. The
USB run in [USB_ROADMAP.md](../roadmaps/done/USB_ROADMAP.md) is the model: it
lists what was not seen on the machine next to what was.
