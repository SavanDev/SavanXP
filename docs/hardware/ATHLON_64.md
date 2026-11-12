# AMD Athlon 64 + chipset graphics

A 2003-2008 desktop on socket 754, 939 or AM2: a K8 processor, a third-party
chipset more often than an AMD one, and graphics in the north bridge. It is
the oldest machine `master` can boot. The shared contract and the numbered
work packages (HW-n) are in [REAL_HARDWARE.md](REAL_HARDWARE.md).

**Nothing in this document has been run.** The machine was described as an
"AMD Athlon X64", taken here as the Athlon 64 family (single-core Athlon 64
and dual-core Athlon 64 X2). The unit has not been inventoried, and on this
platform the board matters more than the CPU: four chipset vendors shipped
integrated graphics for it, each with its own disk, USB, audio and network
controllers. Everything about devices is **expected**.

## Verdict

The CPU qualifies, and that is the finding worth recording: K8 has long mode
and NX, and `master` needs nothing later. Limine's BIOS path should load the
kernel from a DVD. From there this is the target with the most in its way:

- the wall clock is not trustworthy on this CPU as the kernel chooses it
  today, on a dual-core part in particular (HW-9);
- the disk controller is almost certainly not AHCI (HW-3);
- there is no USB (HW-2);
- memory may be too small for the LiveCD's RAM volume (HW-12);
- audio is likely an AC'97 controller the driver would mishandle (HW-7).

The one device that may simply work is the network card, which on many of
these boards is the RTL8139 the tree already drives.

## The processor

Every K8 has what the kernel requires. What varies by revision is everything
the kernel does not use:

