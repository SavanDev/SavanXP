# AHCI storage

`kernel/ahci.cpp` drives SATA disks through an AHCI host bus adapter, the
controller every PC since about 2008 uses and the one built into QEMU's `q35`
machine. It exists for one reason: **DMA**. The legacy `ata` driver moves every
sector through the data port (PIO), and QEMU's IDE model charges a backend
request per sector of a PIO data phase. AHCI moves up to 256 sectors per
command with a single descriptor.

## What QEMU attaches where

Without `--virtio`, `tools/run_qemu.py` puts `build/disk.img` on **port 1** of the
board's AHCI controller. Port 0 already holds the FAT drive the firmware boots
from (`-drive file=fat:...` defaults to the first AHCI port), and a LiveCD boot
adds its CD-ROM as an ATAPI device. So a normal boot shows:

```text
ahci: ahci0 port 0, 1032192 sectors     the FAT boot drive
ahci: ahci1 port 1, 2097152 sectors     disk.img, mounted as /disk
block: 3 device(s) ahci0(rw) ahci1(rw) ahci0p1(rw)
```

`ahci0` is registered like any disk; nothing mounts it because no filesystem
driver claims it, and `fs::mount_any` takes the first device SxFS recognizes.
ATAPI ports and empty ports are skipped.

`SAVANXP_DISK_BUS=ide` (or `run_qemu.py --disk-bus ide`) attaches the disk to
`isa-ide` instead, which is how the `ata` driver keeps being exercised. `--virtio`
uses virtio-blk and ignores the setting.

## How a request runs

The driver keeps the HBA in AHCI mode with interrupts off, and completes every
request by polling, like `ata` and `virtio-blk`:

1. Bring-up, per port with a SATA signature (`0x00000101`) and a device present:
   stop the command list and FIS receive engines, point `PxCLB`/`PxFB` at one
   page of our own (command list, received-FIS area, one command table), clear
   errors, start the engines, and `IDENTIFY DEVICE` for the sector count
   (LBA48 words 100-103, else 60-61).
2. A request uses command slot 0: an H2D register FIS with `READ DMA EXT` or
   `WRITE DMA EXT`, one PRDT entry, then a write to `PxCI` and a spin until the
   HBA clears the bit. Data goes through a 128 KiB physically contiguous bounce
   buffer, so callers never have to hand in DMA-able memory.
3. `block::flush` is `FLUSH CACHE EXT`.
4. A task-file error or a fatal interrupt status stops the port, clears
   `PxSERR`/`PxIS` and starts it again, so the next request can run; the failed
   one returns false to `block::`.

The firmware's ownership is released through BIOS/OS handoff when the HBA
advertises it. 64-bit DMA addresses are used only when `CAP.S64A` says so; the
DMA memory comes from the physical page allocator, which in practice is below
4 GiB.

## What it buys

The 390 KB fixture copy of `progman-smoke`, QEMU under KVM, with host flushes
ignored (`cache=unsafe`) so only the transfer is measured:

| | IDE (PIO) | AHCI (DMA) |
| --- | --- | --- |
| 390 KB copy, 48 commits | 1.99 s | 0.33 s |

With flushes honoured both take about 5 s, because each of the 48 commits
flushes four times and each flush is an `fsync` of the image on the host. The
transfer is no longer what a write costs; the flushes and the frozen system
around them are. See [`SXFS_ROADMAP.md`](../fs/SXFS_ROADMAP.md#where-the-remaining-cost-is).

## Not done

- **Interrupts.** Completion is polled with interrupts disabled, like every
  storage driver here: the kernel's syscalls cannot sleep on a kernel stack
  ([`SMP_ROADMAP.md`](SMP_ROADMAP.md)). NCQ and multiple slots buy nothing
  until requests can overlap.
- **Port multipliers, hot plug, link power management.** Ports are probed once
  at boot.
- **ATAPI.** The LiveCD reaches the kernel as a Limine module, not through the
  CD drive, so no driver reads optical media.
