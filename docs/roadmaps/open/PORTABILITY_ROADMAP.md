# Portability: a second 64-bit architecture

> **Status: proposed (2026-11-24).** Phases P1-P5 are worth doing on x86_64
> for their own sake and can start any time. P6 onwards needs a decision to
> carry a second architecture, which this document prepares but does not
> take. The current boundary is [ARCH.md](../../kernel/ARCH.md); why `master`
> has no architecture axis today is in
> [PENTIUM4_EVO_N1020V.md](../../hardware/PENTIUM4_EVO_N1020V.md#an-i386-kernel-inside-masters-tree-possible-and-not-worth-it).

The question that opened this: *would an NT-style split between the kernel and
a HAL make SavanXP portable to other architectures?* Partly. A HAL is the
smaller half of the work, and SavanXP already has a good part of it. This
document records what actually stands between `master` and a second
architecture, in the order that removes it, and names the first target.

## What NT's HAL did, and did not do

`hal.dll` abstracted the **platform**, not the processor: interrupt
controllers, timers, bus and DMA access, firmware. That is why x86 alone had
several HALs (uniprocessor, MPS, ACPI) picked at install time. What depends
on the **CPU** (traps, context switch, page tables) lived in the kernel
itself, in per-architecture parts of `ke` and `mm`. NT was portable because
three things held together:

1. The kernel was written in C, with per-architecture code confined inside
   Ke and Mm.
2. The HAL absorbed the differences between boards.
3. Drivers never touched hardware directly. They went through
   `READ_REGISTER_ULONG`, `READ_PORT_UCHAR` and the I/O manager.

SavanXP already has the third and part of the first: `namespace arch`, no
inline assembly outside `arch/`, and port I/O fenced into
`arch::x86::port_io` (ARCH.md).

## Decisions

- **No separate HAL module.** NT shipped `hal.dll` as its own binary so one
  kernel image could run on different boards. SavanXP builds one image per
  target, so a compile-time boundary (headers and namespaces) does the same
  job without a loader contract. Later Windows versions folded much of the
  HAL back into the kernel for the same reason.
- **Two layers, not one.** The existing `arch` contract splits in two:
  - `arch`: the CPU. Traps and exceptions, the trap frame, context switch,
    FPU/SIMD state, the MMU backend, barriers, the cycle counter, user-access
    windows.
  - `platform`: the machine. Interrupt controller and interrupt routing,
    MSI message composition, the event timer, PCI configuration access,
    firmware discovery (ACPI today), the early console.

  `platform` is the real NT HAL equivalent. A PC is one `platform`; QEMU
  `virt` would be another on the same `arch`-level code as any aarch64
  machine.
- **The boundary is drawn against a second implementation, not guessed.**
  ARCH.md already refuses to neutralize the contract with only one
  implementation. P1-P5 move code behind interfaces where x86_64 itself has a
  second consumer (MSI for more drivers, ECAM, accessors that read better).
  The neutral shape of each interface is settled only in P7, when the
  aarch64 code exists to test it against.
- **64-bit targets only.** The i386 decision stands: the 32-bit kernel stays
  on `legacy`, for the reasons PENTIUM4_EVO_N1020V.md gives (the VMM forks, NX
  becomes conditional, Limine has no 32-bit protocol). An aarch64 kernel has
  none of those problems: 4-level, 48-bit, 4 KiB-granule paging with a
  separate kernel table root, an execute-never bit, and the same Limine
  protocol with an HHDM. P6 reopens "no architecture axis" for 64-bit
  targets only, as its own stated decision.
- **First target: aarch64 on QEMU `virt` with UEFI.** Everything there is
  either already supported or standard: virtio-pci over PCIe ECAM, GICv3,
  the ARM generic timer, PSCI to start cores, a PL011 UART, `qemu-xhci`.
  With edk2 firmware the machine exposes **ACPI** (MADT with GICC entries,
  MCFG, GTDT, SPCR), so uACPI stays the discovery path and Device Tree can
  wait for real hardware.

## What stands in the way today

Measured on `master` at `f75cd7ab`:

| Obstacle | Where | Layer |
| --- | --- | --- |
| x86 4-level page tables built by hand: PML4/PDPT/PD/PT walkers, PTE bits, `sign_extend_48`, canonical checks | `kernel/vmm.cpp` (1,822 lines) | `arch` (MMU backend) |
| A PC-shaped interrupt contract: ISA IRQ numbers in `register_irq_handler`/`enable_irq`, `InterruptEoi`, `local_apic_*`, IPIs by LAPIC id | `acpi.cpp`, `uacpi_glue.cpp`, `ps2.cpp`, `virtio_gpu.cpp`, `ioapic.cpp`, `iommu_selftest.cpp` | `platform` |
| x86 MSI encoding (`0xfee00000`, APIC id in the address) inside a driver header | `include/kernel/virtio_pci.hpp`, AMD-Vi tables | `platform` |
| PCI configuration through mechanism #1 (ports `0xcf8`/`0xcfc`) only | `kernel/pci.cpp` | `platform` |
| Trap frame with x86 registers, read by name in every dispatcher (`context->rax`, `rdi`, `r10`...) | `process.hpp`, `process.cpp`, `native_dispatch.cpp`, `posix_dispatch.cpp`, `posix_translation.cpp`, `linux_dispatch.cpp` | `arch` (trap frame accessors) |
| Syscall entry by `int $0x80` in the SDK | `include/sxn/sxn.h`, `include/abi/savanxp_native_abi.h`, the POSIX `libc.c` | SDK, per architecture |
| TSC read directly | `random.cpp`, `uacpi_glue.cpp` | `arch` (cycle counter, already named) |
| One target triple, `-mcmodel=kernel`, `-mno-red-zone`, ELF `EM_X86_64` only | `CMakeLists.txt`, `kernel/elf.cpp` | build |
| AMD-Vi as the only IOMMU | `amd_vi.cpp`, `iommu.cpp` | `platform` (SMMU elsewhere) |

What is **not** in the way: the VFS, SxFS, the object manager, the scheduler's
logic, the native dispatcher's semantics, the desktop, and every driver that
reaches its device by MMIO (virtio, AHCI, NVMe, xHCI, HD Audio). The 13 drivers
that use port I/O (ATA, PS/2, RTC, PIT, PC speaker, AC'97, RTL8139, RTL8168,
UHCI, the ACPI fixed registers, the Bochs display, the serial console,
mechanism #1) are PC-only by nature: they are simply not built for another
platform, which ARCH.md's `using namespace arch::x86::port_io` marker already
makes visible.

What is **x86 by nature** and no layer fixes:

- **The Win32 subsystem** runs PE32 code natively in compatibility mode
  (selector `0x2b`). On aarch64 it would need a binary translator, which is a
  project of its own. SSW is x86_64-only, and the subsystem's document says
  so if P6 lands.
- **The Linux subsystem** implements the x86_64 Linux ABI, entered by
  trapping `syscall` through #UD. An arm64 personality is possible (Linux
  arm64 uses the generic syscall table and `svc #0`), but it is a separate
  table and a separate musl profile, not a recompile.

## P1 — Interrupts by handle, not by vector

Drivers ask for *their device's interrupt* and get a handle; the platform
picks the mechanism (MSI-X, MSI, routed INTx) and composes the message.

- An interrupt-source API in the kernel: allocate for a PCI function, an
  ACPI GSI, or a legacy ISA line (PC platform only); install a handler; mask,
  unmask, end of interrupt. Vector numbers and `InterruptEoi` stay inside
  `arch/x86_64` and the PC platform.
- The MSI address and data move out of `virtio_pci.hpp` into the platform,
  where the IOMMU's interrupt remapping already has to see them.
- Fits [DRIVERS_ROADMAP.md](DRIVERS_ROADMAP.md)'s D3 interrupt claims: a claim
  records the handle, and a stop releases it.
- **x86_64 consumer:** MSI-X for NVMe, AHCI and xHCI, which poll today.
- **Gate:** `smoke`, `native-test`, `windowd-smoke`, the IOMMU self-test;
  `grep -rn 'local_apic_\|InterruptEoi' kernel subsystems` prints only the PC
  platform files.

## P2 — PCI configuration access as an operation

- Configuration reads and writes go through an access method chosen at boot:
  ECAM from the ACPI MCFG table when present, mechanism #1 otherwise.
- **x86_64 consumer:** the 4 KiB extended configuration space on q35 and
  real PCIe machines (extended capabilities, AER, ACS for the IOMMU).
- **Gate:** the core smokes on q35 with ECAM selected; one scenario forced
  onto mechanism #1 so the fallback does not rot.

## P3 — Time sources

- `random.cpp` and `uacpi_glue.cpp` call `arch::read_cycle_counter()` (or
  the timer API) instead of the TSC by name.
- The tick, one-shot and periodic event timer become a platform interface
  with the local APIC timer and the PIT as the PC's backends, which
  `arch/x86_64/timer.cpp` already chooses between.
- **Gate:** `smoke`, `power-smoke`; the boot log's `timer:` line is
  unchanged.

## P4 — The MMU backend

The largest phase. `kernel/vmm.cpp` splits into:

- **Generic VM**: regions, section views, the deferred-free and shootdown
  bookkeeping, user range validation against limits the backend reports.
- **A page-table backend** (`pmap` in BSD's terms): create and destroy an
  address space, map, unmap, change protection, walk, and translate a generic
  protection (read, write, execute, user, cache attribute) into PTE bits.
  It also owns the canonical-address rule and the top of the user half.
- **x86_64 consumer:** the code reads better and LA57 (5-level paging)
  becomes a backend change instead of a rewrite.
- **Gate:** `smoke`, `native-test`, `sxgui-smoke`, the loader set (`dltest`,
  `sonametest`, `sonametest-newer`, `ldfailtest`, `diamondtest`), the SMP
  smokes; no measurable slowdown in the page-fault and mapping paths on the
  existing benchmarks.

## P5 — Trap frame and syscall ABI

- Dispatchers read a call through accessors (`syscall_number`,
  `syscall_argument(n)`, `set_syscall_result`) and never name a register.
  `SavedContext` stays x86_64's own type.
- The native ABI document states the register convention per architecture.
  The SDK's `sxn_trap*` moves into a per-architecture header, which is where
  the POSIX `libc.c` stubs get theirs too.
- The 32-bit selectors and the Linux `syscall` trap stay x86_64-only, behind
  a compile-time capability, not an `#ifdef` in each dispatcher.
- **Gate:** `smoke`, `native-test`, `linux-hello`, `linux-restart`,
  `win32-hello`, `win32-window`, `win32-dialog`.

## P6 — The architecture axis (needs the decision)

- `SAVANXP_ARCH` selects the triple, the flags, the linker script, the ELF
  machine and the `arch/` and `platform/` directories, as `legacy` did for
  i386. x86_64 stays the default and its build output does not change.
- **One disk image per architecture** (`build/disk.img` stays x86_64's). The
  persistence rules apply to each image separately; binaries in `/bin` are
  never shared between them.
- The SDK, `tools/build-user.sh` and `ports/` build per architecture;
  external apps are rebuilt, not converted.
- PENTIUM4_EVO_N1020V.md's "no architecture axis" paragraph is rewritten in
  the same commit to say the axis exists for 64-bit targets and why i386
  still stays out.
- **Gate:** the x86_64 build is unchanged (same smokes, same persistence
  check with `./tools/verify_doom_persistence.sh`); an empty aarch64 build
  fails cleanly at the missing `arch` implementation.

## P7 — aarch64 bring-up on QEMU `virt`

- Limine entry at EL1, PL011 serial, exception vectors (`VBAR_EL1`), `svc`
  entry and return, context switch, FP/SIMD save and restore.
- The MMU backend: `TTBR1_EL1` for the kernel half and `TTBR0_EL1` per
  process, 4 KiB granule, 48-bit, `PXN`/`UXN` for W^X, ASIDs or a full flush
  to begin with, TLB shootdown by `TLBI ... IS` instead of IPIs.
- The `virt` platform: GICv3 distributor and redistributors, the generic
  timer, PSCI `CPU_ON` for the other cores, ECAM, MSI through the GIC ITS.
- This is where the neutral shape of the P1-P5 interfaces is settled, and
  ARCH.md is rewritten to describe two implementations.
- **Gate:** boots to the shell over serial on one core, then on several; the
  serial-only part of `smoke` passes on an aarch64 machine profile.

## P8 — Devices and desktop on `virt`

- virtio-blk, -net, -gpu, -input, -sound, -rng and `qemu-xhci` build
  unchanged, being MMIO drivers; the work is the interrupt handles from P1.
- **Gate:** `windowd-smoke`, `taskbar-smoke`, `sxgui-smoke` and a
  `./tools/shoot.sh` capture on aarch64.

## P9 — The test matrix

- Which scenarios run on which architecture, and which run in CI. On an
  x86_64 host aarch64 runs under TCG only, several times slower than KVM, so
  CI probably runs a short aarch64 subset on every push and the full set
  less often. This is decided with measurements, not in advance.

## Risks

- **The memory model.** x86 is TSO; aarch64 reorders loads and stores. The
  kernel has 161 uses of `volatile` outside vendor code, and lock-free paths
  (ring indexes, flags read without the big kernel lock) that may be correct
  only under TSO. P7 needs explicit acquire/release atomics there and MMIO
  accessors with the barriers ARM device memory needs.
- **`char` is unsigned on aarch64.** Code that relies on signed `char` breaks
  silently. Either build with `-fsigned-char` everywhere or fix the code;
  decide before P7, not during it.
- **DMA coherence.** QEMU `virt` is cache-coherent, as are x86 machines, so
  no driver does cache maintenance. Real ARM SoCs often are not. That is a
  reason to stay on `virt` and ACPI servers until a DMA API with sync
  operations exists.
- **Every kernel change has two architectures to answer for** once P6 lands.
  This is the same cost PENTIUM4_EVO_N1020V.md refuses for i386; it is
  acceptable here only because the VMM does not fork and the test harness is
  QEMU on both sides.

## Out of scope

- Real ARM hardware: Device Tree, non-coherent DMA, SMMU, vendor interrupt
  controllers and display engines.
- The Win32 subsystem on aarch64, and an arm64 Linux personality (possible
  later; see above).
- RISC-V and LoongArch. Limine supports both, and the P1-P5 boundary should
  serve them, but nothing here is checked against them.

## Open questions

- Does uACPI's glue need anything beyond the interrupt handle (P1) and the
  timer (P3) to run on aarch64?
- Is the GIC ITS enough for MSI on `virt`, or is GICv2m simpler for a first
  bring-up?
- How much slower is TCG aarch64 than TCG x86_64 for the existing smokes on
  the CI hosts?
