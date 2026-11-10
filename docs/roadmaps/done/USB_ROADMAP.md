# USB: an xHCI stack, built in QEMU first

> **Status: done.** Phases 0 to 6 were built and checked in QEMU, and phase 7
> on real hardware: the ISO boots from a USB stick on the development machine
> and its USB keyboard and mouse drive the desktop. xHCI controllers start,
> devices are enumerated on root ports and behind hubs, USB keyboards, mice
> and tablets work next to PS/2 and virtio ones, USB sticks are block devices
> Disk Management mounts, and devices can be plugged in and pulled out while
> the system runs. What was left out, and what was not checked on the
> machine, is listed at the end of phase 7 and under
> [Out of scope](#out-of-scope).

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
  ([REAL_HARDWARE.md](../../hardware/REAL_HARDWARE.md), HW-8);
- **no removable media**: the FAT driver and Disk Management are ready for a
  stick, but nothing can reach one ([FAT_ROADMAP.md](../open/FAT_ROADMAP.md));
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
routing problem. The [Compaq](../../hardware/PENTIUM4_EVO_N1020V.md) is one of those machines, but
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
it never becomes the root: its priority (5) is below the ramdisk's (10), and
`fs::mount_any` takes the first device it recognizes in that order, so the
system disk and the LiveCD's volume always come first.

A stick is **written through** (`deferred_writes = false`): it can be pulled
out, and writes held in the writeback queue
([BLOCK_WRITEBACK.md](../../kernel/BLOCK_WRITEBACK.md)) would be lost with it
and would keep its block device from being unregistered. Only 512-byte
sectors and direct-access devices (SCSI type 0) are taken; a card reader's
other LUNs, CD drives and 4 KiB-sector disks are logged and left alone.

### USB input coexists with every other input driver

[INPUT.md](../../kernel/INPUT.md) silences any keyboard registered after the first. On real
hardware that is wrong for USB: a laptop's internal PS/2 keyboard and a USB
keyboard are both real, and both must work. **Decided: USB keyboards and
pointers coexist with PS/2 and virtio ones.** `usb-hid` registers after
`ps2`, silences nothing and is never silenced, and registers whenever a
controller started, so a keyboard plugged in later works. QEMU delivers each
event to one device only, so nothing arrives twice; the PS/2-versus-virtio
rule stays as it was.

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

`./build.sh run --usb` (or `SAVANXP_USB=ON`, or `tools/run_qemu.py --usb`)
adds:

```text
-device qemu-xhci,id=xhci
-device usb-kbd,bus=xhci.0,id=usbkbd
-device usb-tablet,bus=xhci.0,id=usbtablet
```

The pointer is a tablet for a QEMU window, where the host pointer maps onto
it. Smokes get `usb-mouse` instead (`--usb-pointer mouse`, the catalog's
`usb_pointer`): QEMU sends relative moves to a relative mouse but buttons to
whichever pointer it activated last, so a tablet next to the PS/2 mouse takes
clicks the mouse moved for, at its own position.

and `run_qemu.py --usb-stick PATH` adds a raw image as `usb-storage` (serial
`SXUSB0`) on the same controller. Both work on the AHCI machine and with
`--virtio`. A smoke scenario asks for them with `usb=True` and
`usb_stick_mib=N`; the stick is a fresh FAT image per run, built by
`fat-image` with `/hello.txt` on it, like `fat_disk_mib`. `SAVANXP_USB=ON`
also adds the devices to any smoke, which is how every existing scenario is
checked against a machine that has USB.

Debugging aids:

- `SAVANXP_USB_TRACE=PATH` writes QEMU's `usb_xhci_*` trace events (every
  doorbell, command and event as QEMU sees it) to PATH, so a hung ring shows
  which side stopped.
- `info usb` in the monitor lists what is attached and at which speed.
- To prove an event travelled USB, take the other devices away: a scenario
  with `no_ps2=True` boots with `i8042=off`, so USB is the only keyboard and
  pointer. QMP `input-send-event` has a `device` argument for this, but
  QEMU 11.1 aborts on it (`Property 'qemu-fixed-text-console.device' not
  found`).

Under UEFI, OVMF drives the controller before the kernel starts. The driver
has to halt and reset it (`HCRST`) and never assume a clean state, which is
also what real firmware requires.

## Phases

Each phase boots, keeps every existing smoke green, and adds one boot log line
or test that proves it.

### Phase 0: harness

**Done.** `--usb` and `--usb-stick` in `run_qemu.py`, the smoke runner and
`build.sh`. No driver: the boot log is unchanged, and `smoke` passes with
`SAVANXP_USB=ON`.

