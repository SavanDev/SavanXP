# AMD Ryzen 5 1600 + Radeon RX Vega 56

The development host, and the only machine `master` has been run on. The
shared contract and the numbered work packages (HW-n) are in
[REAL_HARDWARE.md](REAL_HARDWARE.md).

## Status

| | |
| --- | --- |
| **Verified**, 2026-11-06 | The ISO written with `dd` to a USB stick boots under UEFI; the desktop runs on the firmware framebuffer; a USB keyboard and mouse drive it through both xHCI controllers ([USB_ROADMAP.md](../roadmaps/done/USB_ROADMAP.md), phase 7) |
| **Verified**, 2026-11-07 | Analog playback through the ALC892 after the Azalia port in `2ac6c50` |
| **Verified**, 2026-11-08 | Sound recognizes the microphone, records it and replays it. Doom and Celeste audio, once the ISO carried the rebuilt ports (`22ae69e`) |
| Not working | Ethernet, NVMe, Wi-Fi, the disks on the second AHCI controller, HDMI and DisplayPort audio, anything of the Vega beyond the framebuffer the firmware set |
| Never exercised on the machine | Reading or writing the SATA disks, USB hubs and hot plug, a USB stick as `usb0`, the boot log itself (no run captured it) |

The tested application, jack, input pin, gain and firmware settings of the
audio runs were not recorded.

## Inventory

Read from Linux on the host on 2026-11-07 and again on 2026-11-10 (`lscpu`,
`lspci -nn`, DMI, `/proc/asound`). These are observations of the hardware;
the right-hand column is what SavanXP does with each device.

