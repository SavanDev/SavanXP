# The IOMMU layer

Status: **phases M1 to M4 of
[DRIVER_FRAMEWORK_ROADMAP.md](../roadmaps/open/DRIVER_FRAMEWORK_ROADMAP.md#m1--discovery-and-report)
landed; M1, M2 and M4 verified on the Ryzen 5 1600.** The kernel reads the firmware's description of the IOMMU
at boot and says what it found (M1). With the `iommu` boot option it turns
AMD-Vi on with one identity domain shared by every device, and logs the
faults the units report (M2). A driver can move its device into a domain of
its own, where the device reaches only what the driver mapped; the RTL8139
does (M3). Interrupts are remapped as well: a device or I/O APIC raises only
the vectors the kernel gave it (M4). Without the option nothing changes what
the hardware does. M5 (VT-d) is next.

## Where it lives

| Code | What |
| --- | --- |
| `include/kernel/iommu_tables.hpp`, `kernel/iommu_tables.cpp` | `parse_ivrs` and `parse_dmar`: the two ACPI tables into one `iommu_tables::Report`. No access to the machine, so it runs in the host tests |
| `include/kernel/iommu.hpp`, `kernel/iommu.cpp` | `iommu::discover` and `iommu::start`, called in `kernel_main` right after `acpi::initialize`; `iommu::tick`, from the BSP's tick |
| `include/kernel/amd_vi_tables.hpp`, `kernel/amd_vi_tables.cpp` | AMD-Vi's memory formats: identity page tables, device table entries, commands, event log entries. No access to the machine |
| `include/kernel/amd_vi.hpp`, `kernel/amd_vi.cpp` | Taking the units over, the device table, the command and event rings, draining the event log, the domains |
| `include/kernel/iommu.hpp` (`iommu::Domain` and below) | The driver-facing domain API (M3) |
| `kernel/iommu_selftest.cpp` | `iommu::selftest`, boot option `iommu-selftest`, driving QEMU's `edu` device |
| `kernel/rtl8139.cpp` | The first driver in a domain of its own; boot option `rtl8139-unmapped-tx` |
| `kernel/ioapic.cpp`, `kernel/virtio_pci.cpp` | Give the IOMMU each vector before an I/O APIC entry or an MSI-X entry can raise it (M4) |
| `tests/host/iommu_tables_test.cpp`, `tests/host/amd_vi_tables_test.cpp` | `iommu-tables-test`, `amd-vi-tables-test` |
| `tools/smoke_catalog.py` | `iommu-amd`, `iommu-intel`, `iommu-none`, `iommu-amd-on`, `iommu-rtl8139-fault` |

## What the boot log says

```
iommu: AMD-Vi, 1 unit, interrupt remapping available, address width 40
iommu: unit 0 at 0xfed80000, segment 0, device 00:01.0, IVHD 0x11, 9 device entries, translation off
```

```
iommu: VT-d, 1 unit, interrupt remapping available, address width 48
iommu: unit 0 at 0xfed90000, segment 0, 8 device entries, translation off, interrupt remapping off
```

Or `iommu: none`. A table that breaks a rule is named with the rule, then
the machine is treated as having none:

```
iommu: IVRS refused: an IVHD range ends before it starts
iommu: none
```

Then one `iommu: reserved 0x...-0x... for ...` line per range the firmware
demands stay mapped. "Left on by the firmware" in place of "off" means the
unit was already translating when the kernel arrived; M2 has to take it over
from that state.

- **IVRS first.** When a machine has both tables (none should), the IVRS is
  used and the log says so.
- **Interrupt remapping available**, for AMD-Vi, means some IVHD names an
  I/O APIC: remapping is part of every AMD-Vi unit, but without that entry its
  interrupts cannot be remapped. For VT-d it means the DMAR's `INTR_REMAP`
  flag and every unit's extended capability bit.
- **Address width** is IVinfo's physical address size, or the DMAR's host
  address width.

## Translation: one identity domain (M2)

| Boot options | What happens |
| --- | --- |
| none | Discovery only: `iommu: translation stays off; boot with the iommu option to turn it on` |
| `iommu` | AMD-Vi translating: `iommu: AMD-Vi translating, 1 unit, one identity domain` |
| `noiommu` | Off, whatever else is given: `noiommu` always wins |

The option is opt-in until a target has been verified with it on; it becomes
the default per target after that. The ISO's **SavanXP (IOMMU on)** entry
passes it. On a VT-d machine the option logs that VT-d
is not driven yet (M5) and changes nothing.

- **One domain, every device.** The 65536 entries of the device table all
  point at the same four-level page tables, domain 1. Those tables map, to
  themselves, every memory map region the kernel or firmware uses as memory
  (usable, ACPI reclaimable and NVS, bootloader reclaimable, the kernel and its
  modules) and every reserved range the IVRS lists. Reserved and bad memory,
  the framebuffer and every MMIO range are not mapped: a DMA there faults.
  Drivers keep handing physical addresses to their devices unchanged.
- **This is not isolation.** A device can still reach all of RAM, the IOMMU's
  own tables included. What the domain stops is DMA to addresses that are not
  memory, and what it gives is a report when that happens. Isolation is a
  domain of the device's own (below).
- **Interrupts** are remapped too unless `nointremap` is given (M4, below).
- **Taking a unit over.** Whatever the firmware left running is stopped first
  (the control register written to zero, then a wait for both rings to stop),
  the exclusion range is cleared (the IVMD ranges that asked for one are in the
  domain instead), the rings and the device table are installed, and only then
  is translation turned on. The IVHD flags choose HtTunEn, PassPW, ResPassPW
  and Isoc; Coherent is always on.
- **Flushing.** `INVALIDATE_IOMMU_ALL` where EFR advertises it, then
  `INVALIDATE_DEVTAB_ENTRY` for every requester ID the IVHD lists (all 65536
  for a unit that covers every device or has no IASup), then the domain's
  pages, then a completion wait. The per-device invalidations are needed by
  QEMU, which moves a device onto its translated address space only when that
  device's own entry is invalidated; on hardware they are redundant and cheap.
- **Memory.** The device table is 2 MiB, physically contiguous. Each unit gets
  a 4 KiB command ring and a 4 KiB event log (256 entries each). The identity
  domain uses 2 MiB leaves wherever a whole aligned 2 MiB is memory: 15 page
  tables for a 1 GiB QEMU machine.
- **Faults.** The BSP's tick drains every unit's event log each 100 ms, under
  the kernel lock, and logs at most 16 events per drain, then a count of the
  rest: `iommu: I/O page fault from 00:05.0 at 0x..., write, domain 1`. The
  domain printed is the one the kernel had put the device in when the event
  was read. An overflowed log is reported, emptied and restarted. No
  interrupt is used.

## Domains of their own (M3)

The API in `iommu.hpp`, under the kernel lock like the rest:

| Call | What |
| --- | --- |
| `create_domain(handler, context, domain)` | A new domain with nothing mapped. False when translation is off or all 30 are in use; the driver then hands physical addresses to its device, as before |
| `map(domain, device_address, physical, bytes, access)` | Device addresses to physical pages, all page aligned, with `kDeviceReads` and/or `kDeviceWrites`. Refused when any page of the range is mapped already |
| `unmap(domain, device_address, bytes)` | Returns once no unit can use the old translation |
| `attach(domain, pci_device)` | Moves the function's whole isolation group into the domain, or nothing |
| `block(pci_device)` | Moves the group to the blocked domain |
| `destroy_domain(domain)` | Every device still in it goes to the blocked domain; then its page tables are freed |

- **Device addresses are the driver's choice.** They need not be the
  physical ones, and a device that can only address 32 bits can be given low
  ones for high pages. The RTL8139 puts its buffers at 2 GiB, which is not
  memory on the QEMU boards: a card left in the identity domain by mistake
  faults there rather than writing somewhere.
- **Access is per mapping.** The RTL8139's receive ring is writable by the
  card only, its transmit buffers readable only.
- **Isolation groups.** The requester IDs the unit cannot tell apart share a
  domain. On AMD-Vi they come from the IVRS's alias entries: a device in an
  aliased range reaches the unit as the alias, and so does every other
  device aliased to it (`iommu_tables::isolation_group`). Without an alias the
  group is the function alone. A group is attached whole or not at all, and
  attaching is refused when a member already is in another driver's domain;
  leaving the identity or the blocked domain is always allowed.
- **The blocked domain** has an empty page table: every DMA from a device in
  it faults and is logged, and nobody else is told. A device leaves a domain
  of its own only for the blocked one, never back to the identity domain: an
  unowned, failed or removed function cannot DMA.
- **Faults reach the owner.** After the drain that read them is logged, each
  I/O page fault is handed to the fault handler of the domain the device was
  in, if it is still there. The handler decides what happens; the RTL8139
  stops the card, clears its bus mastering and blocks it, and the rest of the
  system carries on without a network.
- **Domains not yet moved.** Every device a driver has not moved stays in the
  identity domain of M2. Moving the remaining TKDs, and blocking functions
  nobody drives, comes with the framework's DMA calls (F1).
- **Flushing.** Changing a device's domain invalidates its device table
  entries and every domain it was in or is in now; `map` and `unmap`
  invalidate the domain's pages (a unit may cache entries it found not
  present). Each waits for a completion before returning.
