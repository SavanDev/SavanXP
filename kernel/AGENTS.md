# Working in `kernel/`

The root [`AGENTS.md`](../AGENTS.md) applies; this adds what is particular to
the kernel.

## Read first

The document of the subsystem being changed, from the "Kernel" section of
[`docs/README.md`](../docs/README.md). The ones most changes run into:

- [`ARCH.md`](../docs/kernel/ARCH.md): no inline assembly outside `arch/` and
  `include/arch/`; a driver calls the named CPU operation.
- [`KERNEL_SECURITY.md`](../docs/kernel/KERNEL_SECURITY.md): system call
  pointers, lengths and flags, and on-disk metadata, are hostile input.
- [`KERNEL_NATIVE_API.md`](../docs/kernel/KERNEL_NATIVE_API.md): the Sx
  dispatcher and what stays invariant, for a new or changed `SXN_SYS_*` call.
- [`DEVICE_INVENTORY.md`](../docs/kernel/DEVICE_INVENTORY.md): how a driver
  reports the device and the resources it took.
- [`SMP_ROADMAP.md`](../docs/roadmaps/open/SMP_ROADMAP.md): the kernel lock and
  the work to run code outside it.
- Hardware QEMU cannot reach: [`REAL_HARDWARE.md`](../docs/hardware/REAL_HARDWARE.md)
  says how a run on metal is recorded.

## Rules here

- A call the kernel gains reaches SUN and POSIX alike (parity, root
  `AGENTS.md`), with its `nativetest` assertion.
- Logic that can run on the host (table parsers, rules) is also compiled into
  a host test under `tests/host/`, as `inventory_rules.cpp` and the ACPI and
  IOMMU table parsers are.

## Verify

- Always `./build.sh smoke smoke`. A system call or the native API:
  `./build.sh smoke native-test`.
- The scenario of the driver or subsystem touched, from
  `./build.sh smoke --list`: USB `*hci-*` and `usb-*`; audio `hda-*`, `ac97-*`,
  `virtio-*`; IOMMU `iommu-*`; power `power-smoke`, `power-button-smoke`;
  panics `panic-*`; network `net-smoke`, `tcp-smoke`; storage `sxfs-smoke`,
  `partition-smoke`.
- SxFS, the block layer or anything that writes the disk image: the
  persistence check in the root `AGENTS.md`.
