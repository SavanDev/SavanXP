# Hardware inventory

Status: **D1 of [DRIVERS_ROADMAP.md](../roadmaps/open/DRIVERS_ROADMAP.md),
first cut (2026-11-24).** The kernel keeps one list of the hardware it found
and of the driver using each piece, and hands it out whole through
`SXN_SYS_DEVICE_SNAPSHOT` (SUN) and `SAVANXP_SYS_DEVICE_SNAPSHOT` (POSIX).
Its first consumer is [Device Manager](../userland/DEVICE_MANAGER.md). It
lists, and since D3's first slice it also decides who owns a PCI function
([Ownership](#ownership)); lifecycle and removal policy are the rest of D3 to D6.

## What is listed

| Bus (`sxn_device_info.bus`) | What | Source |
| --- | --- | --- |
| `ROOT` | The computer, always the first record, parent of everything else | `inventory::initialize()` |
| `PCI` | Every function of the boot scan, bridges included, whether a driver took it or not | `pci::` table, ids given before any driver starts |
| `PLATFORM` | What a driver found off any bus: the PS/2 keyboard and mouse, the firmware framebuffer (when it is the display backend), the LiveCD's RAM disk | `inventory::platform_add()` from the driver |
| `USB` | Every USB device present, hubs included | `usb::describe()`, from what `usb::attach` recorded |
| `USB_INTERFACE` | Each interface (alternate setting 0) offered to the class drivers, and which one took it | the same |

A PCI function hangs from the bridge whose secondary bus is its bus, or from
the computer on bus 0 or when no bridge leads to it. A USB device hangs from
its hub, or from its host controller's PCI function on a root port. An
interface hangs from its device. Bridges caught in a loop (a secondary bus
pointing back) hang from the computer, so a parent always comes first.

What is not listed yet: disks as children of their controller, HD Audio
codecs as children of theirs, the ACPI namespace, CPUs, the RTC, the PC
speaker, serial ports, the IOMMU as such, and a PS/2 mouse whose set-up
failed (a failed PS/2 keyboard is listed). Each is a record type or a
`platform_add()` away; none is needed by the first consumer.

## Identity

- **An id names one attachment and is never reused** while the system runs.
  The computer and the PCI functions take theirs at boot, in scan order; a USB
  device and each of its interfaces take a new one on every attach, so a stick
  pulled and pushed back in is a new instance. Slot indexes, pointers and names
  are never ids.
- **`generation`** bumps on every change: an instance attached or detached, a
  driver claiming or giving up, a name recorded. Every record of one snapshot
  carries the same value.
- No persistent identity (serial numbers, physical paths across boots) yet.

## States

The enum names all of the roadmap's states so the numbers stay; today the
kernel reports three of them, each with a reason:

| State | Reason | Meaning |
| --- | --- | --- |
| `STARTED` | `NONE` | A driver took it and it works |
| `FAILED` | `START_FAILED` | A driver took it and gave up while starting it |
| `UNBOUND` | `NOT_CLAIMED` | No built-in driver took it: none matches, or its class core chose another device (the second NIC) |
| `UNBOUND` | `BOOT_OPTION` | A boot option kept the drivers away (`nousb` on the USB controllers) |
| `UNBOUND` | `UNSUPPORTED` | The driver took it and found nothing it supports on it (HD Audio with no analog codec); the record keeps the driver's name |

`DISCOVERED`, `STARTING`, `QUIESCING`, `DISABLED` and `REMOVED` are named,
not produced. A removed instance is absent from the next snapshot, never
listed as `REMOVED`.

Flags: `SYNTHETIC` (no hardware behind it: the RAM disk), `FIRMWARE` (used as
the firmware set it up: the boot framebuffer, the host and PCI-to-PCI bridges
`pci::` reaches the functions through), `REMOVABLE` (USB).

## How drivers report

The rule is that a driver reports what it took, at the points it already
has, and listing never reprobes anything:

- `inventory::pci_claim(device, "name")` where a driver decides the function
  is its own, and before it touches anything of it. It returns false when
  another driver holds the function ([Ownership](#ownership)). **From then
  until `pci_started()` the function reads as `FAILED`**, so every early
  return of a start path is reported as the failure it is without the driver
  saying so.
- `pci_release(device, "name")` when the holder gives the function back,
  quiet: it reads as `UNBOUND`/`NOT_CLAIMED` again and its name is cleared.
- `pci_started()` once it works, `pci_failed()` when it stops working later (a
  NIC that does not come up when asked), `pci_unsupported()` for the HD Audio
  case, `pci_name()` for what the hardware calls itself (an NVMe model, the
  RTL8168 revision, the first HD Audio codec).
- Central hooks cover whole families: `virtio_pci::initialize_device` claims
  (the driver name comes from the virtio device type), `DRIVER_OK` starts and
  `fail_device()` fails every virtio driver; `usb::hc::prepare_pci` claims and
  `add_controller()` starts every USB host controller.
- `ata` claims the PCI IDE function in compatibility mode before it probes the
  legacy ports, since those ports are that function's, and gives it back when
  no disk answers. A machine with no such function has its ports probed as
  before, with nothing to claim.

Where each driver does it: `ahci`, `nvme`, `ata`, `rtl8139`, `rtl8168`,
`virtio-net` (started at probe; `DRIVER_OK` waits for bring-up), `ac97`,
`azalia`, the virtio family, the USB controllers, `ps2`, `ramdisk`, and the
framebuffer in `kernel_main`.

## Ownership

A PCI function has one holder at a time: the driver whose name its record
carries. `pci_claim` grants it when the record names nobody, when its driver
gave it back (`UNBOUND`, whatever the reason), or when the claimant is the
holder itself (a driver retrying its own function, as virtio-input does when
a device turns out not to be a pointer). Any other holder keeps it, `FAILED`
included, because a driver that gave up may still own the device's DMA. The
host and PCI-to-PCI bridges are held by `pci`. A refusal logs
`inventory: <driver> refused at <bus>:<slot>.<function>, held by <holder>`
and changes nothing, generation included. The rule is
`inventory::claim_allowed()` in `inventory_rules.hpp`.

What it means for a driver:

- **The claim comes first.** `pci::bar_info()` sizes a BAR by turning decoding
  off and writing all ones to it, so it is the claimant's to call, never a
  probe's. The USB host controllers claim at the top of `start_controller`
  (`usb::hc::claim_pci`) for that reason.
- **Refused means hands off**: the driver returns without mapping, enabling or
  reporting on the function. `pci_started()`, `pci_failed()`,
  `pci_unsupported()` and `pci_name()` are for the holder alone; virtio's
  `initialize_device` clears its `Device` before claiming, so a
  `fail_device()` after a refusal reports nothing.
- `pci_holder()` says who holds a function and in what state.

- **A driver looking for its device passes over held functions** with
  `pci_available()`, the claim's rule without claiming or logging, and goes on
  to the next match; `pci_claim` still decides. `ahci` drives every free HBA;
  `rtl8139`, `rtl8168` and `ac97` take the first free match and drive one,
  since `nic::` selects one NIC and `ac97` keeps one controller's state.
- **A match is checked without side effects.** `ac97` matches class 04/01
  only with I/O BARs 0 and 1, read as the boot scan left them, so another
  audio function of that class (virtio-sound's BARs are memory) is not its.

Not covered yet: claims cover whole functions, not the MMIO ranges, I/O ports
and interrupt routes inside them. That is a later D3 slice.

## Resources

Only what was recorded, never measured at listing time:

- A PCI function's BARs as the boot scan read them: kind (memory or I/O),
  base, 64-bit and prefetchable flags. **The length is 0, "not recorded"**:
  sizing a BAR writes all ones to it, which a listing must not do to a working
  device. A bridge lists its two BARs; CardBus none.
- The interrupt line the firmware wrote, when the function has an INTx pin.
  It is the assignment, not a claim that the driver uses it: most drivers here
  poll.
- Platform devices list what their driver knows: the 8042's ports and IRQ 1
  or 12, the framebuffer's physical range.

## The call

`SXN_SYS_DEVICE_SNAPSHOT` (`0x104E`): rdi = `struct sxn_device_info[]`, rsi =
how many fit, rdx = the record size. The whole inventory is copied in one call
under the kernel lock, parents before children: nothing can come or go
halfway, and there is no index to go stale between calls. The return value is
how many instances there are; when that is more than rsi, only rsi were
written and the caller asks again with room. rsi = 0 only counts. A record
size below `SXN_DEVICE_INFO_V1_SIZE` (or above 64 KiB), more than 65536
records or a bad buffer is `-EINVAL`. Any process may read it: nothing in it
is private, and viewing hardware must not need elevation.

The struct grows at the end only; the kernel writes at most the record size
the caller passed, at that stride. POSIX's `device_snapshot()`
(`SAVANXP_SYS_DEVICE_SNAPSHOT` = 78) is the same call over
`struct savanxp_device_info`, whose layout `static_assert`s tie to SUN's.

SUN: `sxn_device_snapshot(out, capacity)` in `include/sxn/sxn.h`.

## Where it lives

- `include/kernel/inventory.hpp`, `kernel/inventory.cpp`: ids, PCI bindings,
  platform devices, the snapshot.
- `include/kernel/inventory_rules.hpp`, `kernel/inventory_rules.cpp`: the
  pure half, compiled by the host test as the kernel compiles it:
  classification by class codes, BAR decoding, the PCI tree, the record sink.
- `kernel/usb/usb.cpp`: `usb::describe()`; `usb::Device` keeps its inventory
  id, its product and manufacturer strings and its interfaces.

## Tests

- `device-inventory-test` (host): classification, BARs without sizing, the
  PCI tree with an orphan bus and a loop, a sink that runs out of room or
  fails, text cut to fit.
- `nativetest` (`smoke native-test`): the computer first, unique ids, one
  generation, every parent before its children, states with their reasons,
  PCI locations matching their numbers, a started storage controller, a short
  buffer, a short record size and a bad pointer.
- `smoke` (POSIX): `device_snapshot` in order with a started virtio-rng.
- `smoke devmgmt-smoke`: Device Manager's self-test over the live inventory
  with USB devices attached.
- `device-inventory-test` also covers `claim_allowed()`: free, given back,
  held while started or failed, the bridges, the holder again, names that
  share a prefix, an empty claimant.
- `smoke ahci-two-hba`: a disk alone on a second AHCI controller is found
  next to the board's.
- `smoke inventory-claims`: boot option `inventory-selftest`, after every
  driver started. A started function refuses another driver and its record
  and generation stay; a free one is taken, refused to a second driver,
  taken again by its holder, given back and taken by the second. Only the
  records change; no device is touched.
