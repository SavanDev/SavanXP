# AMD FX-4100 (ASRock, AMD 760G + SB7x0)

A Bulldozer processor on an ASRock board with the AMD 760G north bridge and
its Radeon 3000 graphics, and an SB7x0 south bridge. The board is the class
[ATHLON_II.md](ATHLON_II.md) describes from documentation; this run is the
first time one was seen. It runs `master`. The shared contract and the
numbered work packages (HW-n) are in [REAL_HARDWARE.md](REAL_HARDWARE.md).

## Status

| When | What |
| --- | --- |
| **Verified**, 2026-11-13 | SavanXP v0.4.3 (Build 701) booted from a USB stick through the firmware's **legacy BIOS path** and ran for about 6 minutes. Desktop at 1280x1024; a **low-speed** USB mouse through EHCI's OHCI companion; the stick read and a partition mounted through Disk Management; a SATA disk found by the **legacy-port ATA driver**, its partition table read; Doom and `sxgldiag` ran |
| Not working | The **PS/2 keyboard**: `ps2: keyboard reset failed`, and it did not type (HW-8). Ethernet: an Atheros controller with no driver (HW-15) |
| Not run | Audio output: the machine had no speakers, so the ALC662 attached but nothing was heard. Writing the disk |

The log is the only record of the run; no Linux inventory was taken.

## The unit

Read from the boot log (`system-log-fx-4100.txt`, not in the tree).

| Item | Seen on the unit |
| --- | --- |
| Processor | `AuthenticAMD family 0x15 model 0x1 stepping 2`, FX-4100, 4 cores. The BSP's local APIC ID is **16**, not 0 |
| Memory | 3519 MiB usable |
| Board, firmware | ASRock (PCI subsystem `1849`). FADT revision 1, 132 bytes (`assuming version 2`) |
| North bridge | AMD 760G (`1022:9600`), Radeon 3000 (`1002:9616`) |
| South bridge | SB7x0: SATA `1002:4391` in **AHCI** mode, PATA `1002:439c`, SMBus `1002:4385` rev 3c, HD Audio `1002:4383` |
| Disk | `SAMSUNG HD161GJ`, a 160 GB SATA disk |
| USB | Two EHCI (`1002:4396`) and five OHCI (`1002:4397`, `4398`, `4399`) |
| Audio | Realtek ALC662 (`0x10ec0662`) on the SB7x0 controller |
| Ethernet | Atheros AR8151 v2 (`1969:1083`) |
| Wi-Fi | Atheros AR9227 PCI (`168c:002d`). Out of scope |

## What the log shows

### Processor and timers

```
cpu: features lm nx sse2 sse3 ssse3 sse4.1 sse4.2 sse4a cx16 lahf_lm popcnt avx apic invtsc pdpe1gb
cpu: SMEP unavailable on this processor
timer: reloj de pared por PM timer ACPI (0x808, 32 bits)
cpu: APIC local en modo xAPIC (id 16)
timer: local APIC calibrated divisor=12525 target_hz=1000
smp: 4 cores reportados, 4 en linea (bsp lapic 16, xAPIC)
```

- Every hard requirement is met. No SMEP, SMAP or `rdrand`.
- **APIC IDs starting at 16** worked: all four cores came up, IPIs and the
  lazy TLB check passed, and the SCI was routed to `lapic 17`. The kernel
  copes with a BSP that is not APIC 0.
- Family 15h is outside the erratum 400 range; no `C1E` line.
- `timer-stats:` was at 0% while idle (90 of 161 windows) and lost up to 26%
  under load: during `sxgldiag` and from the media player and Doom onwards.
  Unlike the A6-3500 and the i5-4440, the loss follows the work, not the
  clock.

### Storage

```
ata: ata0 268435455 sectors, "SAMSUNG HD161GJ" serial "S225J50Z504774"
block: 10 device(s) ata0(rw) livecd(rw) usb0(rw) ata0p1(rw) ata0p2(rw) ata0p3(rw) ...
```

The first disk `ata.cpp` has found on metal. The SATA function is in AHCI
mode, yet the AHCI driver logged nothing and the disk answered on the legacy
primary channel: the SB7x0 can hand its last two SATA ports to the IDE
controller in compatibility mode ("SATA IDE combined mode"), which is the
likely explanation. Either way:

- **the 28-bit limit is real**: 268435455 sectors is `0x0FFFFFFF`, 128 GiB of
  a 160 GB disk. The partition table was read; anything past that point is
  out of reach until HW-3 adds 48-bit commands;
- the ATHLON_II advice, "set SATA to AHCI", does not on its own bring a disk
  onto the AHCI driver on these boards: which port the disk is on matters too.

### Devices

| Device | Log | Result |
| --- | --- | --- |
| Display | `fb_gpu: 1280x1024 nativo, 5120 KiB mapeados (tipo del firmware)` | **Verified** at 1280x1024, the mode Limine took from EDID. The desktop's saved 1024x768 was refused, as expected |
| USB | `ehci: 0:12.2 1002:4396, 6 ports, 2 companions`, `ohci:` ×5 | **Verified.** `ehci: port 1: low-speed device, handed to the companion`, then `usb-hid: mouse on 458:3a` on OHCI. The stick at high speed. An SD reader with no card answered nothing |
| PS/2 keyboard | `ps2: keyboard reset failed` | **Failed.** A PS/2 keyboard was connected and did not type. See HW-8 |
| Audio | `azalia: ready 0:14.2 codec=0x10ec0662 address=0 pin=0x14 dac=0x2` | The SB7x0 controller attached **without** its snooping quirk, capture found (`pin=0x18 adc=0x8`). **Not heard**: no speakers, so HW-6's question for this controller is still open |
| Ethernet | `nic: ningun driver reclamo el hardware` | Atheros AR8151; OpenBSD `alc(4)` (HW-15) |

### Performance

At 1280x1024, all drawing in software:

| Workload | Result |
| --- | --- |
| Compositor | Median 61 fps across the run, up to 406 fps with small damage |
| Doom (`doomgeneric`) | Median 36 fps, 71 at most, in 204 reports |
| `sxgldiag` software backend | 535 frames in 5 s, 107 fps, 2332 triangles |
| `audiod` | `dropped=0 mismatch=0` with two streams |

## Work for this target

1. **The PS/2 keyboard** (HW-8). Without it a machine that has no USB keyboard
   cannot be used at all.
2. **HW-3**: 48-bit LBA for this disk, and native-mode channels for boards
   whose SATA is set to IDE.
3. **HW-6**: speakers on the jack, to learn whether the SB7x0 plays without
   its snooping quirk.
4. **HW-15**: `alc(4)` for the AR8151.

## Sources

- The boot log of the run on 2026-11-13 (`system-log-fx-4100.txt`, not in the
  tree).