| Feature | K8 | Effect on `master` |
| --- | --- | --- |
| Long mode, NX | All revisions | Boots |
| SSE3 | From revision E (Venice, San Diego, 2005) | Unused |
| `CMPXCHG16B`, `LAHF`/`SAHF` in long mode, `RDTSCP` | Missing on the early revisions | Unused. Nothing in the tree is compiled past the AMD64 baseline, which is the [rule](REAL_HARDWARE.md#the-instruction-set-baseline-is-a-compatibility-rule) that keeps this target alive |
| x2APIC, SMEP, SMAP, `RDRAND` | No | xAPIC over MMIO; `cpu: SMEP unavailable on this processor`; fallback entropy |
| Invariant TSC | **No** | See below |
| C1E | On revision F and later dual-core parts, when the firmware enables it | Can stop the local APIC timer; the kernel clears it on revision F and later (HW-9) |
| Cores | 1, or 2 on the X2 | Started by Limine |

### The clock is the first thing to fix

`master` uses the ACPI PM timer as its wall clock only when it is 32 bits
wide, and the TSC otherwise ([TIME.md](../kernel/TIME.md)). On K8 the TSC is
not a clock:

- it counts the core's own clock, so it changes rate with the frequency and
  slows or stops when the core is halted with clock ramping;
- on an Athlon 64 X2 the two cores' counters are not synchronized and drift
  apart. A read on one core followed by a read on the other can go backwards.
  This is the defect that needed a vendor "dual-core optimizer" on the
  desktop systems of the time.

Every deadline in the kernel hangs off that clock. With a 24-bit PM timer --
which is what a south bridge of this age is likely to have -- an X2 would run
with time that jumps. HW-9 has the fix: without the invariant-TSC CPUID bit,
count the PM timer's wraps or use the HPET.

Until then, the machine is only a fair test on a **single-core** Athlon 64, or
with the boot log showing
`timer: reloj de pared por PM timer ACPI (..., 32 bits)`.

## Chipset graphics

Which one the unit has decides nearly every other row of this document:

| Chipset family | Graphics | South bridge | Notes |
| --- | --- | --- | --- |
| NVIDIA GeForce 6100/6150 + nForce 410/430 (C51 + MCP51) | GeForce 6100, 6150 | MCP51 | The commonest AM2 and late-939 integrated board |
| NVIDIA GeForce 6150SE / nForce 430 (MCP61), GeForce 7025/7050 (MCP68) | GeForce 6150SE, 7025, 7050 | single chip | Budget AM2 boards to 2009 |
| ATI Radeon Xpress 200 (RS480, RS482), Xpress 1150 (RS485) | X300-class | SB400, SB450 or ULi M1573 | Socket 939 and early AM2; common in HP and Compaq desktops |
| AMD 690G (RS690) | Radeon X1250 | SB600 | AM2, 2007 |
| VIA K8M800, K8M890 | S3 UniChrome Pro, Chrome9 | VT8237, VT8237A, VT8251 | Socket 754 and 939 budget boards |
| SiS 760, 761GX | Mirage, Mirage 1 | SiS 964, 965, 966 | Socket 754 and 939 budget boards |

**Today:** VBE through Limine and nothing else (HW-11). The NVIDIA and ATI
video BIOSes offer the VESA 4:3 list dependably; the VIA and SiS ones are the
likeliest to have a short list, limited by 16 to 64 MB of shared memory.
1024x768x32 is the mode to expect to work everywhere.

**Native mode setting is not planned for any of them.** The GeForce chipsets
have no public documentation; UniChrome and Mirage are dead ends with one or
two boards each. The 690G is the exception in principle -- its display side
is AtomBIOS-driven like the [Athlon II](ATHLON_II.md) chipsets -- and could
ride along if that work is ever done. The firmware framebuffer is the design
for this target.

## The rest of the board

| Device | Expected, by chipset | Today | Package |
| --- | --- | --- | --- |
| Firmware | Legacy BIOS (Award, AMI, Phoenix) | Limine BIOS from a DVD. Booting from a USB stick depends on the firmware offering "USB-HDD" with INT 13h extensions | — |
| Disks | PATA for most disks and every optical drive. SATA as an IDE-class or RAID-class function on nForce4/MCP51/MCP61, SB400, VIA and SiS. AHCI only on SB600, later MCPs and VT8251, each with quirks | **Probably no disk.** The legacy-port ATA driver reaches a PATA disk only where the firmware left the channel in compatibility mode, and then in PIO | HW-3, HW-4 |
| USB | OHCI and EHCI (NVIDIA, ATI, ULi, SiS) or UHCI and EHCI (VIA) | **No USB**, and no card to fix it on a board without PCIe. PS/2 keyboard and mouse | HW-2 |
| Audio | AC'97 on the older boards: NVIDIA (ICH-like), ATI IXP on SB400, VIA VT823x, SiS 7012, with Realtek ALC650/655/850 codecs. HD Audio on MCP51 and later, SB600, VT8237A/VT8251 | `ac97.cpp` binds any AC'97-class function and drives it as an Intel ICH. That is close on NVIDIA and SiS and **wrong on ATI and VIA**. HD Audio lacks the ATI and NVIDIA quirks | HW-7, HW-6 |
| Ethernet | NVIDIA's own MAC on nForce; **Realtek RTL8139/8100C** on many ATI, VIA, SiS and OEM boards; RTL8110S/8169 or RTL8111B; VIA Rhine or Velocity; SiS 900; Marvell or Broadcom on early enthusiast boards | An RTL8139 (`10ec:8139`) should work with the existing driver. Anything else: no network | HW-15, HW-1 |
| Input | Two PS/2 ports | Works with the existing driver | — |
| Timers | PM timer always. HPET on SB600 and the later MCPs; absent on VT8237 and most early boards | See [above](#the-clock-is-the-first-thing-to-fix) | HW-9 |
| Interrupts | MADT and IOAPIC, with firmware of the period known for wrong interrupt-source overrides on nForce and ATI boards | The override for IRQ 0 only matters to the PIT fallback. A wrong override elsewhere would show as a device that never interrupts | — |
| Memory | 256 MiB to 4 GiB; 512 MiB to 2 GiB is typical, less what the graphics core borrows | Below 1 GiB the LiveCD's 256 MiB volume leaves too little. Build a smaller one, or install | HW-12 |

On a board with PCI slots only, the RTL8139 card is still the cheap way to a
network. A board with a PCIe x1 slot (most AM2 ones) also takes an xHCI card.

## Work for this target

1. **Inventory the board.** Until the chipset is known this document is a
   list of possibilities.
2. **HW-9: the clock.** Selection by the invariant-TSC bit and wrap counting
   on the 24-bit PM timer; the C1E bits are already cleared. Without it no measurement taken on
   the machine can be believed, including the ones the next steps need.
3. **Run it as it is** from a DVD with the boot log on screen and PS/2 input.
   Record `cpu:`, `timer:`, `smp:`, `fb_gpu:`, `ata:` and `ac97:`.
4. **HW-7, first half:** stop `ac97.cpp` binding controllers it does not
   know. This is a safety fix and should precede any boot on an ATI or VIA
   board.
5. **HW-3.** PCI IDE is the storage driver of this whole generation: PATA,
   and every SATA controller here that is not AHCI.
6. **HW-12.** With a disk driver, an installed system removes the memory
   problem.
7. **HW-2**, EHCI then OHCI or UHCI by chipset.
8. **Audio and network**, per the controllers the unit has.

Steps 2, 4, 5 and 7 are the same code the
[Pentium 4 laptop](PENTIUM4_EVO_N1020V.md) needs on `legacy`. An Athlon 64 is
the machine on which `master` can develop and test it.

## Facts to collect

- `lscpu`: model, stepping (it gives the revision), one core or two.
- `lspci -nn`: the whole list. On this platform it is the document.
- `dmesg | grep -i -E 'tsc|hpet|pm-timer|c1e|clocksource'` from Linux: what
  another kernel decided about this machine's clocks, and whether it marked
  the TSC unstable.
- `sudo dmidecode -t memory -t bios`: installed memory, firmware vendor and
  date.
- Firmware settings: Cool'n'Quiet, C1E, the memory lent to graphics, SATA
  mode where there is one, USB legacy support, boot devices offered.
- The SavanXP boot log: the wall clock line, the VBE mode, and whether `ata:`
  finds anything.