### Phase 1: the controller

Find the xHCI function, take it from the firmware (`USBLEGSUP`), halt, reset,
allocate the device context array, scratchpad buffers, command ring and one
event ring, start it, and round-trip a `No Op` command.
Log: `xhci: <bus:slot.fn>, <n> ports, <n> slots`.

**Done** (`kernel/usb/xhci.cpp`). QEMU logs
`xhci: 0:3.0 1b36:d, 8 ports, 32 slots, 32-byte contexts`. Every controller
found is started, up to four.

### Phase 2: enumeration on root ports

Port reset with real delays, `Enable Slot`, `Address Device`, the device and
configuration descriptors, `SET_CONFIGURATION`. No class drivers yet: every
device is only logged. Log: `usb: port <n> <vid>:<pid> class <c> <speed>`.

**Done** (`kernel/usb/usb.cpp`). The log line carries the product string, and
every interface no driver claims gets a line of its own:

```text
usb: port 5 627:1 class 0 high-speed "QEMU USB Keyboard"
usb: 627:1 interface 0 class 3.1.1: no driver
```

QEMU puts `usb-storage` on a USB 3 port, so the stick enumerates at super
speed with a 512-byte endpoint 0.

### Phase 3: HID keyboard and mouse

Boot protocol, an interrupt endpoint per device, the usage table, LEDs. Then
the HID parser for `usb-tablet` and report-protocol devices.
Test: a smoke that types through `usbkbd` with QMP and moves `usbtablet`.

**Done** (`kernel/usb/hid.cpp`, the parser in `kernel/usb/hid_report.cpp`).
Keyboards use the boot protocol, with LEDs through `SET_REPORT` from `poll`
and a typematic of their own (500 ms, then every 33 ms), since a USB keyboard
does not repeat. Pointers read their report descriptor first, which gives a
mouse its wheel and a tablet its absolute axes; only a boot mouse whose
descriptor cannot be read falls back to the 3-byte boot report. Log lines:
`usb-hid: keyboard on <vid>:<pid>`, `usb-hid: mouse ...`,
`usb-hid: absolute pointer ...`. Three smokes, each on a machine with no
8042: `usb-kbd-smoke` (kbd-smoke's keys), `usb-pointer-smoke` (the tablet,
pointer-smoke's moves) and `usb-mouse-smoke` (taskbar-smoke driven by
`usb-mouse`). Booting without an 8042 found a `ps2` bug on the way: it
waited forever on a controller that is not there.

### Phase 4: mass storage

BOT plus the SCSI subset as a `block::Driver`. A stick present at boot shows
up in Disk Management and mounts at `/mnt` through the FAT driver.
Test: a smoke with `--usb-stick` that reads a known file and writes one back.

**Done** (`kernel/usb/umass.cpp`). A stick is `usb0`, `usb1`... with the
INQUIRY vendor and product as its model and the USB serial string as its
serial: `usb-storage: usb0 "QEMU QEMU HARDDISK" serial "SXUSB0", 32768 sectors`.
Stalls in the data phase are cleared and the CSW still read; a bad CSW or a
failed transport runs BOT reset recovery. A stick found at boot waits for
`block::probe_all()`, which would wipe an earlier registration; one found
later registers at once. `diskmgmt-smoke` now attaches a FAT stick and runs
the same mount, write, permission and remount checks on it as on the SATA
FAT disk. Checked by hand on a USB 2 only controller (`qemu-xhci,p3=0`) too:
QEMU otherwise puts `usb-storage` on a USB 3 port.

### Phase 5: hubs

`usb-hub` in QEMU. Real machines put ports behind internal hubs (webcams,
card readers, the Intel rate-matching hubs), so this is required before real
hardware, not optional.

**Done** (`kernel/usb/hub.cpp`). USB 2 and USB 3 hubs: the hub descriptor,
`set_hub` (the slot context's hub bit and port count, through Configure
Endpoint), `SET_HUB_DEPTH` on USB 3, port power and the power-good wait, then
every connected port reset and attached at once, so a keyboard or a stick
behind a hub is there before input and storage are probed. Devices behind a
hub get a route string, and a low- or full-speed one behind a high-speed hub
gets that hub's slot and port as its transaction translator (single TT; a
multi-TT hub is driven as a single-TT one). Log: `usb-hub: 8 ports on
409:55aa`, then `usb: port 5.1 ...` for a device on its port 1.
`usb-hub-smoke` runs kbd-smoke with the keyboard behind QEMU's `usb-hub`
(`--usb-hub`, catalog `usb_hub`). QEMU's hub is full speed, so the TT and
USB 3 hub paths are not exercised in QEMU.

