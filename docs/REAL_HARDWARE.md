# Real hardware: the Compaq Evo N1020v case

> **Status: analysis only, nothing implemented.** This document is the
> measurement: what stops SavanXP from booting a real 2003 laptop, which parts
> of that answer are architecture and which are driver bugs that QEMU and
> VirtualBox have been hiding, and in what order the work should be done.
>
> The reference machine is a **Compaq Evo N1020v** (2002), chosen because it is
> the least forgiving machine a hobby OS is likely to meet: a 32-bit Pentium 4
> with no long mode, a legacy BIOS, PATA storage, AC'97 audio, a PS/2 trackpad
> and an LCD panel that may only exist in 24-bit video modes.
>
> Nothing here is implemented and nothing here is scheduled. The two sections
> that matter for any other machine are
> [Hardware work that is not about the CPU](#hardware-work-that-is-not-about-the-cpu)
> (driver defects worth fixing regardless of architecture) and
> [Settling the open questions](#settling-the-open-questions-before-writing-code)
> (five minutes of work that decides the whole plan).

## The short version

SavanXP does not boot on that machine today, and the reason is not a missing
driver: **the machine has no 64-bit mode and the system is 64-bit only.** The
CPU is a socket 478 Northwood Pentium 4, which predates EM64T, NX and x2APIC.

Three things stand between that CPU and the kernel, and not one of them is a
driver:

1. the kernel, the userland, the SDK and every port are built for
   `x86_64-unknown-none-elf` ([`CMakeLists.txt:69`](../CMakeLists.txt:69)) and
   the kernel links at `0xffffffff80000000` with `-mcmodel=kernel`
   ([`linker.ld:13`](../linker.ld:13), [`CMakeLists.txt:80`](../CMakeLists.txt:80));
2. NX is a hard requirement, not a feature
   ([`KERNEL_SECURITY.md:59`](KERNEL_SECURITY.md)), and that CPU does not have it;
3. the `limine` boot protocol is **64-bit only** by specification, and Limine
   refuses to run on a CPU without long mode -- including its own IA-32 build,
   whose sole purpose is loading a 64-bit kernel from 32-bit firmware.

So the machine forces one of two decisions:

- **Route A** — port the system to i386 and replace the bootloader. Weeks to
  months, and it is a project, not a ticket.
- **Route B** — target 64-bit hardware instead and use the N1020v only as the
  reason to fix the driver and media gaps. The hybrid ISO already boots through
  a legacy BIOS; what is missing is the list further down in this document.

The driver-level findings below are needed on **either** route, on any machine
that is not QEMU. They are small, local, and verifiable in the existing harness.

## The reference machine

From the [MacDat entry](https://www.macdat.net/laptops/compaq/evo_n1020v.php), the
[Ars Technica review thread](https://arstechnica.com/civis/threads/review-compaq-evo-n1020v-laptop-photos-included.702897/)
and photographs of real units:

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

- **Socket 478, Northwood.** No EM64T (`CPUID.80000001H:EDX[29]` is 0), no NX
  (bit 20), no x2APIC (`CPUID.1H:ECX[21]`), no SMEP, no SMAP, no PCID, no
  invariant-TSC bit. There is a P4 TSC at a constant rate, `cmpxchg16b`, SSE2
  and a local APIC in xAPIC mode -- enough to run a real kernel, but not enough
  to run this one.
- **The panel is 1024x768 or 1400x1050.** The video path in the kernel is
  32-bpp-only, and what a 2002 VESA BIOS offers for a laptop panel is exactly
  the question that decides whether there is a desktop at all.

Everything else in the table is a driver question, and the answers are in
[Hardware work that is not about the CPU](#hardware-work-that-is-not-about-the-cpu).

## Why it cannot boot today

### The system is 64-bit in its bones, not just in its flags

The compiler triple is set once
([`CMakeLists.txt:69`](../CMakeLists.txt:69)) and feeds four profiles: kernel,
uACPI, in-tree userland and external apps/ports. Beyond the flags, the shapes
assume a 64-bit address space:

| Assumption | Where |
| --- | --- |
| Kernel at `0xffffffff80000000`, kernel PML4 slot 256 | [`linker.ld:13`](../linker.ld:13), [`vmm.cpp:16`](../kernel/vmm.cpp:16) |
| 4-level paging, `sign_extend_48`, 48-bit page mask | [`vmm.cpp:15-67`](../kernel/vmm.cpp), [`elf.cpp:116-137`](../kernel/elf.cpp) |
| User stack top at 512 GiB, section views at 16 GiB | [`vmm.hpp:14-15`](../include/kernel/vmm.hpp), [`vmm.hpp:37`](../include/kernel/vmm.hpp) |
| NX bit 63 on every kernel mapping | [`vmm.hpp:49`](../include/kernel/vmm.hpp), [`vmm.cpp:810`](../kernel/vmm.cpp) |
| ELF64 / `EM_X86_64` only | [`elf.cpp:10-12`](../kernel/elf.cpp) |
| 64-bit syscall context, `iretq`, `pushq` | [`context.S:26-98`](../arch/x86_64/context.S), [`process.hpp:45-66`](../include/kernel/process.hpp) |
| 64-bit GDT, TSS with 8 ISTs, 10-byte IDT gates, `fxsave64` | [`cpu_init.cpp:59-95`](../arch/x86_64/cpu_init.cpp), [`cpu_init.cpp:755-764`](../arch/x86_64/cpu_init.cpp) |
| HHDM as the only physical-address path | [`vmm.cpp:967-969`](../kernel/vmm.cpp), [`physical_memory.cpp:111-113`](../kernel/physical_memory.cpp) |

That is roughly 26,700 lines of kernel, 19 files with inline assembly and three
`.S` files, plus 70 in-tree programs and the ports, against
**one compiler triple hardcoded in 8 places**
(see [Route A](#route-a-port-the-system-to-i386)).

### NX is a boot requirement

[`enable_nx()`](../arch/x86_64/cpu_init.cpp) reads `CPUID.80000001H:EDX[20]`
and the BSP **panics** without it
([`cpu_init.cpp:785-787`](../arch/x86_64/cpu_init.cpp)); an AP halts forever.
`docs/KERNEL_SECURITY.md` records the policy, and it is a deliberate one. On
this CPU it cannot be satisfied at all: the Northwood has no NX page-table bit,
and in 32-bit protected mode without PAE there is no encoding for it. An i386
port therefore has to relax the policy, or use PAE and say so.

### The boot protocol has no 32-bit contract

This is the finding that changes the plan rather than just the effort. The
`limine` protocol is documented as 64-bit only: *"Only 64-bit, Little Endian
machines are supported"* and *"All pointers are 64-bit wide"*, there is no
"paging off" mode (the minimum paging mode is 4-level), and the reference
implementation opens with a long-mode CPUID check that panics with *"limine:
This CPU does not support 64-bit mode."*

Limine does list "IA-32 (32-bit x86)" as a supported architecture, and the
release ships `BOOTIA32.EFI` and `*_ia32.asm_x86` files. Those are the
**bootloader itself** built for a narrower path -- the BIOS boot sector always
starts in 16-bit real mode, and the IA-32 UEFI binary exists for 32-bit-only
*firmware* -- but every one of those paths still ends by switching the CPU
*into* long mode to hand a 64-bit kernel over. On a CPU that has no long mode
the loader panics in its CPUID check before it ever reaches a protocol. The only
32-bit-capable protocols in that tree are Linux and Multiboot, and SavanXP
implements neither.

### The kernel brings up no paging of its own

[`vmm::initialize()`](../kernel/vmm.cpp) stores the HHDM offset and adopts
Limine's CR3 (`read_cr3() & kPageMask`); nothing anywhere writes `CR0.PG`,
builds a page table or relocates the kernel. The page tables, the direct map
and the long-mode entry are all the bootloader's. Whatever replaces Limine has
to leave the CPU in a state this kernel can adopt, or the kernel has to grow a
paging bootstrap from scratch.

## Route A: port the system to i386

The only path to that laptop as it is. Limine is out, which in practice means
**GRUB2 with the Multiboot2 protocol**: it is the well-trodden 32-bit legacy-BIOS
loader, and the tags it provides (E820 map, framebuffer, modules, ACPI RSDP,
cmdline) map almost one to one onto what `boot::BootInfo` already carries
([`entry.cpp`](../arch/x86_64/entry.cpp) would become the Multiboot reader).
What is lost and has to be rebuilt by the kernel: the HHDM (a fixed 1 GiB
direct map covers any of these machines), the AP start-up trampolines (INIT/SIPI
from the MADT instead of Limine's `goto_address`), and paging itself.

| Package | Size | Notes |
| --- | --- | --- |
| Build graph: `SAVANXP_ARCH`, four profiles, `linker32.ld`, `-m elf_i386` | S-M | The triple is hardcoded in 8 places across 6 files: [`CMakeLists.txt:69`](../CMakeLists.txt:69), [`tools/build-user.sh:126`](../tools/build-user.sh), [`ports/ccleste/env.sh:71-72`](../ports/ccleste/env.sh), [`ports/doomgeneric/build.sh:110`](../ports/doomgeneric/build.sh), [`ports/ffmpeg/env.sh:62-63`](../ports/ffmpeg/env.sh), [`subsystems/native/build.sh:161`](../subsystems/native/build.sh) |
| `arch/x86`: 32-bit GDT/TSS/IDT, `iret`/`pushal` stubs, `fxsave`, AP trampolines | L | Replaces `arch/x86_64` (`entry`, `cpu_init`, `timer`, `smp`, `context.S`) |
| Paging bootstrap and a 1 GiB direct map, no NX | L | Nothing exists today; `EFER` does not exist in 32-bit mode |
| VMM rewrite | XL | The user address map ([`vmm.hpp:14-15`](../include/kernel/vmm.hpp)) does not fit in 32 bits. Cheapest credible MVP: one address space, per-process stacks, no per-process CR3, no section views, no ASLR -- that halves the work and defers the isolation story |
| `SavedContext` (168 -> 52 bytes), i386 syscall register mapping, ELF32, 4-byte argv slots | M | [`context.S`](../arch/x86_64/context.S), [`elf.cpp:163-263`](../kernel/elf.cpp) |
| Heap off HHDM pointers | S | [`heap.cpp:224-244`](../kernel/heap.cpp) builds arenas straight on the direct map |
| Userland, SDK, ports, busybox | M | The runtime is SavanXP's own freestanding one ([`subsystems/posix/sdk/v1/runtime`](../subsystems/posix/sdk/v1/runtime)), so there is no musl/glibc and no multilib to arrange |
| Harness: a second QEMU machine profile | S-M | See [A permanent harness](#a-permanent-harness-for-a-32-bit-target) |

The good news for the ABI is specific and worth protecting. Syscalls are a
DPL=3 `int $0x80` gate ([`abi/savanxp_native_abi.h:10-12`](../include/abi/savanxp_native_abi.h)),
which exists identically in 32-bit protected mode; the dispatchers pass user
addresses as explicit-width `uint64_t` rather than raw pointers
([`syscall_dispatch.inc:9-25`](../subsystems/posix/kernel/syscall_dispatch.inc));
and the shared SDK structs contain no pointers at all, only fixed-width integers
([`savanxp/syscall.h`](../subsystems/posix/sdk/v1/include/savanxp/syscall.h)).
So the ~440-line dispatcher and the whole SDK survive a port almost unchanged.
The one real LP64 hole is the initial process stack, which is a native SysV argv
array of 8-byte slots ([`elf.cpp:170-239`](../kernel/elf.cpp)).

Order of work, with each step independently bootable and testable:
`build graph` -> `multiboot2 entry + paging bootstrap + serial console` ->
`ATA and SxFS root` -> `ELF32 and a shell` -> `PS/2 and the desktop` ->
`isolation features, if ever`.

## Route B: target 64-bit hardware

If the CPU had long mode, nothing above would be needed: the hybrid ISO already
boots through a legacy BIOS, and
[`tools/iso_boot_test.py`](../tools/iso_boot_test.py) proves both El Torito
paths today. What Route B still needs is everything in the next section, plus
some way to write the OS to a real device.

A 478 Prescott at 3.0 GHz would bring EM64T and NX to this chassis, but it is a
103 W part in a machine designed around a 2.4 GHz one, behind a firmware CPU-ID
check. Not worth the gamble. The sane version of Route B is to fix the list
below and aim at a different x86_64 laptop with a legacy BIOS; the N1020v then
becomes the machine Route A unlocks later.

## Hardware work that is not about the CPU

Every item here is a defect that QEMU, VirtualBox or the current test matrix
tolerates, and that real firmware does not. They apply to **any** metal, on
either route, and they are small enough to land one at a time with the existing
smokes as the guard.

| # | Work | Where |
| --- | --- | --- |
| 1 | **32-bpp-only video.** `bpp == 32` is a hard gate in the driver, the console and the splash, and the userland blitter assumes a 4-byte stride. If the firmware's VESA implementation has no 32-bpp linear mode, there is no console, no splash, no `/dev/gpu0`, no desktop -- only serial. Either support 24/16 bpp end to end, or program a 32-bpp mode | [`fb_gpu.cpp:235`](../kernel/fb_gpu.cpp), [`console.cpp:86`](../kernel/console.cpp), [`boot_screen.cpp:359`](../kernel/boot_screen.cpp), [`gfx2d.c:1002-1005`](../subsystems/posix/sdk/v1/runtime/gfx2d.c) |
| 2 | **No write-combining on the firmware framebuffer.** The WC remap through the PAT exists only for Bochs' `dispi` adapter. The scanout Limine hands over is used as mapped. On a 2.4 GHz Pentium 4 with uncached or write-through video memory the compositor would be unwatchable; remapping the aperture should not depend on `dispi` | [`fb_gpu.cpp:181-232`](../kernel/fb_gpu.cpp), [`vmm.cpp:924-957`](../kernel/vmm.cpp) |
| 3 | **ATA never enables the PCI IDE function.** The driver hardcodes `0x1F0/0x170`, does not include `pci.hpp`, and nothing writes the IDE function's command register. On firmware that gates legacy IDE decode on PCI command bits 0 and 2, every port reads `0xFF` and no disk is found. PIO only, a 255-sector cap the block core does not chunk, and a million-iteration "timeout" with no SRST recovery | [`ata.cpp:8-11`](../kernel/ata.cpp), [`ata.cpp:87-115`](../kernel/ata.cpp), [`block.cpp:40-47`](../kernel/block.cpp) |
| 4 | **AC'97 mixer writes assume an emulator.** `unmute_mixer()` writes `0x0000` to NAM offset `0x00`, whose real meaning is *System Power Down*; the reset bit is 15. QEMU ignores it, real codecs may go silent | [`ac97.cpp:185-191`](../kernel/ac97.cpp) |
| 5 | **No trackpad.** AUX init insists on `0xFF` reset, `0xF6` defaults and the IntelliMouse sample-rate knock; many 2003-era PS/2 pads do not acknowledge those. The keyboard survives, the pointer does not. No absolute or digitizer mode either. And if the pad enumerates as USB there is nothing at all: the tree has no USB stack | [`ps2.cpp:1044-1066`](../kernel/ps2.cpp) |
| 6 | **Wrong NIC coverage.** Only exact `10EC:8139` is claimed. The 10/100 chip in this family varies by SKU (8139C, 8110/8111, 82559, BCM44xx) | [`rtl8139.cpp:23-24`](../kernel/rtl8139.cpp), [`rtl8139.cpp:369`](../kernel/rtl8139.cpp) |
| 7 | **Local APIC timer erratum on P4-class silicon.** The periodic timer runs at divide 16 with a calibrated ~7500-count period at 1 kHz, which is the configuration the "Local APIC Timer May Run at Intermittent Intervals" erratum warns about on Pentium 4 and ICH2-4. The PIT backend already exists and is the natural default on such a CPU | [`timer.cpp:123-154`](../arch/x86_64/timer.cpp), [`timer.cpp:227-230`](../arch/x86_64/timer.cpp), [`timer.cpp:156-183`](../arch/x86_64/timer.cpp) |
| 8 | **SMEP asymmetry.** The BSP prints a warning when SMEP is missing; an AP that cannot enable it halts forever. Pre-Nehalem has no SMEP, so on a multi-core P4 every secondary core is lost at startup | [`cpu_init.cpp:788-790`](../arch/x86_64/cpu_init.cpp), [`cpu_init.cpp:806-808`](../arch/x86_64/cpu_init.cpp) |
| 9 | **ACPI on real firmware.** The SCI is routed without applying the MADT interrupt-source overrides, and every GPE is masked: no lid, no sleep, no wake, no battery. uACPI never runs `namespace_initialize`, so `_STA`/`_INI` never power anything up, and GPE work runs synchronously in interrupt context | [`acpi.cpp:275-288`](../kernel/acpi.cpp), [`acpi.cpp:479`](../kernel/acpi.cpp), [`uacpi_glue.cpp:125-160`](../kernel/uacpi_glue.cpp) |
| 10 | **No way to install on metal.** Nothing in `tools/` writes to a device. The build already stages `limine-bios.sys` and the ISO target already runs `limine bios-install`, so a `--target /dev/sdX` target is plumbing, not new technology. Note Limine's BIOS stage 1 requires INT 13h extensions and has no CHS path | [`CMakeLists.txt:713-714`](../CMakeLists.txt), [`CMakeLists.txt:771`](../CMakeLists.txt) |
| 11 | **Performance and capacity.** 256 MB-1 GB of RAM, PIO reads, no SpeedStep or thermal handling, a 256 KB static userland heap, and `resolution: 1280x800x32` hardcoded in the boot config for a panel that is not that | [`boot/limine.conf:12`](../boot/limine.conf), [`runtime/posix.c:43-44`](../subsystems/posix/sdk/v1/runtime/posix.c) |

## Settling the open questions before writing code

The plan branches on facts nobody in this repository knows yet, and all of them
are cheap to obtain on the machine itself:

1. **Does the CPU have long mode?** `cpuid -1 -l 0x80000001` from any live
   Linux, or the Windows/Phoenix equivalent. Bit 29 is long mode, bit 20 is NX.
   A socket 478 Northwood says no; a misidentified unit might say something else.
2. **What video modes does the BIOS actually offer?** Boot the ISO
   (`./build.sh iso`) burned to a DVD. With a 32-bit-only CPU Limine panics with
   *"This CPU does not support 64-bit mode."* before touching the framebuffer;
   the same run on a 64-bit CPU lists the VBE modes it enumerated, which settles
   item 1 of the table above.
3. **Which NIC and which pointing device?** One boot log, or the Windows Device
   Manager, names both.
4. **Can the BIOS boot from USB?** Look at the boot menu. The DVD path is the
   safe one; a 2002 firmware may not enumerate a USB stick at all.

Answer 1 decides Route A versus Route B, and answer 2 decides whether there is a
desktop at all. Both are minutes of work, and both are worth doing before a
single line is written.

## A permanent harness for a 32-bit target

If Route A is taken, the work needs the same guard rails the rest of the
project has, and QEMU can provide them. `qemu-system-i386` boots through a
legacy BIOS with no long mode available to the guest at all, and a `pc` machine
with an `ide-hd` disk, PS/2 input, AC'97 and a VGA device that answers the Bochs
VBE extension reproduces everything the i386 target depends on: a real E820 map,
an interrupt-driven PIC, PATA storage, and the same fallback backends the x86_64
machine already exercises. NX and PAE are switchable per model
(`-cpu <model>,-nx,-pae`), and the active feature set of any model can be read
back over QMP with `query-cpu-model-expansion`, so the harness can assert the
CPU it thinks it is running on rather than trusting a name. The existing smoke
scenarios and `tools/shoot.sh` would then run against a second machine profile,
which is the same shape as the `--virtio` switch the project already has.

What that harness **cannot** reproduce, and which therefore has to be checked on
the metal: the APIC timer erratum (not modelled), the absence or partiality of
the firmware's VESA implementation, BIOS USB boot, thermal and battery
behaviour, and the real timing of PIO reads against a spinning disk.

## What this document does not decide

- Whether Route A is worth doing at all. That is a project decision, and the
  answer changes if the goal is "SavanXP on real laptops" rather than "SavanXP
  on that laptop".
- Whether Route A keeps both architectures in one tree or builds a 32-bit
  variant. The current graph has no arch dimension at all
  ([`CMakePresets.json`](../CMakePresets.json) has no arch axis), so that choice
  is still open, and it is the difference between one `SAVANXP_ARCH` cache
  variable and two source trees.
- What the 32-bit isolation story is. Deferring per-process CR3 and section
  views is a suggestion for making the port tractable, not a design.
- Whether the desktop is expected to be usable on 1 GB of shared video memory
  and a 2.4 GHz P4 without hardware 3D. That is a performance question, and
  [`GRAPHICS_PERF.md`](GRAPHICS_PERF.md) is where it belongs.