- **Domain IDs**: the identity domain is 1, the blocked one 2, drivers' from
  3. The page tables of a driver's domain use 4 KiB leaves only.

## Interrupt remapping (M4)

With `iommu`, AMD-Vi remaps interrupts as well, unless the boot option
`nointremap` is given or the tables do not allow it. The boot log says which:

```
iommu: I/O APIC 0 reaches the unit as 00:14.0
iommu: interrupts remapped; a device raises only the vectors it was given
```

- **Every device starts with no entry.** Each device table entry points at
  an empty remapping table, so any interrupt message a device sends is
  refused until the kernel gives it that vector. Hardware logs a refusal as
  an I/O page fault with the interrupt flag, which the drain prints as
  `iommu: interrupt from 00:05.0 refused, address 0xfee00000`.
- **One table per isolation group, indexed by vector.** AMD-Vi looks an
  interrupt up by bits 10:0 of its MSI data, and the kernel sends everything
  with fixed delivery, so that index is the vector. A table of 256 entries
  leaves every message as it was: I/O APIC redirection entries and MSI
  messages are programmed exactly as without remapping, and a level line's
  EOI still names the vector its I/O APIC entry holds.
- **Who gives entries.** `ioapic::route_gsi` gives the I/O APIC the vector
  before it unmasks the entry (a refused level interrupt would leave the
  entry's remote IRR set forever), and `ioapic::spread` moves it to its core.
  virtio-pci gives its MSI-X vector before it enables MSI-X. Drivers call
  `iommu::allow_interrupt(device, vector, apic_id)`.
- **I/O APICs** are found in the MADT and must each be named by an IVHD
  special entry, which gives the requester ID their interrupts arrive under.
  One the IVRS does not name keeps remapping off:
  `iommu: interrupt remapping stays off: I/O APIC 2 is not in the IVRS`.
- **The pass-through bits** (INIT, ExtINT, NMI, LINT0, LINT1) are what the
  IVRS's DTE setting byte asks for each device.
- **Entries are 32 bits**, which name the destination APIC in 8 bits: a core
  with an APIC ID above 254 still gets no device interrupts. The 128-bit
  entries that name 32 bits need the unit's x2APIC and guest-virtual-APIC
  features, which nothing here turns on yet.
- **Remapping and DMA are separate.** A device in the blocked domain still
  raises the interrupts it was given; one whose entries should go must have
  them taken away (not needed by any caller yet).

### QEMU's amd-iommu, as measured (QEMU 11.1)

- It translates emulated devices' DMA only with `dma-remap=on`
  (`run_qemu.py --iommu amd` passes it).
- It writes zero in the address field of its I/O page fault events; its own
  trace (`-trace amdvi_page_fault`) has the right address.
- It faults a DMA in 4-byte pieces, so one 4 KiB DMA outside the domain is
  1024 events, four times what the log holds. With the event log interrupt off
  it drops the rest without setting the overflow bit, which hardware sets
  regardless.
- It leaves the RW flag of its I/O page fault events clear, so every fault
  it reports reads as a read.
- It refuses an interrupt without an entry but writes no event for it (a
  TODO in its source); it prints `failed to remap interrupt from devid 0x28`
  once on its own standard error instead.
- It drops the trigger mode of every interrupt it remaps: the message reaches
  the local APIC as edge-triggered, the APIC broadcasts no EOI, and a
  level-triggered I/O APIC line keeps its remote IRR set and never interrupts
  again. The kernel covers this in general: after the local APIC's EOI of a
  vector that arrived edge-triggered (its TMR bit clear), the I/O APIC
  entries of level lines on that vector get an EOI of their own, through the
  I/O APIC's EOI register (version 0x20 and later) or by turning the entry
  edge-triggered and back. Hardware that keeps the trigger mode never takes
  that path.
- It caches a translation by device and page with its permissions, and
  refuses an access the cached permissions forbid without an event. A write
  to a read-only page the device has already read is stopped, but not
  reported. Hardware walks the tables again and reports it.

### VirtualBox's AMD IOMMU, as measured (VirtualBox 7.2.22)

A VM with `--chipset ich9 --iommu amd` has an AMD-Vi unit at `0xfeb80000`,
PCI 00:00.0 (`1022:c0de`), its I/O APIC as 00:14.0. M1, M2 and M4 run there:
`AMD-Vi translating`, `interrupts remapped`, the desktop up, the AHCI root disk
and AC97 audio without a fault, the network online. The self-test and the M3
scenario do not: VirtualBox has neither `edu` nor an RTL8139.

- **Its EFI leaves the register window off.** The unit's registers answer
  only once the base address register of its PCI capability holds the enable
  bit. VirtualBox's BIOS sets it; its EFI does not, so discovery logs
  `registers not readable`. With `iommu`, the kernel writes the IVRS base into
  the capability and sets the bit itself, when the bit is clear and the base
  register is zero or already that address:
  `iommu: unit 0: firmware left its register window off; turned on at
  0xfeb80000, registers readable`. Discovery alone never writes.
- **It keeps the trigger mode** of a remapped interrupt (its source copies
  the original message and replaces only destination, vector and delivery
  mode), so the directed EOI that covers QEMU is never taken there.
- **Its I/O page fault events carry the address and the RW flag**, and its
  source reports a refused interrupt as an I/O page fault with the interrupt
  bit. Nothing on that board raises one, so it has not been seen.
- **Its virtio-net reads and writes device address 0** when the driver sets
  DRIVER_OK: it reads and rewrites the used ring flags of every queue it has,
  the control queue included, without checking that the queue was enabled,
  and a queue the driver never set up sits at 0
  ([VIRTIO.md](VIRTIO.md#virtualboxs-virtio-net-touches-queues-it-was-not-given)).
  Under BIOS, page 0 is not memory in the memory map, so the identity domain
  stops it: `iommu: I/O page fault from 00:03.0 at 0x0, read, domain 1`, then
  the same for a write. Under EFI, page 0 is usable memory, the identity
  domain maps it and the access goes through silently. Either way it is
  harmless here: the kernel never allocates below 1 MiB.

To run it: copy the ISO tree, add `iommu` to the first entry's `cmdline` in
both `limine.conf` and `boot/limine/limine.conf` (UEFI reads the second), rebuild it
with the `xorriso` and `limine bios-install` steps of the `iso` target, and
attach a VDI converted from a copy of `build/disk.img` to an AHCI port. The
serial port goes to a file (`--uart-mode1 file`).

## Rules the parsers enforce

Firmware is a hostile input here, as in the rest of the ACPI walk
([KERNEL_SECURITY.md](KERNEL_SECURITY.md#acpi-table-walk)). The table comes
from `acpi::find_table`, already bounded to its own length, and is **refused
whole** when any of this fails. A refusal leaves the report empty: a half-read
IOMMU description is worse than none, because M2 would build domains from it.

- Every entry's length is checked against what is left of its container
  before anything in it is read: IVRS entries and DMAR structures against the
  table, IVHD device entries against their IVHD, device scopes against their
  structure. A zero length is refused, not stepped on forever.
- **IVHD device entries** below type 0x80 are 4 or 8 bytes by their top two
  bits; 0xf0 declares its own length. Any other type cannot be stepped over
  safely and is refused. A range start must be followed by its end, and the
  end may not come before the start.
- **One IOMMU, several IVHDs.** Firmware describes the same unit once per IVHD
  type so that older software finds one it knows. The highest type wins, as
  in Linux; the others are still checked.
- **Register bases** must be non-zero, aligned (16 KiB for AMD-Vi, a page for
  VT-d) and below 2^52.
- **Reserved ranges** (IVMD, RMRR) may not wrap the address space and are
  widened to whole pages. Misaligned ones exist on real firmware; widening
  them is what any mapping would do anyway.
- **DMAR device scopes** need at least one hop, a whole number of hops, and
  devices and functions in range.
- **Capacity**: 16 units, 512 device entries, 64 reserved ranges, 8 hops per
  path. A table that needs more is refused with a reason that says so, rather
  than kept in part. Raise the constants in `iommu_tables.hpp` if a machine
  needs it.

Before a unit's registers are read, its base must not lie inside memory the
memory map gives the kernel (usable, reclaimable, the kernel's own, the
framebuffer). Only the first page is mapped, uncached. A register that reads
all ones means nothing answers there; the unit is logged with
`registers not readable`. With `iommu`, an AMD-Vi unit in that state gets its
window turned on from its PCI capability (VirtualBox's EFI, above).

## Testing

- `iommu-tables-test` places every table so that it ends where a page with no
  access begins: a read one byte past a bad length is a crash, not a pass. It
  covers a QEMU-like IVRS with every entry kind, a DMAR with every structure
  the kernel reads, every prefix of both, and one table per refusal.
- `amd-vi-tables-test` builds identity tables from a fake allocator and walks
  them back the way the unit does: every page of a mapped range translates to
  itself, the pages around it fault, 2 MiB leaves are used, and mapping twice
  allocates nothing. It also checks the device table entry, the command
  encodings and an I/O page fault event as QEMU writes it.
- `./build.sh smoke iommu-amd` and `iommu-intel` boot with QEMU's `amd-iommu`
  or `intel-iommu` (`intremap=on`) and wait for the boot log line;
  `iommu-none` boots the usual board. `tools/run_qemu.py --iommu amd|intel`
  does the same for an interactive run.
- `./build.sh smoke iommu-amd-on` boots with `iommu iommu-selftest`. The board
  gets QEMU's `edu` device whenever it has an IOMMU, last, so no other device
  changes slot; its DMA mask is widened so it reaches any address. The
  self-test copies a page through edu and back (it must arrive), sends 4 bytes
  to 64 GiB, outside the domain (one fault, from edu, must be reported), floods
  the log with a 4 KiB DMA there, checks the next fault is still reported, and
  copies a page again. Then (M3) it gives edu a domain of its own at 3 GiB:
  a page copied in and out through it must arrive; edu's physical pages, a
  read-only page (one edu never touched, see the QEMU notes) and an unmapped
  one must fault and reach the domain's owner; destroying the domain must
  leave edu blocked, faulting with no owner told, with every page table
  freed. Then (M4) it programs edu's MSI to vector 49 on this core: raised
  with no entry, it must not reach the core; once `iommu::allow_interrupt`
  gave it, it must. The boot runs with interrupts off here, so arrival is read
  from the local APIC's IRR. It ends with `iommu: selftest passed`.
- `./build.sh smoke iommu-rtl8139-fault` boots with `iommu
  rtl8139-unmapped-tx`: the RTL8139 in its own domain with transmit buffer 0
  left out of it. The first frame faults; the scenario waits for
  `rtl8139: a DMA read outside the card's domain; the card is stopped and
  blocked`. netsmoke fails after that, with no network, which is expected.
- Any other scenario runs with translation on through
  `./build.sh --iommu amd --boot-options iommu smoke NAME` (or
  `SAVANXP_IOMMU=amd SAVANXP_BOOT_OPTIONS=iommu`); its serial log must then
  hold `AMD-Vi translating`, `interrupts remapped` and no I/O page fault.
  Whether interrupts really went through the remapping tables shows in
  QEMU's own trace: `SAVANXP_QEMU` names a wrapper script that adds
  `-D FILE -trace amdvi_ir_irte_val -trace 'ioapic_*'`, and every remapped
  interrupt is one `amdvi_ir_irte_val` line, every level line's EOI one
  `ioapic_clear_remote_irr`.
- VirtualBox, with the steps above, is the second emulator: it reports
  fault addresses that QEMU zeroes, and it runs devices QEMU's IOMMU never
  sees, such as its virtio-net.
- Under KVM the emulated IOMMU's interrupt remapping needs the split irqchip,
  and `run_qemu.py` adds `kernel-irqchip=split` for it. Only the TCG path has
  been run so far.

## Pending

- **One real machine has reported.** The Ryzen ran M1, M2 and M4 on
  2026-11-20 with every device in use and no fault
  ([RYZEN5_1600_VEGA56.md](../hardware/RYZEN5_1600_VEGA56.md#the-iommu-runs-2026-11-20)).
  The other verified targets still owe their M1 report with the firmware's
  IOMMU setting on. M3 on hardware needs a card with a domain-aware driver
  in it; the Ryzen's RTL8168 is not one yet.
- **Turning translation on costs about 330 ms** on the Ryzen: one
  `INVALIDATE_DEVTAB_ENTRY` for each of the ~65000 requester IDs its IVHD
  covers. Only QEMU needs them; a unit with `INVALIDATE_IOMMU_ALL` could do
  without.
- **Isolation groups from the PCI topology.** Only the IVRS's alias entries
  are read. A conventional PCI device behind a PCIe-to-PCI bridge that the
  firmware did not describe with an alias, and multi-function devices without
  ACS, are not grouped by walking the bus yet.
- The KVM path (`kernel-irqchip=split`) has not been run.
- **A refused interrupt being reported** has not been seen: QEMU does not
  write the event, so the drain's line for it has only run in the host test.
  VirtualBox writes it, but has no device the self-test can drive.
- **APIC IDs above 254** still get no device interrupts (32-bit entries);
  128-bit entries are the remedy.
- The event log is polled. Its interrupt (the unit's MSI) waits for a consumer
  that needs faults faster than 100 ms.
- The DMAR structures after RMRR (ATSR, RHSA, ANDD, SATC) are length-checked
  and skipped. ANDD names the ACPI devices a scope of type 5 refers to; it is
  read when a consumer needs it.