| Component | Observed | SavanXP |
| --- | --- | --- |
| Board, firmware | ASRock B450M Pro4-F R2.0, UEFI P7.40 (2022-10-26) | Boots through Limine's `BOOTX64.EFI` |
| CPU | Ryzen 5 1600 (family 17h, Zen), 6 cores / 12 threads. NX, SMEP, SMAP, RDRAND, invariant TSC, 1 GiB pages. **No x2APIC** | 12 of `kMaxCpus` 32. The local APIC runs in xAPIC mode over MMIO, the path that was first written for VirtualBox. SMEP on; SMAP unused |
| Memory | 32 GiB (about 31 GiB usable) | Devices must cope with DMA above 4 GiB; AHCI and Azalia check the controller's 64-bit capability |
| PCI | 41 functions on 12 buses | Fits the 64-entry table (HW-13) |
| GPU | Radeon RX Vega 56, `1002:687f`, behind two bridges (`1022:1470`, `1022:1471`) | Firmware (GOP) framebuffer only. See [The Vega 56](#the-vega-56) |
| Displays | DisplayPort at 1920x1080 and HDMI at 1280x1024, on 2026-11-10 | One output, whichever the firmware lights, at the mode Limine picked |
| Analog audio | AMD HDA `1022:1457` at `0b:00.3`; Realtek ALC892 `10ec0892`, subsystem `1849:6893` | Playback and capture verified |
| Display audio | Vega HDA `1002:aaf8` at `09:00.1`; codec `1002:aa01` | Attached as a controller; not an output. Must not displace the analog codec |
| NVMe | Kingston NV1, `2646:500e` | No driver (HW-5) |
| SATA | AMD 400-series AHCI `1022:43c8` at `02:00.1`; AMD FCH AHCI `1022:7901` at `0b:00.2` | The first one found is driven, up to four disks; the other is ignored (HW-4) |
| USB | AMD 400-series xHCI `1022:43d5`; Zeppelin xHCI `1022:145f` | Both verified with HID devices |
| Ethernet | Realtek RTL8111/8168, `10ec:8168` | No driver (HW-1) |
| Wi-Fi | Realtek RTL8188CE, `10ec:8176` | Out of scope |
| Other | PSP `1022:1456`, SMBus `1022:790b`, LPC `1022:790e` | Unused |

## Work for this target

In the order that makes the machine more useful soonest:

1. **Ethernet (HW-1).** The only wired NIC, and the reason Doom and Celeste
   had to stop depending on a network card. Until it lands nothing that uses
   the network can be checked here.
2. **Capture one boot log.** No run recorded the `cpu:`, `timer:`, `smp:`,
   `fb_gpu:` and `xhci:` lines, so this document cannot say which wall clock
   the machine uses, whether twelve processors came online or which mode the
   firmware gave. The **boot log on screen** entry is enough.
3. **Both AHCI controllers and their disks (HW-4).** The kernel sees disks on
   one of them and has never read a sector there. Reading is safe; note that
   a disk holding an SxFS volume becomes the root in place of the live system.
4. **The display mode (HW-11).** The ISO's default entry now takes the
   monitor's preferred mode; confirm it comes up at 1920x1080. See below.
5. **NVMe (HW-5)**, then setup (HW-12): the path to an installed system on
   this machine's main disk.
6. **The physical audio checks** still owed to the follow-up software, below.

Not planned: the native Vega driver, HDMI and DisplayPort audio, Wi-Fi.

## The Vega 56

### What it gives today

The firmware's GOP driver sets one mode on one output, Limine hands its
framebuffer over mapped write-combining, and `fb_gpu` presents into it. That
is enough for the desktop, and it is everything there is:

- one resolution for the whole session, chosen before the kernel starts;
  Display Properties cannot change it, because `fb_gpu` offers mode setting
  only on Bochs' adapter;
- one monitor, whichever the firmware initialized;
- no vertical sync and no page flip, so the compositor tears;
- no hardware cursor, no blanking, no display power management;
- every pixel drawn by the CPU. SxGL is a software rasterizer on every
  machine; the hardware backend in [VIRGL_ROADMAP.md](../roadmaps/open/VIRGL_ROADMAP.md)
  is for virtual machines.

### Why a native driver is not planned

Vega 10 is driven by Linux's `amdgpu`, and nothing smaller exists to learn
from. Lighting a display natively means AMD's Display Core, the largest part
of that driver; anything past mode setting means loading signed firmware for
the card's security processor, power controller and command processors, then
a memory manager and a command submission path. OpenBSD and FreeBSD do not
have their own driver either: they carry the Linux one whole, behind a
compatibility layer for the Linux kernel's interfaces.

A port would therefore be a Linux kernel-interface layer first and a GPU
driver second: XL, with a second project hidden inside it. Nothing in the
desktop needs it today.

### What is worth doing instead (HW-11)

- **Let the mode follow the monitor.** Done in the ISO: its entries no longer
  carry a fixed `resolution:` line, so Limine takes the mode the monitor's
  EDID prefers from GOP, which should put the desktop at 1920x1080 here with
  no driver at all. Not yet confirmed on this machine.
- **Report what was received.** A `fb_gpu:` line with the mode, depth and the
  mapping's real cache type, and Display Properties showing a fixed mode as
  fixed.
- **Measure before optimizing.** The compositor's numbers in
  [GRAPHICS_PERF.md](../desktop/GRAPHICS_PERF.md) are from virtual machines. A
  1920x1080 software desktop on this CPU has not been measured.

### Display audio

The Vega's HDA function is a second HD Audio controller with an HDMI codec
behind it, and it enumerates **before** the analog one. Sound over HDMI or
DisplayPort needs the GPU side to enable audio on the active output and
publish the monitor's capabilities to the codec, which is the native driver's
job. It is blocked on the same decision, and stays a separate objective from
the analog work ([AZALIA_ROADMAP.md](../roadmaps/open/AZALIA_ROADMAP.md)).

## Audio on this host

The requested OpenBSD driver is `sys/dev/pci/azalia.c` with its codec support
and HDA definitions, not `ac97.c`. AC'97 remains useful for existing virtual
machines and the Compaq target, but it cannot operate either HDA controller.
The GPU controller precedes the analog controller in PCI enumeration, so
binding the first HDA device is not a valid selection policy.

The ALC892 dump identifies the rear green line-out as pin `0x14`, fed by mixer
`0x0c` and DAC `0x02`; the front headphone pin is `0x1b`. The rear pin has EAPD
and a mute-only output amplifier; mixer `0x0c` has mute-only input amplifiers.
They must be configured even though master volume is applied in software.
Unused pins marked disconnected by firmware must be excluded. These node
numbers describe this codec, not constants to hardcode into a generic driver.

The port and acceptance gates are in
[AZALIA_ROADMAP.md](../roadmaps/open/AZALIA_ROADMAP.md), and the driver in
[AZALIA.md](../kernel/AZALIA.md). QEMU verifies the HDA transport and recorded
PCM; physical playback was confirmed separately by the user. HDMI belongs to a
separate future objective. The follow-up analog software includes routing,
optional jack switching, capture, independent devices, preferences and
controller recovery; these need the physical checks below.

### Follow-up audio checks after the software objective

Boot the final ISO and record the image/commit, firmware audio settings,
connector and application used. The earlier playback confirmation covers only
the initial port, not these follow-up capabilities.

1. Open **Sound settings...** from the speaker popup. Check rear green line-out
   and front headphones separately, then optional plug/unplug switching. Unknown
   readings must preserve the route; a failed route must not be shown as selected.
2. Test rear/front microphone and line-in with the three-second record/replay
   controls. Verify independent input gain/mute and simultaneous playback.
3. Change application levels, output/input defaults and policy; restart the audio
   service and reopen clients to check restoration. Check that service/device
   errors are reported and capture reopens explicitly after controller recovery.
4. Confirm external applications and their data remain installed. Power-cycle
   preference persistence requires a writable persistent root; an ISO boot's RAM
   disk can test service restarts but does not retain guest writes after reboot.

Keep Vega HDMI outside this run. Report hardware results separately from the
QEMU PCM, visual and simulated-fault evidence in the roadmap.

## Facts still to collect

- The boot log, once (item 2 above): wall clock source, processors online,
  framebuffer mode, the `azalia:` controller order.
- Whether the firmware's PM timer is 32 bits wide, which decides if the wall
  clock is the PM timer or the TSC ([TIME.md](../kernel/TIME.md)). Either is
  sound on this CPU.
- Whether the board has a serial header. With a bracket on one the kernel
  log is on COM1 and no photograph is needed.