### Phase 6: hot plug

Port status change events: attach a device after boot, and detach one. A
removed stick unregisters its block device; a mounted volume on it is released,
and what was not yet written is lost. QEMU's `device_add` and `device_del`
drive it from a smoke.

**Done.** Enumeration takes hundreds of milliseconds of waits, so it does
not run from the timer tick. `usb::poll()` (the tick, through the input
layer) only drains events and hands out keys and pointer moves;
`usb::service()` does the long work and runs where the disk writeback queue
drains, when a core is idle (the idle process's yield). A system busy on
every core therefore sees a plug only once a core frees up.

- A root port is acted on only when its **connect** change bit is set: the
  reset of a port raises a change event too, and acting on that would
  enumerate the device again forever.
- A hub's status change endpoint flags its ports; `service()` reads each
  flagged port's status, detaches what was there and attaches what is.
- A plug pushed in is debounced 100 ms before its reset.
- A pulled device is detached children first; its class drivers forget it.
  A pulled keyboard releases every key it held.
- A pulled stick goes through `storage::forget_disk()`: every volume on it
  or its partitions is released (`storage: <path> released, its disk is
  gone`), then `partition::drop()` and `block::unregister_device()`. If a
  file or a mapped section still holds a node of one of its volumes, the
  volume stays mounted with every I/O failing, and the release is tried
  again every second until it succeeds (`usb-storage: usb0 pulled out while
  in use; ...`). A stick's flush succeeds once it is gone, so a FAT volume
  can be released; an SxFS volume, whose release writes, cannot, and stays
  until reboot.
- A stick plugged in after boot registers at once and its partition table is
  read (`partition::rescan`).

`usb-hotplug-smoke`: on a machine with no 8042, `tools/qmp_usb_hotplug.py`
pulls the keyboard out, plugs a new one in, plugs a stick in and pulls it out,
checking the guest's log after each step (run_smoke gives callbacks the log
as `SAVANXP_SMOKE_SERIAL_LOG`); then it types kbd-smoke's keys, which only
the hot-plugged keyboard can deliver. Pulling a stick whose volume is mounted
and in use is not covered by a smoke.

### Phase 7: real hardware

The handoff on real firmware, vendor quirks from `xhci_pci.c`, slow devices,
and the one-keyboard rule fixed in the input layer. Checked on the machine,
with the boot log over serial or on screen.

**Prepared.** The ISO menu has a **boot log on screen** entry (`verbose`) and a
**no USB** one (`nousb`), so a machine without a serial port shows where USB
stops it, and still boots if it does (BUILD_LINUX.md). Booting the ISO from a
USB stick works in QEMU (OVMF booting `usb-storage`, no 8042): the root is the
`livecd` ramdisk, the stick itself shows up as `usb0` with the ISO's three
partitions, and nothing on it is mounted, since its EFI partition holds no
`boot/kernel.elf`. Scratchpad pages, which real controllers ask for by the
hundred, are allocated up to the specification's 512.

The real-hardware code from OpenBSD's `xhci_pci.c` (reference) is in:

- a controller the firmware left in D1-D3 is brought to D0 through its PCI
  power management capability, with the 10 ms D3 recovery
  (`xhci: <bdf> was in D3, waking it`);
- a controller whose registers read all ones is skipped
  (`xhci: <bdf> does not answer`), and the extended-capability walks stop on
  all ones instead of looping;
- the firmware gets 5 s, not 1, to release the controller;
- on Intel (8 Series and later) the ports the firmware allows are moved from
  EHCI to xHCI (`USB3_PSSEN` from `USB3PRM`, `XUSB2PR` from `XUSB2PRM`).

AMD's one quirk there, `XHCI_NOCSS`, is about saving controller state across
suspend, which this driver never does.

**Done: the run on the machine** (2026-11-06). The development machine, an
AMD 400-series chipset xHCI (`1022:43d5`) and a Zeppelin xHCI (`1022:145f`)
under UEFI, booted the ISO written with `dd` to a Kingston DataTraveler 3.0,
and its USB keyboard and mouse drove the desktop. That is the part QEMU
could not show: the firmware let go of the controllers, no PS/2 emulation was
left to carry the input, and the driver enumerated real devices.

Not seen on the machine: the boot log lines themselves (the run did not
capture them), the stick as `usb0`, hubs, and hot plug. Those paths are
covered in QEMU only. The machine also has four SATA disks on AHCI, which the
kernel does see; the run did not touch them.

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
- Whether EHCI is ever needed: the first target machine has only xHCI.
