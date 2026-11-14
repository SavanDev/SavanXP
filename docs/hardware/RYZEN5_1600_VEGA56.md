# AMD Ryzen 5 1600 + Radeon RX Vega 56

The development host, and the first machine `master` ran on. The
shared contract and the numbered work packages (HW-n) are in
[REAL_HARDWARE.md](REAL_HARDWARE.md).

## Status

| | |
| --- | --- |
| **Verified**, 2026-11-06 | The ISO written with `dd` to a USB stick boots under UEFI; the desktop runs on the firmware framebuffer; a USB keyboard and mouse drive it through both xHCI controllers ([USB_ROADMAP.md](../roadmaps/done/USB_ROADMAP.md), phase 7) |
| **Verified**, 2026-11-07 | Analog playback through the ALC892 after the Azalia port in `2ac6c50` |
| **Verified**, 2026-11-08 | Sound recognizes the microphone, records it and replays it. Doom and Celeste audio, once the ISO carried the rebuilt ports (`22ae69e`) |
| **Verified**, 2026-11-12 | One boot log captured, v0.4.3 (Build 701) under UEFI: all 12 processors online, the wall clock on the 32-bit PM timer, 1920x1080 from GOP, four SATA disks found and their partition tables read, a USB stick as `usb0` with a partition mounted through Disk Management's elevation prompt. See [The boot log](#the-boot-log-2026-11-12) |
| **Verified**, 2026-11-13 | A second, 35-minute run of the same build under UEFI: Celeste, Nightwood, the media player and `sxgldiag` (241 fps). Audio played throughout, but the **media player had audible micro-cuts** now and then, and nothing else did. See [The second run](#the-second-run-2026-11-13) |
| Not working | Ethernet, Wi-Fi, HDMI and DisplayPort audio, anything of the Vega beyond the framebuffer the firmware set |
| Never exercised on the machine | Writing the SATA disks, USB hubs and hot plug |

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
| PCI | 41 functions on 12 buses | Fits the 256-entry table (HW-13) |
| GPU | Radeon RX Vega 56, `1002:687f`, behind two bridges (`1022:1470`, `1022:1471`) | Firmware (GOP) framebuffer only. See [The Vega 56](#the-vega-56) |
| Displays | DisplayPort at 1920x1080 and HDMI at 1280x1024, on 2026-11-10 | One output, whichever the firmware lights, at the mode Limine picked |
| Analog audio | AMD HDA `1022:1457` at `0b:00.3`; Realtek ALC892 `10ec0892`, subsystem `1849:6893` | Playback and capture verified |
| Display audio | Vega HDA `1002:aaf8` at `09:00.1`; codec `1002:aa01` | Attached as a controller; not an output. Must not displace the analog codec |
| NVMe | Kingston NV1, `2646:500e` | Driver written (HW-5), not yet run here: read-only unless booted with `nvme-rw` |
| SATA | AMD 400-series AHCI `1022:43c8` at `02:00.1`; AMD FCH AHCI `1022:7901` at `0b:00.2` | All four disks are on the 400-series controller (`02:00.1`), read from Linux on 2026-11-12; the FCH controller has none. SavanXP drives the first HBA, up to four disks, so it sees all of them. A fifth disk, or one on the FCH controller, would be invisible (HW-4) |
| USB | AMD 400-series xHCI `1022:43d5`; Zeppelin xHCI `1022:145f` | Both verified with HID devices |
| Ethernet | Realtek RTL8111/8168, `10ec:8168` | No driver (HW-1) |
| Wi-Fi | Realtek RTL8188CE, `10ec:8176` | Out of scope |
| Other | PSP `1022:1456`, SMBus `1022:790b`, LPC `1022:790e` | Unused |

## The boot log, 2026-11-12

Captured with SavanXP v0.4.3 (Build 701) from the ISO on a USB stick.

```
boot: firmware UEFI 64, loader Limine 10.8.5
timer: reloj de pared por PM timer ACPI (0x808, 32 bits)
cpu: APIC local en modo xAPIC (id 0)
smp: 12 cores reportados, 12 en linea (bsp lapic 0, xAPIC)
xhci: 2:0.0 1022:43d5, 22 ports, 32 slots, 32-byte contexts
xhci: a:0.3 1022:145f, 8 ports, 32 slots, 64-byte contexts
ps2: keyboard reset failed
fb_gpu: 1920x1080 nativo, 8100 KiB mapeados (tipo del firmware), mode-setting no disponible
azalia: 9:0.1 no supported analog path, skipped
azalia: ready b:0.3 codec=0x10ec0892 address=0 pin=0x14 dac=0x2 rings=256/256
nic: ningun driver reclamo el hardware
ahci: ahci0 port 0, 976773168 sectors, "WDC WD5000LPCX-22VHAT0"
ahci: ahci1 port 1, 1953525168 sectors, "ST1000LM024 HN-M101MBB"
ahci: ahci2 port 4, 1953525168 sectors, "WDC WD10EZEX-00KUWA0"
ahci: ahci3 port 5, 976773168 sectors, "WDC WD5000LPCX-00VHAT0"
boot ready: 32293 MiB usable, 116 MiB reclaimable, video 1920x1080
```

- **Wall clock:** the PM timer is 32 bits wide, so it is the wall clock and
  the TSC is not.
- **Timer:** `timer-stats:` lost at most 5 ms in any of the 27 two-second
  windows (0%), with the handler at 11 µs on average and 16 µs at most.
- **Audio:** the Vega's HD Audio function enumerates first and is skipped as
  having no analog path; the ALC892 on `1022:1457` is the one driven.
- **USB:** a low-speed HP mouse on the 400-series controller, the stick at
  high speed and a keyboard (`0c45:5204`, which also offers a boot mouse) on
  the Zeppelin controller.
- **PS/2:** `keyboard reset failed` with nothing on the board's PS/2 port, as
  expected.
- **ACPI:** uACPI loaded 9 AML tables without errors. The `_PRT` link devices
  report no IRQ in `_CRS`, and most functions show interrupt line 255; the
  routing comes from the `_PRT` entries that name a GSI directly.
- **Disks:** four disks, all on the 400-series controller and all of the
  machine's SATA disks. That is also the most `kMaxDisks` allows. Their
  partitions were read (`ahci0p1` to `ahci3p3`). No write to them is
  recorded. The `ahci:` line does not name the controller.

## The second run, 2026-11-13

Same build, same boot path, 35 minutes (`system-log-ryzen.txt`, not in the
tree). Devices and timers came up as in the first log. New in it:

- A USB optical drive (`e8d:1836`, "MT1836") was seen and refused:
  `usb-storage: device type 5 not supported`.
- `sxgldiag`: 1205 frames in 5 s, 241 fps, 5289 triangles.
- **The media player's micro-cuts have a cause in the log.** The windows
  where `timer-stats:` lost time (124 of 729) are the ones in which the
  player was running, 64–147 s and 867–1009 s, and in them `smp-stats:`
  names POSIX syscall #0, `read`, as the longest holder of the global lock:
  about 10–19 ms per call, around 190 ms of every two seconds. The player's
  `audiod` port counted up to 197838 underruns. Celeste and Nightwood, run
  later with sound, lost nothing. The same holds on the other
  machines that ran the player (see
  [REAL_HARDWARE.md](REAL_HARDWARE.md#long-reads-under-the-global-lock)).
- At 31–33 s a single `read` held the lock for **978 ms**: every core waited,
  the tick lost 979 ms, and the HD Audio stream was dropped
  (`azalia: DMA polling deadline missed; stream stopped`). It recovered: the
  later streams played and stopped with `drops=0 underruns=0`.

## Work for this target

In the order that makes the machine more useful soonest:

1. **Ethernet (HW-1).** The only wired NIC, and the reason Doom and Celeste
   had to stop depending on a network card. Until it lands nothing that uses
   the network can be checked here.
2. **NVMe (HW-5)**, then setup (HW-12): the path to an installed system on
   this machine's main disk. The driver is written; what is owed is a boot
   that reads the NV1 read-only, then a write test with `nvme-rw`
   ([NVME.md](../kernel/NVME.md)).
3. **The physical audio checks** still owed to the follow-up software, below.

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
  EDID prefers from GOP. Confirmed on 2026-11-12: `fb_gpu: 1920x1080 nativo`.
- **Report what was received.** A `fb_gpu:` line with the mode, depth and the
  mapping's real cache type, and Display Properties showing a fixed mode as
  fixed.
- **Measure before optimizing.** The compositor's numbers in
  [GRAPHICS_PERF.md](../desktop/GRAPHICS_PERF.md) are from virtual machines.
  The 2026-11-12 run is the first on metal: up to 87 fps at 1920x1080, with
  a frame typically composed in under 1 ms.

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

- Whether the board has a serial header. With a bracket on one the kernel
  log is on COM1 and no photograph is needed.
