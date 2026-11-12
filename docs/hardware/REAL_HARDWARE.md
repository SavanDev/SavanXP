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

| Target | Document | Branch | Status on 2026-11-12 |
| --- | --- | --- | --- |
| AMD Ryzen 5 1600 + Radeon RX Vega 56 (ASRock B450M Pro4-F R2.0) | [RYZEN5_1600_VEGA56.md](RYZEN5_1600_VEGA56.md) | `master` | **Verified:** boots from USB under UEFI, desktop on the firmware framebuffer, USB keyboard and mouse, analog audio in and out. No Ethernet, NVMe or native Vega driver |
| Intel Pentium 4, Compaq Evo N1020v | [PENTIUM4_EVO_N1020V.md](PENTIUM4_EVO_N1020V.md) | `legacy` (i386) | Cannot run `master`: no long mode, no NX. The i386 port on `legacy` boots, mounts SxFS and runs one ring 3 program in QEMU |
| AMD Athlon II + chipset graphics | [ATHLON_II.md](ATHLON_II.md) | `master` | Not run. **Expected** to reach the desktop with PS/2 input; no USB, no network |
| Intel Core 2 Duo + chipset graphics | [CORE2_DUO.md](CORE2_DUO.md) | `master` | Not run. **Expected** to reach the desktop with PS/2 input; no USB, no network |
| AMD Athlon 64 + chipset graphics | [ATHLON_64.md](ATHLON_64.md) | `master` | Not run. The oldest CPU `master` can boot; storage, USB and the wall clock all need work first |

Only the first two machines have been inventoried. The other three are known
by CPU family, which decides what the kernel may assume about the processor
but not which chipset, disk controller, NIC or audio codec is on the board.
Each of those documents ends with the facts to collect from the unit.

## What `master` requires of a machine

Read from the tree on 2026-11-12. A machine that fails a **hard** row does not
boot; a machine that fails a soft one boots with something missing.

