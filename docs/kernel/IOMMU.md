# The IOMMU layer

Status: **phase M1 of
[DRIVER_FRAMEWORK_ROADMAP.md](../roadmaps/open/DRIVER_FRAMEWORK_ROADMAP.md#m1--discovery-and-report)
landed: discovery and report only.** The kernel reads the firmware's
description of the IOMMU at boot, reads each unit's registers without writing
them, and says what it found. Nothing changes what the hardware does. M2
(AMD-Vi on, one identity domain) is next.

## Where it lives

| Code | What |
| --- | --- |
| `include/kernel/iommu_tables.hpp`, `kernel/iommu_tables.cpp` | `parse_ivrs` and `parse_dmar`: the two ACPI tables into one `iommu_tables::Report`. No access to the machine, so it runs in the host tests |
| `include/kernel/iommu.hpp`, `kernel/iommu.cpp` | `iommu::discover`, called in `kernel_main` right after `acpi::initialize`, and `iommu::report()` for the phases that follow |
| `tests/host/iommu_tables_test.cpp` | `iommu-tables-test` |
| `tools/smoke_catalog.py` | `iommu-amd`, `iommu-intel`, `iommu-none` |

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
`registers not readable`.

## Testing

- `iommu-tables-test` places every table so that it ends where a page with no
  access begins: a read one byte past a bad length is a crash, not a pass. It
  covers a QEMU-like IVRS with every entry kind, a DMAR with every structure
  the kernel reads, every prefix of both, and one table per refusal.
- `./build.sh smoke iommu-amd` and `iommu-intel` boot with QEMU's `amd-iommu`
  or `intel-iommu` (`intremap=on`) and wait for the boot log line;
  `iommu-none` boots the usual board. `tools/run_qemu.py --iommu amd|intel`
  does the same for an interactive run.
- Under KVM the emulated IOMMU's interrupt remapping needs the split irqchip,
  and `run_qemu.py` adds `kernel-irqchip=split` for it. Only the TCG path has
  been run so far.

## Pending

- **No real machine has reported yet.** M1 is done when each verified
  target's hardware page records what it reports with the firmware's IOMMU
  setting on; the Ryzen is the first one that matters.
- The DMAR structures after RMRR (ATSR, RHSA, ANDD, SATC) are length-checked
  and skipped. ANDD names the ACPI devices a scope of type 5 refers to; it is
  read when a consumer needs it.
