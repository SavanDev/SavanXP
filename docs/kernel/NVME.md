# NVMe storage

`kernel/nvme.cpp` drives NVMe SSDs: any PCI function of class `01/08/02`. It
is a selective port of OpenBSD's `nvme(4)` (`sys/dev/ic/nvme.c`, `nvmereg.h`,
`sys/dev/pci/nvme_pci.c`, pinned in
[THIRD_PARTY_PROVENANCE.md](../third-party/THIRD_PARTY_PROVENANCE.md#openbsd-nvme4-nvme-driver)),
cut down to what `block::` needs: no SCSI layer, no `bus_dma`, no interrupts.
It is HW-5 in [REAL_HARDWARE.md](../hardware/REAL_HARDWARE.md).

## Disks are read-only unless the boot entry says otherwise

Every NVMe namespace is registered **read-only** unless the kernel command line
carries `nvme-rw`. The partitions cut out of it inherit that.

The rule exists because, when the driver was written, the only NVMe drive on
hand was the boot disk of the development host, with another system on it.
Reading its partition table is harmless; one wrong write is not. So on metal
the driver starts by reading, and writing is a deliberate act: add `nvme-rw`
to the entry's command line in Limine's editor (`E` on the menu).

`boot/limine-dev.conf`, which only QEMU boots, carries `nvme-rw`, so a QEMU
machine with its root on NVMe works like any other. `boot/limine.conf`, the
ISO's, does not. When the driver has written to real drives without
surprises, the default flips and this section goes.

## What QEMU attaches where

`SAVANXP_DISK_BUS=nvme` (or `run_qemu.py --disk-bus nvme`) puts `build/disk.img`
on QEMU's emulated controller (`-device nvme`, PCI `1b36:0010`, serial
`SXNVME0`) instead of AHCI port 1. The FAT boot drive stays on AHCI port 0.
Every launcher honours the variable, so the smokes run on NVMe too:

```bash
SAVANXP_DISK_BUS=nvme ./build.sh smoke smoke
```

A boot then shows:

```text
nvme: controller 1b36:10, NVMe 1.4
nvme: "QEMU NVMe Ctrl", firmware 11.1.3, 256 namespace(s), mdts 7
nvme: nvme0 namespace 1, 2097152 sectors, "QEMU NVMe Ctrl" serial "SXNVME0"
block: 3 device(s) nvme0(rw) ahci0(rw) ahci0p1(rw)
```

The driver's priority (107) is between virtio-blk (110) and AHCI (105).

## How it works

Bring-up per controller, in `nvme_attach()`'s order:

1. Map BAR0 uncached, enable memory decoding and bus mastering, disable INTx.
2. Read `CAP`: the doorbell stride, the ready timeout (`CAP.TO`, in 500 ms
   units), the largest queue. A controller whose smallest memory page is above
   4 KiB, or that lacks the NVM command set, is skipped.
3. Disable it (waiting for `CSTS.RDY` to follow), point `ASQ`/`ACQ` at a
   16-entry admin queue pair, and enable it with 4 KiB pages, 64-byte
   submission and 16-byte completion entries.
4. Identify Controller: model, serial, firmware, `MDTS` (which caps a request
   below the driver's own 256 sectors) and the namespace count.
5. Create one I/O queue pair, physically contiguous, with interrupts off.
6. Identify Namespace for namespaces 1 to 8: each active one becomes `nvmeN`.

Every command is submitted alone and its completion polled through the phase
bit, like every other storage driver here: the kernel's syscalls cannot sleep
on a kernel stack. Data goes through a 128 KiB physically contiguous bounce
buffer. A request of more than two pages points PRP2 at a one-page PRP list.
`block::flush` is the NVM `Flush` command.

A command that times out, or a controller that sets `CSTS.CFS`, marks the
controller failed. Its queues can no longer be trusted, so every later request
on it fails instead of guessing.

## Shutdown

An NVMe drive wants to be told before it loses power. Without that it counts an
unsafe shutdown in its SMART log and may spend the next power-up recovering.
`block::Driver` has an optional `shutdown` hook, and `block::shutdown_all()`
drains the writeback queue and then runs every hook. `/dev/power` calls it
before S5 and before a reboot. The NVMe hook deletes the I/O queues, sets
`CC.SHN` to a normal shutdown and waits up to 4 s for `CSTS.SHST`, as
`nvme_shutdown()` does. Success prints:

```text
nvme: controller 0 shut down
```

The power button does not take this path. Its SCI handler calls
`acpi::shutdown()` directly from the interrupt, without draining the writeback
queue, as it already did for every disk.

## Limits

- **512-byte blocks only.** `block::` is built on 512-byte sectors. A namespace
  formatted with 4 KiB blocks, or with metadata, is logged and skipped:
  `nvme: namespace 1 has 4096-byte blocks ...`. Most consumer drives ship
  formatted with 512-byte blocks. A drive that offers both can be reformatted
  (`nvme format --lbaf`), which erases it.
- **2 TiB.** `block::` addresses 32-bit sector numbers, so a larger namespace
  is exposed up to its first 2 TiB, as with AHCI. `DeviceIdentity` keeps the
  real size.
- **Two controllers, four namespaces, eight namespace IDs scanned** per
  controller.
- **No interrupts, one queue, one command in flight.** More would buy nothing
  until requests can overlap.
- **No Host Memory Buffer.** DRAM-less drives (the Kingston NV1 of the Ryzen
  target among them) work without one, more slowly.
- **No SMART, no TRIM, no APST.** The drive stays in its default power state.
