# Compaq Presario 21N2F7AR (Core i7-6500U)

A 14-inch laptop sold in Argentina from 2016 under the Compaq brand: a
Skylake-U processor with HD Graphics 520, on a board that the whole 21N line
shares. It runs `master`. The shared contract and the numbered work packages
(HW-n) are in [REAL_HARDWARE.md](REAL_HARDWARE.md).

## Status

| When | What |
| --- | --- |
| **Verified**, 2026-11-12 | SavanXP v0.4.3 (Build 701) booted from a USB stick through the firmware's **legacy BIOS path**, chosen on purpose to test it, and ran for about 30 minutes. Desktop at the panel's native 1920x1080; built-in keyboard and touchpad over PS/2; a USB mouse; the internal SATA disk and the stick; analog audio through the internal speakers and through speakers on the headphone jack; the internal microphone; running on battery with the charger unplugged; Doom, Nightwood and `sxgldiag` all ran. No Ethernet driver; Wi-Fi, Bluetooth and the webcam have none either |
| Not run | The UEFI path, which the firmware also offers; Secure Boot; HDMI, suspend, the lid, and reading the battery |

What follows is the run's boot log on that date, plus the model's retail
specification where the log says nothing. One unit has been seen. It differs
from the retail listings in its Wi-Fi card and its disk, so other units of
the same model may differ too.

## The unit

| Item | Retail listing | Seen on the unit |
| --- | --- | --- |
| Processor | Core i7-6500U | `GenuineIntel family 0x6 model 0x4e stepping 3`, i7-6500U @ 2.50GHz, 4 logical CPUs |
| Memory | 4 GB DDR3L, soldered, no slot | 3706 MiB usable |
| Disk | 1 TB SATA hard disk (one listing: 500 GB) | `FUJITSU MHW2120BH`, a 120 GB disk, with four partitions |
| Display | 14" Full HD | 1920x1080 from the firmware |
| Ethernet | — | Realtek RTL8111/8168, `10ec:8168` rev 07 |
| Wi-Fi, Bluetooth | Wi-Fi b/g/n, Bluetooth 4.0 | Intel Dual Band Wireless-AC 3160 (`8086:08b3`), with Bluetooth on USB (`8087:07dc`) |
| Webcam | 1.0 MP | Alcor Micro "HD Webcam" on USB (`058f:3822`) |
| Audio codec | — | Realtek ALC269 (`0x10ec0269`) |
| Firmware | Windows 10 era | Insyde, OEM ID `SKL-ULT`. Boots both UEFI and legacy BIOS |
| Board | `K21_VC 1` (parts listings) | PCI subsystem vendor `1bab` on every PCH function |

The disk is not the one the listings name, and the Wi-Fi is Intel rather than
the Realtek that driver catalogues give for the "Presario 21" family. Either
the unit was serviced or the model shipped with more than one configuration.

## What the log shows

### Boot and processor

```
boot: firmware x86 BIOS, loader Limine 10.8.5
cpu: features lm nx sse2 sse3 ssse3 sse4.1 sse4.2 cx16 lahf_lm popcnt avx apic x2apic rdrand smep smap invtsc pdpe1gb
timer: PM timer ACPI de 24 bits (0x1808), el reloj de pared sigue en el TSC
cpu: APIC local en modo x2APIC (id 0)
timer: local APIC calibrated divisor=1500 target_hz=1000
smp: 4 cores reportados, 4 en linea (bsp lapic 0, xAPIC)
```

- Every hard and soft CPU requirement is met.
- **The PIT is not gated.** Sunrise Point can stop the 8254's clock, and the
  kernel calibrates the TSC against PIT channel 2; here the TSC and the local
  APIC timer calibrated normally. The calibration takes about 1.4 s of the
  boot, the expected two RTC edges.
- The wall clock is the TSC, as expected: the PM timer is 24 bits wide and
  the TSC is invariant.
- `timer-stats:` reported 0% lost time in all 439 windows of the run, with a
  timer handler of at most 28 µs.
- The two APIC lines disagree. `cpu:` reads the APIC base MSR and says
  x2APIC; `smp:` prints Limine's response flag and says xAPIC. On this
  legacy boot the APIC was in x2APIC mode without Limine reporting it. The
  kernel works either way, but the `smp:` line should report the mode the
  processor is in.

### ACPI

uACPI loaded the namespace from 12 AML tables (Insyde `SKL-ULT`) with no
errors, in 368 ms, and resolved `_PRT` with 33 entries. The FADT is 244 bytes
for revision 5, which uACPI treats as version 3 (`FADT length 244 doesn't
match expected for revision 5, assuming version 3`); nothing failed because
of it. The SCI is on GSI 9 with the power button enabled.

### PCI

```
00:00.0 8086:1904  host bridge
00:02.0 8086:1916  HD Graphics 520
00:08.0 8086:1911  Gaussian mixture model
00:14.0 8086:9d2f  xHCI                      0c.03.30
00:14.2 8086:9d31  thermal subsystem
00:16.0 8086:9d3a  ME interface
00:17.0 8086:9d03  SATA, AHCI mode           01.06.01
00:1c.0 8086:9d13  PCIe root port -> 01:00.0
00:1c.4 8086:9d14  PCIe root port -> 02:00.0
00:1e.0 8086:9d27  LPSS UART
00:1e.4 8086:9d2b  eMMC/SD host              08.05.01
00:1f.0 8086:9d48  LPC
00:1f.2 8086:9d21  PMC
00:1f.3 8086:9d70  HD Audio                  04.03.00
00:1f.4 8086:9d23  SMBus
01:00.0 10ec:8168  RTL8111/8168 Ethernet
02:00.0 8086:08b3  Wireless-AC 3160
```