| Area | Requirement | Where |
| --- | --- | --- |
| CPU, hard | Long mode. Limine refuses a CPU without it before the kernel is loaded | [Limine protocol](https://github.com/limine-bootloader/limine-protocol/blob/trunk/PROTOCOL.md): *"Only 64-bit, Little Endian machines are supported"* |
| CPU, hard | NX. The BSP panics with `cpu: NX/W^X is required`; an AP without it halts. On Intel, XD switched off in the firmware is switched back on first (HW-10) | `enable_nx()` in `arch/x86_64/cpu_init.cpp`, [KERNEL_SECURITY.md](../kernel/KERNEL_SECURITY.md) |
| CPU, soft | Local APIC; xAPIC over MMIO or x2APIC over MSRs. Without one the PIT drives a single core | `initialize_local_apic()`, `arch/x86_64/timer.cpp` |
| CPU, soft | SMEP, enabled when present (`cpu: SMEP unavailable on this processor` otherwise). `RDRAND`, with a TSC-mixed fallback. SMAP is not used | `enable_smep()`, [KERNEL_SECURITY.md](../kernel/KERNEL_SECURITY.md) |
| CPU, limit | 32 logical processors (`arch::kMaxCpus`) | `include/arch/x86_64/cpu.hpp` |
| Instruction set | Plain `x86_64`: SSE2 and nothing later. No `-march`, no intrinsics, no hand-written assembly beyond the baseline; FFmpeg is built with `--disable-asm`. Syscalls are `int $0x80`, so nothing depends on `SYSCALL`/`SYSRET` | `CMakeLists.txt`, `tools/build-user.sh`, `ports/*/` |
| Firmware | Legacy BIOS with INT 13h extensions, or 64-bit UEFI. Limine 10.8.5; the ISO is hybrid (El Torito for both, and `dd`-able to a USB stick). The 32-bit UEFI loader is not staged | `boot/limine.conf`, `CMakeLists.txt` (`savanxp_iso`) |
| ACPI | An RSDP from the loader and a FADT. A MADT moves interrupts to the IOAPIC with its source overrides applied; without one the legacy PIC is used. uACPI resolves `_PRT` | `kernel/acpi.cpp`, `kernel/ioapic.cpp`, `kernel/uacpi_glue.cpp` |
| Clocks | A CMOS RTC: the LAPIC timer is calibrated across two RTC second edges. PIT channel 2: the TSC is calibrated against it. The wall clock is the ACPI PM timer when it is 32 bits wide and the **TSC otherwise** | [TIME.md](../kernel/TIME.md) |
| Video | A linear 32 bpp framebuffer from the firmware (VBE or GOP). The ISO's boot entries ask Limine for `1024x768x32`, a VESA mode every video BIOS offers; the development configuration keeps `1280x800x32`, which the QEMU scenarios click against. No mode setting after boot outside QEMU's adapters; all drawing is software | `kernel/fb_gpu.cpp`, `kernel/console.cpp` |
| Memory | The LiveCD root is a 256 MiB SxFS volume Limine loads into RAM as a module, next to the kernel and the initramfs. The floor has not been measured; 1 GiB is the least worth trying | `SAVANXP_ISO_SECTORS` in `CMakeLists.txt` |
| Storage | AHCI: the first HBA only, four disks. ATA: the two legacy channels at `0x1F0`/`0x170`, PIO, 28-bit LBA commands. USB mass storage behind xHCI. No NVMe, no ATAPI | `kernel/ahci.cpp`, `kernel/ata.cpp`, `kernel/usb/umass.cpp` |
| USB | xHCI only. A machine whose ports hang off EHCI, UHCI or OHCI has no USB | [USB_ROADMAP.md](../roadmaps/done/USB_ROADMAP.md) |
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
| HW-2 | USB before xHCI: EHCI, then UHCI and OHCI | L | — | **needed** | **needed** | **needed** | **needed** |
| HW-3 | PCI IDE: native-mode BARs, bus-master DMA, 48-bit LBA | M | — | unless AHCI is selected | **needed** on ICH7 | **needed** | **needed** |
| HW-4 | AHCI: every HBA, more than four disks, controller quirks | S | **needed** | useful | useful | on SB600 | — |
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

This is the single package with the most reach: it is what stands between the
development host and everything that needs a network on metal (`ntpsync`, Web
Viewer, the SUN sockets).

Until it exists, a PCI RTL8139 card is the one way to get a network on any
desktop with a PCI slot. The driver has only ever run in emulators as far as
this tree records, so that card is also its first test on metal.

### HW-2 — USB before xHCI

Every machine before about 2012 routes its ports to an EHCI controller for
high speed and to UHCI (Intel, VIA) or OHCI (AMD, ATI, NVIDIA, ALi, SiS)
companions for low and full speed. With xHCI alone such a machine has no USB
keyboard, mouse or stick once the kernel is running.

`kernel/usb/` was built for this: the core reaches controllers through an
interface of its own, so a second controller driver does not touch HID, hubs
or mass storage ([USB_ROADMAP.md](../roadmaps/done/USB_ROADMAP.md), "Where the
code lives"). Order that pays soonest:

1. **EHCI.** Sticks and anything behind a USB 2.0 hub, on every old machine
   regardless of vendor. Needs split transactions for low and full speed
   devices behind a hub's transaction translator.
2. **UHCI** or **OHCI**, whichever the machine at hand has: keyboards and mice
   plugged straight into a root port.

Reference: OpenBSD `ehci(4)`, `uhci(4)`, `ohci(4)`, and the BIOS handoff in
`ehci_pci.c`. All three are polled, like the xHCI driver.

Two workarounds exist today, neither verified. A legacy BIOS with "USB legacy
support" emulates a PS/2 keyboard from a USB one for as long as no driver
takes the controllers, and the kernel never touches them; whether the
emulation survives `ps2.cpp`'s controller reset and self-test is a property of
each firmware. And on a desktop with a PCIe slot, an add-in xHCI card gives
working USB ports with the driver that exists; only AMD's own controllers have
been run on metal.

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

### HW-4 — AHCI coverage

`find_hba()` keeps the first AHCI function and `kMaxDisks` is 4. The
development host has two HBAs, so the disks on the second are invisible.
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

Codecs are a separate question. The graph walk is generic, the quirk table is
not compiled, and only the ALC892 has been heard. Each new codec (ALC66x/88x,
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

### HW-8 — PS/2 pointers

`initialize_mouse()` requires the pad to acknowledge a reset, defaults and the
IntelliMouse sample-rate knock; a pad that does not gets
`ps2: mouse reset failed` and no pointer. Laptop pads from Synaptics and ALPS
answer in their own ways, and none has been tried. Relative three-byte
packets are enough for a first pointer; the absolute modes are a later step.

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

- **The mode is chosen once, by a line in `boot/limine.conf`.** Every entry
  asks for `1024x768x32`, the one VESA mode with 32 bpp a video BIOS of any
  age can be expected to list; when the firmware has no such mode Limine
  picks another. A VBE BIOS from this era offers the VESA 4:3 list and often none of
  the widescreen modes its own panel needs; GOP offers what the monitor
  reports. Wanted: boot entries or an option for the mode, and the native
  resolution by default.
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
`vr(4)`, SiS `sis(4)`, Atheros `alc(4)`. Wireless is out of scope: it needs an
802.11 stack and vendor firmware.

## Order of work

What the packages above buy, most reach first:

1. **HW-1.** A network on the development host, where everything else is
   already tested.
2. **HW-9 and HW-10.** Small, and they are the ones that can stop an old
   machine from being usable at all.
3. **HW-6, HW-13, HW-4.** Small widenings of drivers that exist.
4. **HW-3.** Disks on every machine older than the development host.
5. **HW-2.** USB on those same machines.
6. **HW-11**, the mode choice and the log line; native mode setting only
   after a target has been run on its firmware framebuffer and found wanting.
7. **HW-5, HW-12.** Installing.

HW-7, HW-8, HW-14 and HW-15 wait for a machine that needs them.

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

Problems gathers the lines that read like errors. To take the log away, File >
Save Log... writes it to any volume that is mounted; the LiveCD's own volume
is lost at reboot, so mount a FAT partition from Disk Management first (on a
disk the kernel can drive: AHCI, or USB behind xHCI). Otherwise photograph the
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
