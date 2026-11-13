# The architecture boundary: what is x86 and where it lives

The kernel is written against one CPU contract, the functions in
**namespace `arch`**, and every architecture implements it. Code outside
`arch/` includes `arch/cpu.hpp`, which selects the implementation for the
target being built:

| Where | What |
|---|---|
| `include/arch/cpu.hpp` | The only header generic code includes. It picks `arch/x86_64/cpu.hpp` when building for x86_64, and fails the build for a target with no implementation |
| `include/arch/x86_64/cpu.hpp` | x86_64's declaration of the contract, plus its inline single instructions |
| `arch/x86_64/` | The sources: boot entry (`entry.cpp`), descriptor tables, interrupts, the PIC and the local APIC, the FPU (`cpu_init.cpp`), the timer backends (`timer.cpp`), AP start-up (`smp.cpp`), context switch and syscall entry (`context.S`), and `memcpy`/`memset` on the string instructions (`memory.cpp`) |
| `include/arch/x86/io.hpp` | Port I/O, namespace `arch::x86::port_io`. It belongs to the whole x86 family: the instructions are the same in 32-bit protected mode |

## The rule

The shared device/resource lifecycle, Plug and Play and the planned SUN Device
Manager are tracked in
[`DRIVERS_ROADMAP.md`](../roadmaps/open/DRIVERS_ROADMAP.md). They build on this
architecture boundary and the existing class registries.

**No inline assembly outside `arch/` and `include/arch/`.** A driver or a
kernel subsystem that needs something from the processor calls it by name:

| Need | Call |
|---|---|
| Spin-wait hint | `arch::cpu_relax()` |
| Order device-ring writes before the doorbell (virtio, AHCI) | `arch::memory_barrier()` |
| Order write-combining stores (a framebuffer) | `arch::store_barrier()` |
| Short section with interrupts off, from any context | `arch::save_and_disable_interrupts()` / `arch::restore_interrupts()` |
| Switch or read the page table root, drop one translation | `arch::load_page_table_root()`, `read_page_table_root()`, `invalidate_page()`, `flush_tlb()` |
| Model-specific registers, cycle counter | `arch::read_msr()`, `write_msr()`, `read_timestamp_counter()` |
| Touch user memory in place (SMAP window) | `arch::user_access_begin()` / `user_access_end()`, or a scoped `arch::UserAccess` |
| Port I/O | `out8/16/32`, `in8/16/32`, `io_wait()` from `arch/x86/io.hpp` |

The check is mechanical and must print nothing:

```bash
grep -rn 'asm' kernel subsystems/*/kernel include/kernel
grep -rn '"arch/x86_64/' kernel subsystems/*/kernel include/kernel
```

A compiler-only barrier is not assembly: it is
`__atomic_signal_fence(__ATOMIC_SEQ_CST)` (the AC'97 driver uses it).

`context.S` defines `arch::resume_context` under its C++ mangled name
(`_ZN4arch14resume_contextEPN7process12SavedContextEm`). Renaming the namespace
or changing that signature means changing the label too, or the link fails.
The entry stubs read `arch::g_smap_enabled` the same way
(`_ZN4arch14g_smap_enabledE`).

## The contract is x86-shaped, and says so

`namespace arch` is neutral in name, not yet in content. Part of it is the x86
family's: `InterruptEoi::pic`, `local_apic_*`, `register_irq_handler` with ISA
IRQ numbers, `send_ipi` by LAPIC id, the TSS selector behind `cpu_index()`.
An i386 port implements those as they are, since it has the same PIC and APIC.
A non-x86 port would have to redraw that part of the contract, and that is the
right time to do it: drawing it now, with one implementation, would be
guessing.

## Segments

Long mode leaves segmentation almost empty, but a few selectors are still
part of the contract with user code. The GDT is one table shared by every
core (`arch/x86_64/cpu_init.cpp`):

| Selector | Entry | Use |
| --- | --- | --- |
| `0x08` | Kernel code | 64-bit, ring 0 |
| `0x10` | Kernel data | ring 0 |
| `0x1b` | User data | Flat, G and D/B set: SS of every user thread, and DS/ES/SS of 32-bit code |
| `0x23` | User code | 64-bit, ring 3 |
| `0x2b` | User 32-bit code | L clear, D set, ring 3: a native program's 32-bit code |
| `0x30` + 16 *n* | TSS of core *n* | `cpu_index()` reads it back with `str` |

The constants are `arch::kUserDataSelector`, `kUserCodeSelector`,
`kUserCode32Selector` and `kTssSelectorBase` (`include/arch/x86_64/cpu.hpp`).
There is no LDT, no 16-bit segment, no call gate and no `syscall`/`sysenter`.

**32-bit user code.** Any native program may far-return or far-jump to `0x2b`
and run 32-bit code below 4 GiB, and come back through `0x23`. It is how the
Win32 subsystem's `winhost` runs a PE32 guest
([WIN32_SUBSYSTEM_ROADMAP.md](../roadmaps/open/WIN32_SUBSYSTEM_ROADMAP.md#k1--32-bit-user-segments)).
The kernel needs nothing else for it: interrupts and exceptions from
compatibility mode arrive through the same long-mode gates, "from user mode"
is still `cs & 3`, `iretq` returns to whichever mode the frame names, and a
fault in 32-bit code ends its process like any other user exception.

**The system call ABI stays 64-bit.** `int 0x80` from code segment `0x2b`
returns `-ENOSYS` before any subsystem sees it: the upper halves of the
registers are undefined after 32-bit code, so the kernel reads none of them.

**Data selectors are per-thread state.** A kernel entry does not touch DS, ES,
FS or GS, and 64-bit code ignores them except for the FS and GS bases. 32-bit
code does not: it needs DS and ES non-null and its own FS. The scheduler saves
the four selectors of the thread it switches away from and loads the
incoming thread's, then its FS and GS bases (`IA32_FS_BASE`,
`IA32_GS_BASE`), in that order, because loading a selector resets its base. A
new thread and an exec start with null selectors; fork copies them.

Under QEMU's TCG a null DS in compatibility mode does not fault, so a lost
selector passes unnoticed there; `nativetest`'s 32-bit checks catch one only
on KVM or hardware.

## Port I/O marks a PC-only driver

A driver pulls port I/O in with `using namespace arch::x86::port_io;` inside
its anonymous namespace, and that line is also the marker: the driver exists
only on PC hardware (ATA, PS/2, RTC, PIT, PC speaker, PCI configuration
mechanism #1, the ACPI fixed registers, AC'97, RTL8139, the Bochs display).
Drivers that reach their device through memory-mapped registers (AHCI, virtio,
the local APIC) do not include it.

## What a second architecture changes

The `legacy` branch's i386 port ([PENTIUM4_EVO_N1020V.md](../hardware/PENTIUM4_EVO_N1020V.md#the-i386-port-on-legacy))
is the case this boundary is drawn for:

- An `include/arch/x86/cpu.hpp` and an `arch/x86/` that implement
  `namespace arch`, and a branch in `arch/cpu.hpp` that selects them.
  `memory_barrier` needs SSE2 there (`mfence`), which every CPU the port
  targets has.
- `arch/x86/io.hpp` as it is.
- `memory.cpp` rewritten with `rep movsd`.
- What does **not** follow from this boundary: `vmm.cpp` still builds 4-level
  page tables itself. That is the VMM package in PENTIUM4_EVO_N1020V.md.
