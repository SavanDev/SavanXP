# USB: an xHCI stack, built in QEMU first

> **Status: plan only. Nothing here is implemented.** The tree has no USB code
> at all: no host controller driver, no HID, no mass storage. This document
> fixes the decisions before the first line is written, says which parts of
> OpenBSD's stack are used and how, and orders the work so that every phase
> boots and can be checked in QEMU on its own.

## The short version

- **One controller: xHCI.** Every PC since about 2012 has one, and it handles
  every USB speed itself. EHCI, OHCI and UHCI wait until a target machine needs
  them.
- **Polled, like every other driver here.** No interrupts and no sleeping in the
  kernel. The event ring is drained from `input::Driver::poll` for HID and from
  the block request for storage.
- **Built and tested in QEMU**, which emulates xHCI and the devices behind it.
  What QEMU cannot show (firmware handoff, chip quirks, real reset timing) is
  the last phase, on real hardware.
- **OpenBSD is a reference, not a source to port wholesale.** Its drivers are
  bound to `autoconf`, `bus_dma` and interrupt-driven sleeping, none of which
  exist here. Small self-contained pieces (descriptor structs, register
  definitions, the HID report parser, the ID table) are copied with their ISC
  notices. See [What comes from OpenBSD](#what-comes-from-openbsd).
- **Order:** harness, controller, enumeration, HID keyboard and mouse, mass
  storage, hubs, hot plug, real hardware.

## Why it matters

Without USB, SavanXP on a modern machine has:

- **no keyboard under UEFI** unless the laptop's keyboard is PS/2 internally.
  Firmware emulates PS/2 for a USB keyboard only under legacy BIOS, and that
  emulation stops at `ExitBootServices` on a UEFI boot;
- **no external mouse**, and no touchpad on machines that wire it over USB
  ([REAL_HARDWARE.md](../../kernel/REAL_HARDWARE.md), item 5);
- **no removable media**: the FAT driver and Disk Management are ready for a
  stick, but nothing can reach one ([FAT_ROADMAP.md](FAT_ROADMAP.md));
- **no install source or target** beyond SATA, IDE and virtio
  ([SETUP_AND_UPDATES.md](../../userland/SETUP_AND_UPDATES.md), "Limits accepted").

## What already fits

The kernel already has the places a USB stack plugs into. None of them needs
to change shape:

| Need | What exists |
| --- | --- |
| Find the controller | `pci::` enumeration, `pci::bar_info`, `pci::find_capability`; class `0x0C`, subclass `0x03`, prog-if `0x30` is xHCI |
| DMA memory | `memory::allocate_page` and `memory::allocate_contiguous_pages`, as AHCI uses them ([AHCI.md](../../kernel/AHCI.md)) |
| Real delays | `timer::monotonic_ns()`. USB needs wall-clock waits (50 ms port reset, 10 ms recovery); AHCI's spin budgets in register reads are not enough here |
| Keyboard and pointer | `input::register_driver`; `poll` runs from the timer tick and from blocking reads ([INPUT.md](../../kernel/INPUT.md)) |
| A disk | `block::register_driver`; `partition::` slices it, `fat::` and Disk Management mount it at `/mnt` |
| A disk that goes away | `block::generation()` already bumps on removal, and `fs::kMaxMounts` reserves a slot for one removable medium |
| Firmware handoff | AHCI already does BIOS/OS handoff (`BOHC`); xHCI's `USBLEGSUP` capability is the same idea |

## Decisions

### xHCI only, until a machine asks for more

xHCI covers low, full, high and super speed on one controller, so one driver
reaches every device on a modern machine. Older machines route USB 1.1 and
2.0 ports to UHCI/OHCI plus EHCI companions, which is three drivers and a
routing problem. The [Compaq](../../kernel/REAL_HARDWARE.md) is one of those machines, but
it cannot run master at all, so it does not drive this decision.

### Polled, no interrupts

The same model as `ahci`, `ata` and `virtio-blk`: completion is found by
reading the event ring. One interrupter, its ring drained by whoever needs a
result. The kernel cannot sleep on a kernel stack
([SMP_ROADMAP.md](../done/SMP_ROADMAP.md)), so an interrupt would have nothing to wake.
MSI or MSI-X comes when the kernel can block, not before.

The cost is latency on HID: a key is seen at the next tick, which is the same
latency `virtio-input` has today.

### Where the code lives

`kernel/usb/`: `xhci.cpp` (the controller), `usb.cpp` (enumeration and the
device table), `hid.cpp` (keyboard and pointer), `umass.cpp` (mass storage),
plus the copied OpenBSD headers with their notices. The USB core sees
controllers through a small interface of its own, so EHCI could be added later
without touching HID or storage.

### HID: boot protocol first

Keyboards and mice support the fixed 8-byte "boot protocol" (`SET_PROTOCOL`
0), so no report descriptor has to be parsed to get a keyboard and a relative
mouse working. Usages map to set 1 positions at the driver's edge with a
table, as [INPUT.md](../../kernel/INPUT.md) already says a USB keyboard would. Lock LEDs go
out with `SET_REPORT` from `poll`, never from `set_keyboard_leds` itself.

`usb-tablet` and most real touchpads only speak the report protocol. They
need the HID report parser, which is the piece copied from OpenBSD.

### Mass storage: Bulk-Only Transport and a SCSI subset

Every stick speaks BOT with SCSI commands: `INQUIRY`, `TEST UNIT READY`,
`READ CAPACITY (10)` and `(16)`, `READ (10)`, `WRITE (10)`, `REQUEST SENSE`,
`SYNCHRONIZE CACHE`. UAS (USB Attached SCSI) is faster and optional on the
device side; it waits. The result is one `block::Driver` like the others, and
it never becomes the root: `fs::mount_any` keeps preferring the system disk.

### The rule that one keyboard wins has to change

[INPUT.md](../../kernel/INPUT.md) silences any keyboard registered after the first, because
QEMU feeds the host keyboard to every backend at once. On real hardware that
is wrong: a laptop's internal PS/2 keyboard and a USB keyboard are both real,
and both must work. The fix belongs to the input layer, not to the USB driver,
and has to land before USB HID is enabled on real hardware. Until then the
harness keeps the old behavior by registering USB HID after `ps2`, so QEMU's
PS/2 keyboard keeps winning unless a scenario turns it off.

## What comes from OpenBSD

Categories as in [THIRD_PARTY_ADOPTION.md](../../third-party/THIRD_PARTY_ADOPTION.md).
Everything copied gets an entry in
[THIRD_PARTY_PROVENANCE.md](../../third-party/THIRD_PARTY_PROVENANCE.md) with
the pinned OpenBSD commit, in the commit that copies it.

`Selective port` (copied, ISC header kept):

| File | Why |
| --- | --- |
| `sys/dev/usb/usb.h` | Standard descriptors and request codes. Pure data |
| `sys/dev/usb/xhcireg.h` | xHCI registers, TRB types and context layouts. Pure data |
| `sys/dev/hid/hid.c`, `hid.h` | The HID report descriptor parser. Self-contained, fiddly, and not worth rewriting |
| `sys/dev/usb/usbdevs` with `devlist2h.awk` | Vendor and product names from text, the same case as `pcidevs` |

`Reference` (read, then written from scratch):

| File | What to learn from it |
| --- | --- |
| `sys/dev/usb/xhci.c` | Ring management, the order of slot and endpoint setup, error recovery: the parts the specification explains worst |
| `sys/dev/pci/xhci_pci.c` | BIOS handoff and per-vendor quirks (Intel port routing, AMD). The most useful file for real hardware |
| `sys/dev/usb/uhub.c`, `usbdi.c`, `usb_subr.c` | Port reset timing, enumeration, `SET_ADDRESS` and descriptor fetching |
| `sys/dev/usb/umass.c`, `umass_scsi.c` | BOT and its reset recovery when a transfer stalls |
| `sys/dev/usb/usb_quirks.c` | Which devices lie about themselves |
| `sys/dev/usb/ukbd.c`, `sys/dev/hid/hidkbd.c`, `hidms.c` | Boot-protocol reports and the usage tables |

`Do not adopt`: the driver framework itself (`usbdi` as an API, `autoconf`
attachment, `bus_dma`, the `spl` levels and `tsleep`). It is what makes the
drivers impossible to lift, and SavanXP's registries already do that job.

## The QEMU harness

`tools/run_qemu.py` gains `--usb`, which adds:

```text
-device qemu-xhci,id=xhci
-device usb-kbd,bus=xhci.0,id=usbkbd
-device usb-tablet,bus=xhci.0,id=usbtablet
```

and `--usb-stick PATH`, which adds a raw image as `usb-storage` on the same
controller. Both work on the AHCI machine and with `--virtio`.

Debugging aids:

- `-trace 'usb_xhci_*'` logs every doorbell, command and event as QEMU sees
  it, so a hung ring shows which side stopped.
- `info usb` in the monitor lists what is attached and at which speed.
- QMP `input-send-event` takes a `device` argument, so a smoke can type into
  `usbkbd` specifically and prove the key arrived through USB, not PS/2.

Under UEFI, OVMF drives the controller before the kernel starts. The driver
has to halt and reset it (`HCRST`) and never assume a clean state, which is
also what real firmware requires.

## Phases

Each phase boots, keeps every existing smoke green, and adds one boot log line
or test that proves it.

### Phase 0: harness

`--usb` and `--usb-stick` in `run_qemu.py` and the smoke runner. No driver.
The boot log is unchanged, and QEMU's `info usb` shows the devices.

### Phase 1: the controller

Find the xHCI function, take it from the firmware (`USBLEGSUP`), halt, reset,
allocate the device context array, scratchpad buffers, command ring and one
event ring, start it, and round-trip a `No Op` command.
Log: `xhci: <bus:slot.fn>, <n> ports, <n> slots`.

### Phase 2: enumeration on root ports

Port reset with real delays, `Enable Slot`, `Address Device`, the device and
configuration descriptors, `SET_CONFIGURATION`. No class drivers yet: every
device is only logged. Log: `usb: port <n> <vid>:<pid> class <c> <speed>`.

### Phase 3: HID keyboard and mouse

Boot protocol, an interrupt endpoint per device, the usage table, LEDs. Then
the HID parser for `usb-tablet` and report-protocol devices.
Test: a smoke that types through `usbkbd` with QMP and moves `usbtablet`.

### Phase 4: mass storage

BOT plus the SCSI subset as a `block::Driver`. A stick present at boot shows
up in Disk Management and mounts at `/mnt` through the FAT driver.
Test: a smoke with `--usb-stick` that reads a known file and writes one back.

### Phase 5: hubs

`usb-hub` in QEMU. Real machines put ports behind internal hubs (webcams,
card readers, the Intel rate-matching hubs), so this is required before real
hardware, not optional.

### Phase 6: hot plug

Port status change events: attach a device after boot, and detach one. A
removed stick unregisters its block device; a mounted volume on it is released,
and what was not yet written is lost. QEMU's `device_add` and `device_del`
drive it from a smoke.

### Phase 7: real hardware

The handoff on real firmware, vendor quirks from `xhci_pci.c`, slow devices,
and the one-keyboard rule fixed in the input layer. Checked on the machine,
with the boot log over serial or on screen.

## Out of scope

- EHCI, OHCI, UHCI, until a target machine needs them.
- Isochronous transfers: audio, webcams.
- UAS, USB 3 streams, power management, selective suspend.
- Booting or installing the system from USB. The ISO already boots from a
  stick through the firmware. Running *from* one after boot is a separate
  question that phase 4 makes possible but does not answer.

## Open questions

- How the input layer lets several keyboards and pointers coexist without QEMU
  delivering every event twice.
- Whether HID polling at the timer tick is good enough for a mouse at 1000 Hz,
  or whether the event ring needs draining more often.
- Which real machine is the first target, which decides whether EHCI is ever
  needed.
