# USB host controllers

The USB stack in `kernel/usb/` reaches devices through four controller
drivers: xHCI for machines from about 2012 on, and for the ones before it EHCI
(USB 2.0 high speed) with its UHCI or OHCI companions (USB 1.1 low and full
speed). HID, hubs and mass storage do not know which one a device hangs from.
The stack itself, its phases and the xHCI driver are in
[USB_ROADMAP.md](../roadmaps/done/USB_ROADMAP.md); this document is about the
controllers and what makes the older three differ.

| Driver | File | Chipsets | Speeds on its root ports |
| --- | --- | --- | --- |
| xHCI | `xhci.cpp` | Every PC from about 2012 | All |
| EHCI | `ehci.cpp` | 2002 to 2012, every vendor | High; low and full only behind a high-speed hub |
| UHCI | `uhci.cpp` | Intel (ICH0 to ICH10), VIA | Low and full |
| OHCI | `ohci.cpp` | AMD, ATI, NVIDIA, SiS, ALi, and Apple's in QEMU | Low and full |

All four are written from their specifications, with OpenBSD's drivers read as
reference and nothing copied ([THIRD_PARTY_ADOPTION.md](../third-party/THIRD_PARTY_ADOPTION.md)).
All four are polled, like every driver here: interrupts stay off, in the
controller and in the PCI command register.

## The controller table

`hc.cpp` holds up to 16 controllers, of any kind, and
`usb::hc::` is the one interface the core and the class drivers call. Each
driver fills a `ControllerOps` with its functions and adds each controller it
started with `hc::add_controller(ops, local)`; `Device::controller` is the
index in that table, and a driver finds its own controller with
`hc::local_index()`. Per kind there is room for 4 xHCI or EHCI and 8 UHCI or
OHCI, which covers a 2008 south bridge: six UHCI and two EHCI on ICH9 and
ICH10, five OHCI and two EHCI on AMD SB700.

`hc::initialize()` starts xHCI, then **EHCI before UHCI and OHCI**. Setting
EHCI's `CONFIGFLAG` routes every port to it; it then hands the devices that
are not high speed to the companions, which find them when their own ports
are scanned. That is also the order of the boot scan in `usb::initialize()`.

`Device::slot` is xHCI's device slot. The other three have no slots: there it
is the USB address, which `hc::assign_address()` picks per controller and
sends with `SET_ADDRESS` through the driver's own `control()`.

Every device line names its controller: `usb: uhci1 port 2 ...`, `usb: ehci0
port 3.1 ...`. The index counts controllers of that kind in PCI order.

## Taking the controller from the firmware

A legacy BIOS with "USB legacy support" drives the controllers itself from
SMM, to give a USB keyboard to DOS as a PS/2 one. Each driver takes its
controllers back, which ends that emulation:

| Driver | How | Log when the firmware does not let go |
| --- | --- | --- |
| xHCI | `USBLEGSUP` extended capability, 5 s | `xhci: the firmware did not release the controller` |
| EHCI | `USBLEGSUP` in PCI configuration space at `EECP`, 5 s; `USBLEGCTLSTS` cleared | `ehci: the firmware did not release the controller` |
| UHCI | `LEGSUP` (PCI configuration 0xC0) written `0x8f00` | — |
| OHCI | `OwnershipChangeRequest` while `InterruptRouting` is set, 500 ms | `ohci: the firmware did not release the controller` |

