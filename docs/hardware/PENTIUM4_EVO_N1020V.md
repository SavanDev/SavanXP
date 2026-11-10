# Intel Pentium 4: the Compaq Evo N1020v

The 2002 Compaq laptop is the i386 target of the `legacy` branch. This
document says why it cannot run `master`, whether `master` could ever support
it, where the i386 port stands, and what the machine needs besides a 32-bit
kernel. The shared contract and the numbered work packages (HW-n) are in
[REAL_HARDWARE.md](REAL_HARDWARE.md).

## The short version

SavanXP `master` does not boot on that machine, and the reason is not a
missing driver: **the machine has no 64-bit mode and the system is 64-bit
only.** The CPU is a socket 478 Northwood Pentium 4, which predates Intel 64,
NX and x2APIC.

- **`master` will not support this machine.** Not as a matter of effort on a
  driver: the bootloader, the build and the kernel's memory model all assume
  long mode. See [Can `master` support it?](#can-master-support-it).
- **The `legacy` branch is the route.** Its i386 kernel boots from GRUB2 in
  QEMU, mounts the SxFS root over ATA, runs one 32-bit program in ring 3 and
  reads the PS/2 keyboard. It has never been run on the laptop.
- **A Pentium 4 is not excluded as such.** The late ones have Intel 64 and XD
  and are `master` targets like any other legacy-BIOS machine. See
  [64-bit Pentium 4 machines](#64-bit-pentium-4-machines).
- **Most of the device work is shared.** PCI IDE, USB before xHCI, AC'97 and
  the timer fixes are needed by the 64-bit targets too, so they land on
  `master` and reach `legacy` by merge.

## The reference machine

From the [MacDat entry](https://www.macdat.net/laptops/compaq/evo_n1020v.php), the
[Ars Technica review thread](https://arstechnica.com/civis/threads/review-compaq-evo-n1020v-laptop-photos-included.702897/)
and photographs of real units. Nothing here was read from the unit itself.

| | |
| --- | --- |
| CPU | socket 478 Intel Pentium 4 @1.8-2.4 GHz; some units Mobile Pentium 4 @1.8-1.9 GHz, Celeron @1.5-1.7 GHz |
| Chipset | ATI RS200 north bridge, "Radeon IGP 340" (`1002:4137`, R100, 2D), 32 MB shared |
| RAM | DDR266, 2 slots, 256 MB standard, 1 GB max (widened units report 1.50 GB) |
| Storage | 2.5" PATA disk, 20-60 GB; CD/DVD or DVD-RW optical |
| Display | 14.1" or 15" TFT at 1024x768, or 15" TFT at 1400x1050 |
| Audio | AC'97 codec, stereo speakers |
| Network | 56k modem; 10/100 Ethernet; WiFi/Bluetooth optional |
| Input | PS/2 keyboard and trackpad, no digitizer |
| Other | 1x CardBus, USB, VGA out, S-Video, FireWire, COMPAQ MultiPort, infrared |
| Firmware | Compaq/Phoenix-era legacy BIOS, no UEFI |

Two of those lines decide everything else:

- **Socket 478, Northwood.** No Intel 64 (`CPUID.80000001H:EDX[29]` is 0), no
  NX (bit 20), no x2APIC (`CPUID.1H:ECX[21]`), no SMEP, no SMAP, no PCID, no
  invariant-TSC bit. There is a P4 TSC at a constant rate, SSE2 and a local
  APIC in xAPIC mode -- enough to run a real kernel, but not enough to run
  this one.
- **The panel is 1024x768 or 1400x1050.** The video path in the kernel is
  32-bpp-only, and what a 2002 VESA BIOS offers for a laptop panel is exactly
  the question that decides whether there is a desktop at all.

**Expected**, from the platform rather than from a unit: the RS200 was paired
with an ALi M1535+ south bridge, which would make the IDE controller, the USB
1.1 (OHCI) controller and the audio controller ALi parts. The audio matters:
an ALi M5451 carries an AC'97 codec but is not an Intel ICH, and the AC'97
driver in the tree only knows the ICH (HW-7). Confirm with `lspci -nn`.

## Why it cannot boot `master`

### The system is 64-bit in its bones, not just in its flags

The compiler triple is set once (`SAVANXP_TARGET_TRIPLE` in `CMakeLists.txt`)
and feeds four profiles: kernel, uACPI, in-tree userland and external
apps/ports. Beyond the flags, the shapes assume a 64-bit address space:

| Assumption | Where |
| --- | --- |
| Kernel at `0xffffffff80000000`, kernel PML4 slot 256 | `linker.ld`, `kernel/vmm.cpp` |
| 4-level paging, `sign_extend_48`, 48-bit page mask | `kernel/vmm.cpp`, `kernel/elf.cpp` |
| User stack top at 512 GiB, section views at 16 GiB | `include/kernel/vmm.hpp` |
| NX bit 63 on every kernel mapping | `include/kernel/vmm.hpp`, `kernel/vmm.cpp` |
| ELF64 / `EM_X86_64` only | `kernel/elf.cpp` |
| 64-bit syscall context, `iretq`, `pushq` | `arch/x86_64/context.S`, `include/kernel/process.hpp` |
| 64-bit GDT, TSS with ISTs, 16-byte IDT gates, `fxsave64` | `arch/x86_64/cpu_init.cpp` |
| HHDM as the only physical-address path | `kernel/vmm.cpp`, `kernel/physical_memory.cpp` |

### NX is a boot requirement

`enable_nx()` reads `CPUID.80000001H:EDX[20]` and the BSP **panics** without
it (`cpu: NX/W^X is required`); an AP halts forever.
[KERNEL_SECURITY.md](../kernel/KERNEL_SECURITY.md) records the policy, and it
is a deliberate one. On this CPU it cannot be satisfied at all: the Northwood
has no NX page-table bit, and in 32-bit protected mode without PAE there is no
encoding for it. An i386 port therefore has to relax the policy, or use PAE
and say so.

### The boot protocol has no 32-bit contract

This is the finding that changes the plan rather than just the effort. The
`limine` protocol is documented as 64-bit only: *"Only 64-bit, Little Endian
machines are supported"* and *"All pointers are 64-bit wide"*, there is no
"paging off" mode (the minimum paging mode is 4-level), and the reference
implementation opens with a long-mode CPUID check that panics with *"limine:
This CPU does not support 64-bit mode."*

Limine does list "IA-32 (32-bit x86)" as a supported architecture, and the
release ships `BOOTIA32.EFI`. That is the **bootloader itself** built for a
narrower path -- the BIOS boot sector always starts in 16-bit real mode, and
the IA-32 UEFI binary exists for 32-bit-only *firmware* -- but every one of
those paths still ends by switching the CPU *into* long mode to hand a 64-bit
kernel over. The only 32-bit-capable protocols in that tree are Linux and
Multiboot, and `master` implements neither.

### The kernel brings up no paging of its own

`vm::initialize()` stores the HHDM offset and adopts Limine's CR3; nothing
anywhere writes `CR0.PG`, builds the first page table or relocates the kernel.
The page tables, the direct map and the long-mode entry are all the
bootloader's.

## Can `master` support it?

Three different things could be meant, and they have three answers.

### This CPU on `master`'s kernel: no

There is nothing to enable. The Northwood cannot enter long mode, and every
row of the table above is a consequence of long mode. No amount of driver
work changes that, and a CPU swap is not a way around it: Intel 64 arrived
with the LGA 775 Pentium 4s, and this document found no socket 478 part that
carries it. Check Intel's specifications for an exact sSpec before betting on
one.

### An i386 kernel inside `master`'s tree: possible, and not worth it

The `legacy` branch already proved the mechanics: a `SAVANXP_ARCH` axis picks
the triple, the flags, the linker script and the loader, and `arch/x86` sits
beside `arch/x86_64` behind the `namespace arch` contract
([ARCH.md](../kernel/ARCH.md)). Merging that axis into `master` is a small
change. Keeping it true is not:

- **The VMM does not port, it forks.** The user address map, per-process
  address spaces, section views, the HHDM and W^X-by-NX are 64-bit designs.
  The i386 kernel on `legacy` runs one program at a fixed window precisely
  because it has none of them yet. Everything `master` builds on top --
  threads, shared libraries, sections, the native dispatcher -- would need a
  second implementation or a second, weaker contract.
- **Every feature would have to land twice.** `master` is 206 commits past the
  point `legacy` branched from (v0.4.0, 2026-10-08), with about 27,700 lines
  added under `kernel/`, `arch/` and `include/` alone. The i386 side is eight
  commits. A tree with both makes every kernel change answer for an
  architecture whose only user is one laptop.
- **The test matrix doubles.** Every smoke and every visual check would run on
  two machine profiles, or the 32-bit one would rot unnoticed.
- **Security policy would become conditional.** NX is a hard requirement
  today. With a CPU that cannot have it in the same tree, "required" turns
  into "required where available", in the one document that should not hedge.

The decision recorded here stays as it was: **the 32-bit code lives on
`legacy`, and `master` keeps no architecture axis.**

### What `master` does for this machine: quite a lot

- **The device drivers.** HW-2, HW-3, HW-7, HW-8 and HW-9 are written on
  `master` for the 64-bit legacy-BIOS targets
  ([ATHLON_64.md](ATHLON_64.md), [CORE2_DUO.md](CORE2_DUO.md)), tested in the
  existing harness, and merged into `legacy`. The storage stack already
  crossed over that way: `ata`, `block`, `partition` and `sxfs` compiled for
  i386 untouched.
- **Staying portable where it is free.** Drivers keep to explicit-width
  types and the `arch::` operations, with no inline assembly outside `arch/`
  ([ARCH.md](../kernel/ARCH.md)). That rule is what made the storage stack
  cross, and it costs `master` nothing.
- **Taking the neutral fixes back.** A change `legacy` made to shared code
  that is correct on both architectures -- `vfs::initialize` accepting a
  missing initramfs is one -- belongs on `master`, so the branch's diff is
  only what is really 32-bit.

What `master` should not do is grow `#if` branches for i386 in shared files.
The branch carries those.

## 64-bit Pentium 4 machines

"Pentium 4" covers two different processors as far as SavanXP is concerned.
The Prescott-2M 6xx, the 5x1 models, Cedar Mill and the Pentium D have Intel
64 and the XD bit; they meet the hard rows of the
[contract](REAL_HARDWARE.md#what-master-requires-of-a-machine) and boot the
ordinary ISO through Limine's BIOS path. Nothing in the tree is specific to
them. **Expected**, not run:

| Area | On an LGA 775 Pentium 4 or Pentium D |
| --- | --- |
| CPU | Long mode and XD; the firmware may have XD switched off (HW-10). Hyper-Threading or two cores through xAPIC. No SMEP, no x2APIC |
| Timer | NetBurst is the family the local APIC timer erratum is about (HW-9). The PM timer on Intel's south bridges of the time is expected to be 24 bits wide, which puts the wall clock on the TSC; that TSC runs at a constant rate |
| Storage | ICH5, ICH6 or ICH7 with SATA in IDE mode and a PATA channel (HW-3); AHCI only on the R variants |
| USB | UHCI and EHCI (HW-2) |
| Audio | AC'97 on ICH5/ICH6, HD Audio from ICH6 on (HW-7, HW-6) |
| Video | Intel GMA 900/950 or a card in the AGP or PCIe slot, through VBE (HW-11) |
| Input | PS/2 |

Such a machine is the closest thing to the Compaq that `master` can run, and
the cheapest place to test the drivers the Compaq needs.

## The i386 port on `legacy`

> This work belongs to the `legacy` branch and is found there; `master` does
> not carry it. Status read from `origin/legacy` at `fec419a` (2026-10-18).

Limine is out, so the loader is **GRUB2 with the Multiboot2 protocol**: the
well-trodden 32-bit legacy-BIOS path, whose tags (E820 map, framebuffer,
modules, ACPI RSDP, cmdline) map almost one to one onto what `boot::BootInfo`
carries. What the kernel has to rebuild because Limine no longer does it: the
direct map, AP start-up (INIT/SIPI from the MADT) and paging itself.

| Package | Size | Status on `legacy` |
| --- | --- | --- |
| Build graph: `SAVANXP_ARCH`, four profiles, `linker32.ld`, `-m elf_i386` | S-M | **Landed.** `tools/arch-env.sh` carries the same mapping to the scripts outside CMake |
| Harness: a second QEMU machine profile | S-M | **Landed.** `--legacy` selects `qemu-system-i386`; `tools/check_legacy_cpu.py` asserts no long mode and no NX; `tools/legacy_boot_test.py` boots the ISO |
| Multiboot2 entry, flat GDT, COM1 console | M | **Landed** in `arch/x86` |
| Paging bootstrap and a 1 GiB direct map, no NX | L | **Landed** as an identity map of the first 1 GiB with 4 MiB pages |
| ATA and the SxFS root | M | **Landed.** `ata`, `block`, `partition`, `sxfs` and the VFS run in the 32-bit kernel |
| ELF32 and ring 3 | M | **Landed** for one program: GDT with user segments, a 32-bit TSS, `int $0x80`; `write`, `exit`, `getpid` and `system_info` answer |
| PS/2 | M | **Keyboard landed** as raw set 1 scancodes on serial, with the PIC remapped and a gate per IRQ. No decoding, no events to userland, no pointer |
| `arch/x86`: timer, scheduler entry, `fxsave`, AP trampolines | L | Not started. Interrupts are off during boot and there is no PIT handler |
| VMM | XL | Not started. One address space, one program. The cheapest credible next step is still per-process stacks without per-process CR3, section views or ASLR, which defers the isolation story |
| Syscall surface, shell, the rest of userland | L | Not started beyond the four calls above |
| Framebuffer console and desktop | L | Not started. Depends on what VBE mode GRUB can set on the unit |

The good news for the ABI is specific and worth protecting. Syscalls are a
DPL=3 `int $0x80` gate, which exists identically in 32-bit protected mode; the
dispatchers pass user addresses as explicit-width `uint64_t` rather than raw
pointers; and the shared SDK structs contain no pointers at all, only
fixed-width integers (`include/abi/savanxp_abi.h`). The one real LP64 hole is
the initial process stack, which is a native SysV argv array of 8-byte slots.

Two risks the status table hides:

- **The branch is behind.** It carries none of `master`'s work since v0.4.0:
  no USB, no HD Audio, no SUN growth, no VFS changes. Each merge from `master`
  is now a port of its own, and it gets harder with every release.
- **It has only run in QEMU.** `qemu-system-i386` with a `qemu32` CPU has no
  long mode and no NX, which is the property that matters, but it is not an
  ATI RS200 with an ALi south bridge.

### A permanent harness for a 32-bit target

`qemu-system-i386` boots through a legacy BIOS with no long mode available to
the guest at all, and a `pc` machine with an `ide-hd` disk, PS/2 input, AC'97
and a VGA device that answers the Bochs VBE extension reproduces everything
the i386 target depends on: a real E820 map, an interrupt-driven PIC, PATA
storage, and the same fallback backends the x86_64 machine already exercises.
NX and PAE are switchable per model (`-cpu <model>,-nx,-pae`), and the active
feature set of any model can be read back over QMP with
`query-cpu-model-expansion`, so the harness asserts the CPU it thinks it is
running on rather than trusting a name.

What that harness **cannot** reproduce, and which therefore has to be checked
on the metal: the APIC timer erratum (not modelled), the absence or
partiality of the firmware's VESA implementation, BIOS USB boot, thermal and
battery behaviour, and the real timing of PIO reads against a spinning disk.

## Device work for this machine

Independent of the CPU, and mostly shared with the 64-bit targets:

| Device | Work | Package |
| --- | --- | --- |
| PATA disk and optical drive on a PCI IDE function | Enable the function, native-mode BARs, bus-master DMA. On a 2.4 GHz CPU with a 4200 rpm disk, PIO is the difference between slow and unusable | HW-3 |
| Radeon IGP 340 through VBE | The 32 bpp gate and the mode list of a 2002 VBIOS. On `legacy` the kernel must also map the framebuffer write-combining itself, since GRUB sets up no page tables | HW-11 |
| Trackpad | A pad that does not acknowledge the reset or the IntelliMouse knock leaves the keyboard and no pointer | HW-8 |
| USB 1.1 | OHCI, if the south bridge is ALi's. The xHCI stack on `master` is of no use here | HW-2 |
| Audio | AC'97 codec behind a controller that is probably not an ICH | HW-7 |
| Ethernet | Only exact `10ec:8139` is claimed. The 10/100 chip in this family varies by unit | HW-15 |
| Local APIC timer | The divide-by-16 periodic configuration on P4-class silicon. The PIT is the natural default on such a CPU | HW-9 |
| ACPI | No lid, battery, thermal zone or fan policy: the embedded controller is never spoken to | HW-14 |
| Memory | 256 MB to 1 GB with 32 MB lent to video. A 256 MiB RAM volume cannot be the root here; the system has to run from the disk | HW-12 |

## Settling the open questions before writing code

The plan branches on facts nobody in this repository knows yet, and all of
them are cheap to obtain on the machine itself:

1. **Does the CPU have long mode?** `cpuid -1 -l 0x80000001` from any live
   Linux. Bit 29 is long mode, bit 20 is NX. A socket 478 Northwood says no; a
   misidentified unit might say something else. Booting the `master` ISO
   answers the same question in one line: Limine stops with *"This CPU does
   not support 64-bit mode."*
2. **What video modes does the BIOS actually offer?** `videoinfo` at the GRUB
   prompt of the `legacy` ISO lists them. This settles whether there is a
   32 bpp mode at the panel's size.
3. **Which south bridge, NIC, audio controller and pointing device?**
   `lspci -nn` and one boot log.
4. **Can the BIOS boot from USB?** Look at the boot menu. The DVD path is the
   safe one; a 2002 firmware may not enumerate a USB stick at all.

## What this document does not decide

- Whether finishing the i386 port is worth doing at all. That is a project
  decision, and the answer changes if the goal is "SavanXP on real machines"
  rather than "SavanXP on that laptop": every other target in this directory
  runs `master`.
- What the 32-bit isolation story is. Deferring per-process CR3 and section
  views is a suggestion for making the port tractable, not a design.
- Whether the desktop is expected to be usable on a 2.4 GHz Pentium 4 with
  shared video memory and no hardware drawing. That is a performance
  question, and [GRAPHICS_PERF.md](../desktop/GRAPHICS_PERF.md) is where it
  belongs.
