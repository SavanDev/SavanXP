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

## The contract is x86-shaped, and says so

`namespace arch` is neutral in name, not yet in content. Part of it is the x86
family's: `InterruptEoi::pic`, `local_apic_*`, `register_irq_handler` with ISA
IRQ numbers, `send_ipi` by LAPIC id, the TSS selector behind `cpu_index()`.
An i386 port implements those as they are, since it has the same PIC and APIC.
A non-x86 port would have to redraw that part of the contract, and that is the
right time to do it: drawing it now, with one implementation, would be
guessing.

## Port I/O marks a PC-only driver

A driver pulls port I/O in with `using namespace arch::x86::port_io;` inside
its anonymous namespace, and that line is also the marker: the driver exists
only on PC hardware (ATA, PS/2, RTC, PIT, PC speaker, PCI configuration
mechanism #1, the ACPI fixed registers, AC'97, RTL8139, the Bochs display).
Drivers that reach their device through memory-mapped registers (AHCI, virtio,
the local APIC) do not include it.

## What a second architecture changes

The `legacy` branch's i386 port ([REAL_HARDWARE.md](REAL_HARDWARE.md#route-a-port-the-system-to-i386))
is the case this boundary is drawn for:

- An `include/arch/x86/cpu.hpp` and an `arch/x86/` that implement
  `namespace arch`, and a branch in `arch/cpu.hpp` that selects them.
  `memory_barrier` needs SSE2 there (`mfence`), which every CPU the port
  targets has.
- `arch/x86/io.hpp` as it is.
- `memory.cpp` rewritten with `rep movsd`.
- What does **not** follow from this boundary: `vmm.cpp` still builds 4-level
  page tables itself. That is the VMM rewrite in REAL_HARDWARE.md.