So once a controller starts, a USB keyboard on it is SavanXP's or nobody's.
`nousb` (the ISO's *no USB* entry) skips all four drivers and leaves the
firmware's emulation alone: the way to boot a machine whose controller the
driver cannot handle.

## Companion routing

An EHCI root port keeps a device only if it is high speed (EHCI 1.0, 4.2):

- before the reset, a line in the K state means a low-speed device;
- after the reset, a port that is not enabled means a full-speed one (only a
  high-speed device's chirp leaves it enabled).

Either way the driver sets the port's owner bit and logs `ehci: port 3:
full-speed device, handed to the companion`. The companion sees a connect
change on its own port and enumerates the device there, at boot or while the
system runs. When the device is pulled out the port returns to EHCI by itself.
A low- or full-speed device behind a high-speed hub stays on EHCI and is
reached with split transactions to the hub's transaction translator: the
queue head carries the hub's address (`Device::tt_slot`) and port.

## Schedules

The three older drivers share one shape:

- **One transfer at a time.** The kernel runs with interrupts off and makes
  one synchronous control or bulk transfer at a time, so the transfer
  descriptors, the 4 KiB control buffer and the 64 KiB bulk buffer are one set
  per driver, shared by its controllers.
- **Interrupt endpoints in fixed slots.** Each controller has 32 interrupt
  slots, each a queue head (UHCI, EHCI) or endpoint descriptor (OHCI) chained
  on the periodic schedule, with its own TD and a 64-byte buffer. Every frame
  points to the first, so an interrupt endpoint is polled every millisecond
  whatever its `bInterval`; a HID device that has nothing to say answers NAK.
  `usb::poll()` finds the finished ones and calls their callbacks.
- **DMA below 4 GiB.** UHCI and OHCI address 32 bits, and the EHCI driver
  keeps everything below 4 GiB too, with `CTRLDSSEGMENT` at 0. Their memory
  comes from `memory::allocate_contiguous_pages_below(count, kDma32Limit)`.

| | UHCI | OHCI | EHCI |
| --- | --- | --- | --- |
| Control and bulk | One queue head after the interrupt ones; its element pointer gets the TDs, linked depth first | One ED on the control list and one on the bulk list, reprogrammed while skipped, a frame waited after | One queue head linked after the reclamation head for the transfer, unlinked with the async advance doorbell |
| A TD carries | One packet | Up to a page | Up to five pages; bulk uses 16 KiB |
| Data toggle | Kept by the driver per endpoint | The ED's toggle carry, saved per endpoint | The queue head's overlay, saved per endpoint |
| A short IN packet | Short packet detect stops the queue | Rounding only on the last TD: earlier, a data underrun halts the ED | The alternate pointer terminates, which stops the queue |
| Control: short data stage | The status stage runs as a second step | Rounding on: the controller goes on to the status stage | The data qTD's alternate pointer is the status qTD |
| Port changes | The ports are read, at most every 20 ms | `RootHubStatusChange` | `USBSTS.PortChangeDetect` |

## Testing

`tools/run_qemu.py --usb-host` (catalog field `usb_host`) puts the USB devices
on another controller: `uhci` is a PIIX3 UHCI, `ohci` QEMU's `pci-ohci` with
four ports, `ehci` an ICH9 EHCI with its three UHCI companions at `00:1d`, as
on a Core 2 board, and `ehci-ohci` the same EHCI with two OHCI companions, as
on an AMD board. QEMU's plain `usb-ehci` takes no companions.

| Scenario | What it shows |
| --- | --- |
| `uhci-smoke`, `ohci-smoke`, `ehci-smoke`, `ehci-ohci-smoke` | kbd-smoke's keys on a keyboard behind a full-speed hub; on the two EHCI ones the hub is handed to a companion |
| `uhci-stick-smoke`, `ohci-stick-smoke`, `ehci-stick-smoke` | Disk Management's FAT stick: bulk transfers, at high speed on `ehci` |
| `ohci-hotplug-smoke`, `ehci-hotplug-smoke`, `ehci-ohci-hotplug-smoke` | usb-hotplug-smoke: a keyboard replaced and a stick plugged in and out while running; on EHCI the new keyboard is full speed and handed over at run time |

QEMU's keyboard, mouse and tablet attach at high speed to an EHCI port, so
`ehci-hotplug-smoke` also runs high-speed interrupt endpoints.

What QEMU cannot show, and real hardware will:

- **Split transactions.** QEMU has no high-speed hub, so a low- or full-speed
  device behind a transaction translator has never been run.
- **Low speed.** Every QEMU device is full or high speed. A real USB keyboard
  or mouse is usually low speed.
- **The firmware's handoff** on real BIOSes, and the chipsets' quirks: VIA's
  UHCI, AMD SB600/SB700 OHCI, NVIDIA's.

## Gotcha: a UHCI writes the TD before it moves the queue

When a TD completes, a UHCI writes the TD's status first and advances the
queue head's element pointer after. A driver that sees the TD done and puts
the next transfer on the same queue head at once can have its pointer
overwritten by the controller's late write, and the new transfer never runs.
Under KVM that is what happened to every second control stage on the ISO's
first boot (`usb: SET_ADDRESS 1 failed: timeout`). So `uhci.cpp` counts a
transfer, or an interrupt read, as finished only once the element pointer
also says the queue is empty; after a short packet or an error, which leave
the pointer on the TD, it clears the pointer, waits out the frame, and clears
it again. OHCI is read the same way, through the ED's head pointer, which the
controller writes last; EHCI takes the transfer queue head off the schedule
with the doorbell before reusing it.

## Not done

- Isochronous transfers, on any controller.
- Interrupt OUT transfers; an interrupt IN endpoint's packet is at most 64
  bytes.
- Clearing a hub's transaction translator buffer after an error on a split
  transaction.
- Suspend, resume and remote wakeup.
