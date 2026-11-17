# i386 SMP bring-up

Milestone 6.2 adapts the SMP design of SavanXP v0.4.0, commit
`8ece07f4dae90812155a541951cbf8b3701bd92d`. Its first checkpoint implements
AP startup, private TSS/stacks and repeated directed IPIs. User processes
still run only on the BSP. `--smp N` exposes this bring-up stage, not parallel
user execution. Physical-machine validation remains reserved for v0.4.2.

## Discovery and protected-mode entry

The existing ACPI table walker supplies a checksummed MADT through the native
RAM alias. The topology parser validates table and entry bounds, enabled CPU
flags, duplicate IDs and LAPIC address overrides. Disabled online-capable CPUs
are not started: hotplug is separate work. The BSP is dense CPU index zero
even if firmware lists it later. The reference capacity is 32 CPUs; the parser
reports the enabled count separately from the bounded startup list. Native
xAPIC delivery requires IDs below 256 and device addresses below 4 GiB.
Malformed/unsupported topology falls back to the BSP without starting APs.

Limine supplied the reference APs already in long mode on usable stacks.
GRUB2 does not do that. The native adaptation copies a bounded real-mode
trampoline to physical `0x7000`, checks that E820 describes that page as usable
and that no Multiboot information/module overlaps it, then sends INIT followed
by SIPI. The page is already excluded from allocation with the reserved low
kernel range. If unavailable, APs are not started and the BSP keeps booting.

The trampoline installs flat protected-mode segments, enables PSE, loads the
shared kernel CR3 and enables paging/write protection. It clears INIT's CD/NW
bits so AP caches match the BSP's normal memory behavior. The stack table is
indexed by the AP's own initial APIC ID and populated before any launch. This
avoids a mutable per-launch mailbox: a late AP still finds its own stack.
Four contiguous allocator pages per AP provide its 16 KiB supervisor stack.
Stacks remain reserved even after a startup timeout, so a delayed AP cannot
enter on memory reused by a process or another CPU.

Each AP loads the already-built GDT/IDT, its own TSS selector, and x87/SSE
control state. Only then does it enable local interrupts and publish online.
AP code touches its own slot, registers and atomic ping count; it does not
print, allocate, mount devices or call the scheduler concurrently with the BSP.

## Local APIC and interrupts

The local xAPIC registers use an uncached shared supervisor MMIO view. The BSP
owns the single LINT0 ExtINT route for the existing PIC; AP LINT inputs and
timers remain masked. The PIC/PIT driver and its IRQ numbering are unchanged.
External vectors have separate saved-frame stubs and explicit local-APIC EOI;
vector 255 is a dedicated spurious gate and sends no EOI.

The startup selftest sends two fixed-vector IPIs to each AP, waiting on that
AP's acknowledgement counter. It also reads the AP's observed LAPIC ID and
hardware `STR` result. Repeated delivery checks EOI as well as the first ICR
write; checking an individual slot prevents a broadcast from passing as a
directed message. With four CPUs the log reports three distinct selectors
`0x30`, `0x38`, `0x40` and six acknowledged pings.

The reference virtio GPU can now arm MSI-X vector 49 on the BSP. Its existing
handler and deferred completion path remain; the PIC INTx fallback is retained
when a local APIC is unavailable. Device tests require actual IRQ notifications
in addition to the MSI-X boot marker. IOAPIC/GSI and AML routing are still
separate work; this does not claim x2APIC initialization or CPU hotplug.

## TSS layout and port permissions

A protected-mode TSS is 104 bytes. Its `esp0` is at offset 4 and the 16-bit I/O
bitmap offset is at byte 102. The old native structure appended extra selector
words and placed that field at byte 108; hardware instead read zero from byte
102 and treated part of the zeroed TSS as an I/O permission bitmap. An observed
control boot with the previous kernel allowed a ring-3 `IN` from port `0x80`.

The corrected layout has compile-time size/offset assertions and puts the
bitmap beyond the descriptor limit, denying user port access. The added
`multitest32 fault-io` case must terminate with #GP while its parent/sibling
survive and pages are reclaimed. The same diagnostic on the earlier kernel
completed the instruction and exited 99; on the corrected kernel it produced
#GP. The test therefore detects the original permission defect independently
of merely compiling a structure with the expected layout.

## Verification

```sh
./build.sh iso
./build.sh smoke smp-boot --smp 4
./build.sh smoke smp-boot --smp 32 --virtio
./build.sh smoke virtio-smoke --smp 4
./build.sh smoke sxgui-smoke --virtio --smp 4
./build.sh smoke power-smoke --virtio --smp 4
./build.sh smoke
./build.sh smoke memory
./build.sh smoke shared-memory
./build.sh test
```

The SMP harness compares software reports with QMP's actual CPU list and
register contexts. Each AP must be halted in ring 0, on a distinct reserved
supervisor stack, with its own 104-byte TSS and the expected selector. It
requires paging, write protection, PSE and FPU controls, and rejects disabled
caches. Artifacts retain serial logs, `cpus.json` and `registers.txt`.

Four-CPU and 32-CPU bring-up passed, including 62 targeted IPIs at the limit.
The virtio service/capture suite and all six SxGUI glyph/input captures passed
with four CPUs started and GPU MSI-X armed. ACPI reboot, shutdown and the
power-button SCI also passed in that profile. An isolated optimized build
without diagnostic applications passed four-CPU bring-up and desktop startup.
The 20 host tests include malformed MADT entries, duplicate/sparse IDs,
capacity truncation and a BSP listed after the retained CPU bound.

The default full smoke and the unchanged 32 MiB memory/shared-library gates
passed, including the new ring-3 I/O fault. Doom rebuild persistence and POSIX
rebuild/cold-boot persistence passed on isolated images. Extraction after the
normal rebuild matched the built diagnostic/UI binaries and preserved the
installed Doom/Ccleste executables and IWAD byte for byte.

## Next checkpoint: scheduling and synchronization

The APs are online but parked. `system_info.cpu_count` reports enabled firmware
CPUs and `cpu_online` reports startup acknowledgements; neither means that APs
run user processes. Boot logs explicitly report `scheduling=1` and the existing
PIT scheduler remains the only execution owner of mutable kernel state.

Before releasing APs to userland, restore the reference per-CPU scheduler
state and idle ownership, the global kernel lock at interrupt/syscall entries
and its release after leaving the outgoing stack. Add per-AP timer/reschedule
delivery, deferred termination of processes running remotely, and lazy shared
kernel-TLB synchronization at lock acquisition. Parked APs currently access
only permanent mappings; that is why they do not yet need a shootdown protocol.
The next regressions must prove actual concurrent user execution and safe
remote teardown, not infer them from CPU counts or successful startup IPIs.
