# The architecture boundary: what is x86 and where it lives

Everything that depends on the processor lives in two places:

| Where | What |
|---|---|
| `arch/x86_64/` | The sources: boot entry (`entry.cpp`), descriptor tables, interrupts, the PIC and the local APIC, the FPU (`cpu_init.cpp`), the timer backends (`timer.cpp`), AP start-up (`smp.cpp`), context switch and syscall entry (`context.S`), and `memcpy`/`memset` on the string instructions (`memory.cpp`) |
| `include/arch/x86_64/` | What the rest of the kernel may call: `cpu.hpp` (namespace `arch::x86_64`) and `io.hpp` (namespace `arch::x86_64::port_io`) |

## The rule

**No inline assembly outside `arch/` and `include/arch/`.** A driver or a
kernel subsystem that needs something from the processor calls it by name:

| Need | Call |
|---|---|
| Spin-wait hint | `arch::x86_64::cpu_relax()` |
| Order device-ring writes before the doorbell (virtio, AHCI) | `memory_barrier()` |
| Order write-combining stores (a framebuffer) | `store_barrier()` |
| Short section with interrupts off, from any context | `save_and_disable_interrupts()` / `restore_interrupts()` |
| Switch or read the page table root, drop one translation | `load_page_table_root()`, `read_page_table_root()`, `invalidate_page()`, `flush_tlb()` |
| Model-specific registers, cycle counter | `read_msr()`, `write_msr()`, `read_timestamp_counter()` |
| Port I/O | `out8/16/32`, `in8/16/32`, `io_wait()` from `io.hpp` |

The check is mechanical and must print nothing:

```bash
grep -rn 'asm' kernel subsystems/*/kernel include/kernel
```

A compiler-only barrier is not assembly: it is
`__atomic_signal_fence(__ATOMIC_SEQ_CST)` (the AC'97 driver uses it).

## Port I/O marks a PC-only driver

Port I/O is an x86 address space of its own. A driver pulls it in with
`using namespace arch::x86_64::port_io;` inside its anonymous namespace, and
that line is also the marker: the driver exists only on PC hardware (ATA,
PS/2, RTC, PIT, PC speaker, PCI configuration mechanism #1, the ACPI fixed
registers, AC'97, RTL8139, the Bochs display). Drivers that reach their device
through memory-mapped registers (AHCI, virtio, the local APIC) do not include
it.

## What a second architecture changes

The `legacy` branch's i386 port ([REAL_HARDWARE.md](REAL_HARDWARE.md#route-a-port-the-system-to-i386))
is the case this boundary is drawn for:

- `io.hpp` is valid as it is: the port instructions are the same in 32-bit
  protected mode.
- `cpu.hpp` is the contract. An `arch/x86` provides the same functions;
  `memory_barrier` needs SSE2 there (`mfence`), which every CPU the port
  targets has.
- `memory.cpp` is rewritten with `rep movsd`.
- What does **not** follow from this boundary: the code still calls
  `arch::x86_64::` by that name, and `vmm.cpp` still builds 4-level page
  tables itself. Renaming the namespace to a neutral `arch::` is mechanical
  once a second architecture exists; the page tables are the VMM rewrite in
  REAL_HARDWARE.md.