17 functions. There is no LPSS I2C controller, which settles the touchpad: it
is PS/2.

### Devices

| Device | Log | Result |
| --- | --- | --- |
| Display | `fb_gpu: 1920x1080 nativo, 8100 KiB mapeados (tipo del firmware), mode-setting no disponible` | **Verified.** The VBE BIOS offered the panel's native mode |
| SATA | `ahci: ahci0 port 2, 468860015 sectors, "FUJITSU MHW2120BH"` | **Verified.** The firmware was in AHCI mode. The four partitions were found |
| USB | `xhci: 0:14.0 8086:9d2f, 18 ports, 32 slots, 32-byte contexts` | **Verified.** A super-speed stick (`usb-storage`), a **low-speed** mouse (`usb-hid: mouse on 275d:ba6`), and the internal Bluetooth and webcam, which have no drivers |
| Keyboard | — | **Verified** by use: the built-in keyboard through the 8042 |
| Touchpad | `ps2: mouse without wheel (3-byte packets)` | **Verified.** It answered the standard reset and works as a relative pointer. HW-8 is not needed here |
| Audio | `azalia: ready 0:1f.3 codec=0x10ec0269 address=0 pin=0x14 dac=0x2` | **Verified**: the internal speakers, speakers on the headphone jack, and capture from the internal microphone (`azalia: capture pin=0x12 adc=0x9`). Plugging into the jack did not move the sound; choosing the jack in Sound did, and only one output played at a time. The function is class `04/03`, so the AC'97 driver did not bind it. One `azalia: late writer` warning in 30 minutes |
| Ethernet | `nic: ningun driver reclamo el hardware` | No network (HW-1) |
| Wi-Fi | — | Out of scope |

### Power

Unplugging the charger while the system ran changed nothing visible. That is
what `master` should do today: every GPE is masked, so the AC and battery
notifications the embedded controller raises never reach the kernel, and the
firmware handles the switch on its own. Nothing shows the battery level or
the AC state yet (HW-14).

Two ordinary lines are worth knowing about. The desktop profile on the stick
had a saved resolution of 1024x768 from another machine, and `windowd:
display mode 1024x768 refused` is the expected answer on a firmware
framebuffer; the desktop kept 1920x1080. `audiod: UDP unavailable` follows
from having no NIC.

### Performance

At 1920x1080 on the firmware framebuffer, all drawing in software:

| Workload | Result |
| --- | --- |
| Compositor | Up to 93 fps with small damage; composing typically under 1 ms. The worst window composed for 42 ms on average (20 fps) |
| Doom (`doomgeneric`) | 16 to 37 fps in the 18 reports |
| `sxgldiag` software backend | 827 frames in 5 s, 165 fps, 3507 triangles |
| `audiod` (Nightwood) | `streams=1 dropped=0 mismatch=0`, `lost=0 underruns=0` |

## Still unknown

- **UEFI.** The firmware offers it, and this run deliberately used the legacy
  path. Booting the same ISO through `BOOTX64.EFI` needs Secure Boot off, and
  GOP should give the same mode.
- **Automatic jack switching.** Plugging speakers into the jack left the
  sound on the internal speakers until the jack was chosen in Sound. The
  automatic switching option was not turned on for the run, so that is the
  default behaviour: `azalia` drives one output at a time, and switching on plug
  is an optional `audiod` policy that starts disabled
  ([AZALIA.md](../kernel/AZALIA.md#jack-presence)). Whether the ALC269 reports
  presence on that jack, and the switch follows with the policy on, is
  untested.
- **HDMI**, its audio, and anything that needs ACPI beyond S5: battery, lid,
  sleep, thermal (HW-14).

## Work for this target

1. **HW-1.** The Ethernet is the same `10ec:8168` as the development host's,
   so this laptop gets a network with the same driver.
2. **The `smp:` APIC mode line**: print what the CPU is in, not Limine's flag.
3. **The UEFI path**, once, to know whether the ISO boots with only Secure
   Boot switched off.
4. **Jack switching on the ALC269**: turn the policy on and plug the jack.
   Only if it does not follow is there codec work (HW-6).
5. **HW-14**: the battery level, AC state and the lid.
6. A driver for the Intel 3160 is a Wi-Fi project, and Wi-Fi is out of scope.

## Sources

- The boot log of the run on 2026-11-12 (`system-log-i7-eze.txt`, not in the
  tree).
- Retail listings: [Cetrogar](https://www.cetrogar.com.ar/notebook-compaq-21n2f7ar-core-i7.html),
  [Mauri Computación](https://www.mauricomputacion.com.ar/catalogo/16800-compaq-presario-21n2f7ar-intel-i7-6500u-4g-1tb-win10-14-pulg-full-hd),
  [Lemon Store](https://www.lemonstore.com.ar/a_5951_-nb-compaq-21n2f7ar-corei7-4gb-1tb-wc-14-pulgadas-win-10-notebooks-santiago-del-estero-santiago-del-estero).
- Launch of the line: [Enfasys, 2016-09-02](https://www.enfasys.net/2016/09/02/compaq-presenta-su-nueva-linea-de-notebooks/).
- Board and parts: [Vulcano, K21_VC 1 for the 21N2F7AR](https://www.vulcano.com.ar/motherboard-para-notebook-compaq-21n-21n2f7ar-k21_vc-1-42212.htm),
  [Vulcano, K21_VC 1 for the 21N2F5AR](https://www.vulcano.com.ar/motherboard-para-notebook-compaq-21n-21n2f5ar-k21_vc-1-42213.htm).
