# virtio: what the transport asks of a driver here

Every paravirtualized device SavanXP drives — `virtio-net`, `virtio-blk`,
`virtio-gpu`, `virtio-input`, `virtio-sound` — sits on the same modern
virtio-pci transport, implemented once in
[`kernel/virtio_pci.cpp`](../kernel/virtio_pci.cpp). This document records the
rules of that transport that are not visible from the code that uses it, and
that cost a debugging session each time they are rediscovered.

## A 64-bit field is touched in two 32-bit halves, low first

The virtio specification (1.x, §4.1.3.1) only guarantees that a device answers
accesses of the *natural width* of each field: 8 bits for a byte, 16 for a
word, 32 for a doubleword. For the 64-bit fields there is no natural width to
promise — the specification says the driver may access the two 32-bit halves
independently, and that is the only portable way to reach them. Linux does the
same thing (`vp_iowrite64_twopart`).

The fields that matter are the three queue addresses of the common
configuration — `queue_desc`, `queue_driver`, `queue_device` — plus any 64-bit
field of a device-specific configuration, such as the capacity of
`virtio-blk`.

Nobody writes them by hand. `virtio_pci::write_mmio_u64()` and
`virtio_pci::read_mmio_u64()` are the only way in, and they exist for this
reason alone.

## QEMU forgives it; VirtualBox kills the machine

A single 64-bit access compiles to one `movq` against the device's MMIO
window. QEMU splits an access wider than the device implements into accesses
the device does implement, so the driver looks correct there and the smoke
suites pass. VirtualBox does not split: its virtio core rejects the access,
returns `VERR_INVALID_PARAMETER` from the MMIO write handler, and PGM turns
that into a **guru meditation** — the whole virtual machine stops, not just
the device.

It looks like this in `VBox.log`:

```
AssertLogRel PGMAllPhys.cpp(4339) pgmPhysWriteHandler: PGM_HANDLER_PHYS_IS_VALID_STATUS(rcStrict, true)
rcStrict=VERR_INVALID_PARAMETER GCPhys=00000000f0000020
Changing the VM state from 'RUNNING' to 'GURU_MEDITATION'
!!         VCPU0: Guru Meditation -2 (VERR_INVALID_PARAMETER)
```

`GCPhys` is the give-away: subtract the base of the device's MMIO region (the
log lists it, `f0000000` for `virtio-net #0 (modern)`) and the remainder is the
offset inside the common configuration — `0x20` is `queue_desc`. The `rip` in
the same dump resolves against `build/kernel.elf` with
`toolchain/llvm/bin/llvm-symbolizer`, which names the function directly.

The consequence for the work: **the QEMU smokes cannot catch this class of
bug**. A change to `virtio_pci::` or to any virtio driver is only verified once
it has also brought a queue up under VirtualBox, which is the stricter of the
two and the one that behaves like real hardware would.

## Where a queue comes up

`probe()` only reads the configuration space: the device identity, the feature
bits and whatever the device publishes about itself (the MAC of `virtio-net`,
the capacity of `virtio-blk`). Queues are armed later, in `bring_up()`.

That is why a bad access to a queue address does not show up during boot for a
NIC: nothing sets up the network queues until userland asks the interface to go
up — `netinfo` on `/dev/net0` is normally the first thing that does — and so a
transport bug surfaces as "running this program kills the machine" rather than
as a boot failure.
