# The driver framework: TKD, IUD and one driver API

Status: **planned, no phase landed.** Today every driver is a C++ file linked
into the kernel and registered by hand in `kernel/kernel_main.cpp`
([DRIVERS_ROADMAP.md](DRIVERS_ROADMAP.md#current-implementation)). There is no
IOMMU support, no driver outside the kernel, and no API a driver is written
against other than the kernel's own internals.

This page is the plan for two kinds of driver over one API, and for the IOMMU
work that has to come first. It builds on the common device foundation in
[DRIVERS_ROADMAP.md](DRIVERS_ROADMAP.md) (inventory, binding, DMA) and does
not replace it.

## The short version

- **Two kinds of driver.** A **TKD** (Trusted Kernel Driver) runs inside the
  kernel. An **IUD** (Isolated Userland Driver) runs as its own process and
  can crash, hang or be restarted without taking the kernel with it.
- **One API for both**, working name **SDF** (Savan Driver Framework), in the
  manner of Windows' KMDF and UMDF. A driver is written once against SDF and
  built for either host. Which host it runs in is a build decision, not a
  property of its source.
- **Isolation is a property of the driver, not a stage of its life.** A new or
  doubtful driver starts as an IUD. It may be promoted to a TKD after it
  passes the [promotion gates](#f5--promotion-and-qualification), and it may
  equally stay an IUD forever. "Experimental" is a separate label.
- **The IOMMU comes first.** Without one, a userland driver for a
  bus-mastering PCI device can still make the device write anywhere in
  physical memory, so its isolation would be in name only. Phases M1 to M4
  land before any SDF code.
- **The motivating device** is the Ryzen host's Wi-Fi card, a Realtek
  RTL8188CE (`10ec:8176`,
  [RYZEN5_1600_VEGA56.md](../../hardware/RYZEN5_1600_VEGA56.md)), whose Linux
  driver has a reputation for freezing the machine. It is the last phase
  here, not the first: it also needs an 802.11 stack and vendor firmware.

## What an IUD contains, and what it does not

The claim "an IUD cannot take the system down" is only as true as this table.
It is the reason for the order of the phases.

| Failure | Contained by |
| --- | --- |
| A bug in driver code: bad pointer, infinite loop, failed assertion, heap corruption | The process boundary. The supervisor ends and restarts the host |
| The device is programmed to DMA outside its buffers | The IOMMU domain (M3). Without an IOMMU: **not contained** |
| The device raises an interrupt it was never given (a forged MSI write) | Interrupt remapping (M4). Without it: **not contained** |
| An interrupt storm, or a host that never acknowledges | The kernel masks the source and leaves it masked until the host re-arms it |
| The host dies with DMA in progress | Bus mastering off, the domain emptied, the function reset; DMA memory is freed only once the device provably cannot reach it |
| A register read that never completes, or a link error that stalls the bus | **Nothing.** That is hardware; no privilege level helps |
| The device's own firmware hangs | The system stays up; the device is marked failed until it is reset or the machine restarts |

Consequences recorded as rules:

- **A confined IUD for a bus-mastering PCI function requires a per-device
  IOMMU domain and interrupt remapping.** On a machine without them the same
  driver may run only as an **unconfined** IUD: a machine-wide setting, off by
  default, changed only while elevated, and shown as such in Device Manager.
  An unconfined IUD still gains crash and hang containment for its own code;
  it is no safer than a TKD against its device.
- **A USB IUD needs no IOMMU.** It only submits transfers; the host controller
  driver, a TKD, does the DMA. That is the cheapest confinement there is, and
  it is why the USB client API has its own phase.
- **The boot path stays TKD.** An IUD starts after the root is mounted and the
  driver service is up. The system disk, the display, the keyboard and
  pointer that answer the elevation prompt
  ([USERS_AND_PERMISSIONS.md](../../kernel/USERS_AND_PERMISSIONS.md#elevation)),
  and everything the rescue system uses must have a TKD.

## Decisions

### One source, three hosts

A driver's source includes only the SDF headers (`include/sdf/`, working
path) and is compiled, unchanged, by three hosts:

| Host | Where the driver runs | What implements SDF |
| --- | --- | --- |
| Kernel host | Linked into the kernel: a TKD | Thin calls onto `pci::`, `vm::`, `memory::`, `iommu::`, the interrupt and timer code and the class cores |
| User host | A SUN process, one per bound device instance: an IUD | A library over the Sx device calls below, with a single-threaded event loop |
| Test host | The build machine, under CTest | A simulated device with scripted registers, interrupts and injected faults |

- SDF is a **C API** with C linkage. The kernel is C++ and SUN programs are
  C; both can compile it.
- Compatibility is **at the source level**. There is no stable driver binary
  ABI, and a TKD is still linked into the kernel statically. Runtime kernel
  modules and driver packages remain a separate project.
- The **test host is not optional**. It is where fault injection is cheap
  (a register that never becomes ready, an interrupt that never comes, a DMA
  allocation that fails), and it follows the repository's habit of testing
  parsers and cores on the host first.
- Driver source is a **freestanding subset**: no floating point (the kernel
  is built `-mno-sse`), no global constructors, no threads of its own, no
  libc beyond what SDF provides, no system call, no kernel header, and no
  inline assembly ([ARCH.md](../../kernel/ARCH.md)).

### The execution model: serialized and non-blocking

Two facts about today's kernel shape the whole API. Kernel code runs under
one global lock ([SMP_ROADMAP.md](SMP_ROADMAP.md)), and it cannot sleep on its
system-call stack ([DRIVERS_ROADMAP.md](DRIVERS_ROADMAP.md#d4--dma-and-asynchronous-io-contracts)).
A model that needed either to change would tie the framework to the lock
split.

- **SDF serializes every callback of one device instance.** Start, stop,
  interrupt, timer, deferred work and class requests never run concurrently
  for the same instance. The kernel host gets that from the global lock
  today and from a per-instance lock later; the user host gets it from its
  event loop. A driver takes no lock of its own by default.
- **Callbacks do not block.** A driver expresses a wait as a timer or a
  completion, not as a sleep. Hardware-mandated short delays use a bounded
  busy wait.
- **Blocking work is a declared capability.** A driver that needs to sleep
  (a port of a driver written around `tsleep`) declares it. The user host
  always offers it; the kernel host does not until the kernel has worker
  contexts that may sleep. A driver that declares a capability the kernel
  host lacks **cannot be built as a TKD**, so that promotion gate is checked
  by the compiler rather than by review.

### The mechanism is the kernel's, the policy is a service's

- The **kernel** owns: the device claim, mapping a function's regions into a
  process, interrupt delivery as waitable objects, DMA memory and the IOMMU
  domain, the function reset, and the teardown when a claim's last handle
  closes. It never decides which driver a device gets.
- A SUN system service, working name **`driverd`**, owns: matching instances
  from the inventory
  ([D1](DRIVERS_ROADMAP.md#d1--common-inventory-and-the-first-management-api))
  against the installed IUDs, starting one host process per bound instance,
  restarting it with backoff, giving up after repeated failures, and
  reporting state. It is started by init and takes a port name under `sys.`.
- An IUD is an SXE image under `/system/drivers/` (working path). Its match
  table and declared capabilities are data in the image, read by `driverd`
  without running it.
- A host is started with **handles, not ambient authority**: the claim and
  its class channel, passed at spawn (`SXN_SYS_SPAWN_EX`). It must not be
  able to open raw devices, write system files or claim a second device.
  Whether that needs a dedicated identity beside the user and the system is
  [open](#open-questions).

### The class core never waits for a host

A class core (`nic::`, `block::`, `audio::`) is called under the global lock
and must stay correct when the host on the other side is dead.

- The kernel end of an IUD is a **class proxy**: an ordinary class driver
  (`nic::Driver` and its `Nic` table, for the first one) whose operations
  only enqueue to, and drain from, rings in a section shared with the host.
  A full ring is a failed or dropped request, never a wait.
- Every request the proxy accepted **completes exactly once**, with an error
  if the host went away.
- The classes must learn that a device can **arrive after boot and leave at
  any time**. `nic::bind_best()` chooses one NIC during boot and keeps it;
  an IUD NIC appears later and disappears on every restart. That change to
  `nic::` and `net::` is a prerequisite of F2, not a detail of it.
- Legacy drivers keep their class interfaces. Nothing is rewritten in bulk:
  a driver moves to SDF in its own commit, when it is touched or when it is
  wanted as an IUD.

### Promotion is optional and reversible

- A driver's mode is one entry in the build (working design: one CMake list
  of TKDs, one of IUDs). Promotion moves the entry; the source does not
  change.
- Some drivers are **IUD by design** and are never candidates: anything that
  links a large stack or needs files, such as an 802.11 driver. They declare
  it (`SDF_CAP_USERLAND_ONLY`), which also lets them use SUN directly.
- **Demotion is the same move backwards.** A TKD that turns out unstable on
  some machine goes back to being an IUD without being rewritten. Running a
  TKD's source as an IUD to diagnose it is the same mechanism.

## The IOMMU phases

The kernel gains an `iommu::` layer with one API over two implementations.
AMD-Vi comes first: the motivating card is in an AMD machine. The work is
independent of the SDF phases and of
[DRIVERS_ROADMAP.md](DRIVERS_ROADMAP.md) D1 to D4, and can start now.

Which targets have a usable IOMMU is not known for any of them: it depends on
the chipset and on a firmware setting. Finding out is the first gate.

### M1 — discovery and report

Read-only; no behavior change.

- Parse the ACPI **IVRS** (AMD-Vi) and **DMAR** (VT-d) tables with the
  bounded readers of `acpi_tables`: firmware is a hostile input
  ([KERNEL_SECURITY.md](../../kernel/KERNEL_SECURITY.md#acpi-table-walk)), so
  every length, device range and address is checked, and the parser is tested
  on the host against tables built to be wrong.
- Record per unit: its register base, the device ranges and aliases it
  covers, the reserved memory ranges the firmware demands stay mapped (IVMD,
  RMRR), whether it supports interrupt remapping, and whether the firmware
  left it enabled.
- Boot log: `iommu: AMD-Vi, 1 unit, interrupt remapping available` or
  `iommu: none`.

**Done when:** QEMU with an emulated `amd-iommu` and with `intel-iommu` each
report correctly, a machine without one reports `none`, and the malformed
tables are refused by name. Each verified target's hardware page records what
it reports with the firmware's IOMMU setting on.

### M2 — AMD-Vi on, one identity domain

Turn translation on without changing what any driver sees.

- Build the device table, the command buffer, the event log and the I/O page
  tables; take the unit over from the firmware whatever state it left.
- Every device goes into one shared **identity domain** covering usable RAM
  plus the firmware's reserved ranges, so existing TKDs keep handing physical
  addresses to their devices unchanged.
- I/O page faults are read from the event log and written to the system log
  with the offending function and address.
- Opt in at first with an `iommu` boot option; `noiommu` always wins. It
  becomes the default per target, as each is verified.

**Done when:** the core, network, audio, USB and disk smokes pass in QEMU
with `amd-iommu` and the option on; a Ryzen run with the firmware's IOMMU
enabled is recorded on its hardware page; a fault is reported and survived;
`./tools/verify_doom_persistence.sh` passes.

### M3 — per-device domains and the `iommu::` API

The piece the framework stands on.

- `iommu::` gains: create and destroy a domain, attach a function to it, map
  and unmap ranges of **device addresses** to physical pages, flush after an
  unmap, and a **blocked domain** with no mappings at all.
- A domain covers an **isolation group**: the functions the unit cannot tell
  apart (a conventional PCI device behind a PCIe bridge, a function with
  aliases). A group is attached as a whole or not at all.
- An unowned, failed or removed function moves to the blocked domain. That is
  what makes freeing DMA memory after a failed stop provable instead of
  hopeful.
- A fault is delivered to the owner of the domain as well as logged.
- Devices with 32-bit DMA can be given low device addresses for high pages.
  That removes the need for `allocate_contiguous_pages_below` on such
  machines; it is a benefit, not a goal of this phase.

**Done when:** a TKD moved into its own domain works unchanged; the same
driver with one of its buffers deliberately left unmapped produces a named
fault, the kernel stays up and the device ends in the blocked domain; host
tests cover the page-table and group arithmetic.

### M4 — interrupt remapping

Without it a device that can write to the MSI address window can raise any
vector on any core, IOMMU domain or not.

- Route MSI, MSI-X and IOAPIC interrupts through the unit's remapping table;
  a function may raise only the entries it was given.
- This also removes the limit recorded in
  [SMP_ROADMAP.md](SMP_ROADMAP.md): a core with an APIC ID above 254 can then
  receive device interrupts.

**Done when:** every smoke of M2 passes with remapping on; a function that
raises an entry it does not own is refused and reported; the SMP smokes keep
their interrupt distribution.

### M5 — VT-d

The same `iommu::` API over Intel's DMAR units, tested with QEMU's
`intel-iommu` and on whichever Intel target reports one. It can land any time
after M3 and gates nothing on the Ryzen.

## The framework phases

These consume the foundation of [DRIVERS_ROADMAP.md](DRIVERS_ROADMAP.md): D1
for instance identities, D3 for exclusive claims and the lifecycle states, D4
for the DMA contract. SDF is the form those contracts take when driver code
sees them, so the two are designed together and neither defines them twice.

### F1 — SDF and the kernel host

- The headers, the kernel host and the test host.
- **The consumer is the RTL8139**: its driver is rewritten against SDF and
  stays a TKD. It is small, QEMU emulates it, and its smokes already exist,
  so a regression shows at once.
- Needs at least the exclusive claim of D3, so an SDF driver and a legacy
  probe cannot both take one function.

**Done when:** the network smokes pass on the SDF RTL8139; the same source
passes the test host's suite, including injected failure at every start
step; the legacy `rtl8139.cpp` is gone.

### F2 — the Sx device surface, the user host and `driverd`

- The Sx calls in [The Sx surface](#the-sx-surface-under-the-user-host), their
  SUN wrappers with `nativetest` assertions
  ([SUN.md](../../userland/SUN.md)), the user host library, `driverd`, and the
  `nic::` class proxy.
- **The consumer is the same RTL8139 source, built as an IUD.** A build
  switch picks the host; nothing else differs.
- Needs M3 and M4, and a `nic::`/`net::` that accepts a NIC arriving late and
  leaving.

**Done when:** a QEMU smoke with an emulated IOMMU boots, `driverd` starts the
RTL8139 host, and the network smokes pass through it; the TKD build still
passes the same smokes; throughput in both modes is measured and recorded,
with no promise attached.

### F3 — supervision and containment

Each row of [the table above](#what-an-iud-contains-and-what-it-does-not)
becomes a test.

- The host is killed mid-traffic, loops forever, stops acknowledging its
  interrupt, programs a DMA address outside its domain, and exits during
  start. Each time: the kernel stays up, pending requests fail once, the
  function is reset and blocked, the host restarts, and the network comes
  back without a reboot.
- A host that keeps failing is given up on with a reason that the inventory
  and Device Manager show: mode (TKD, confined IUD, unconfined IUD), state,
  restart count, last fault.
- A device that cannot be reset stays blocked and is reported failed; its DMA
  memory is kept rather than freed under a device that may still write.

**Done when:** all of those are smokes, and none leaks a claim, a domain, a
section or an interrupt route across twenty restarts.

### F4 — firmware files and the USB client API

Two additions, each landing with its consumer and not before.

- **Firmware:** `sdf_firmware_load` reads an image by bare name from
  `/system/firmware/` (working path) and nowhere else. A TKD that needs one
  cannot start before the root is mounted.
- **USB:** claim an interface and submit control, bulk and interrupt
  transfers from an IUD, with the host controller drivers unchanged. The USB
  stack's own class drivers (`kernel/usb/usb_internal.hpp`) are untouched.

### F5 — promotion and qualification

A driver moves from the IUD list to the TKD list only when all of these hold,
and the evidence is recorded with the change:

- it builds for the kernel host, so it declares no capability that host
  lacks;
- it passes the test host's suite, fault injection included;
- as an IUD it completed a recorded soak on real hardware with no restart, no
  IOMMU fault and no masked-interrupt timeout;
- repeated start, stop and reset leak nothing;
- it is not on the boot path by accident: a TKD starts before the root, and
  its start must not depend on files.

## Wi-Fi: the RTL8188CE

A project of its own, listed here because it is why the framework exists and
because it fixes what the framework must provide.
[REAL_HARDWARE.md](../../hardware/REAL_HARDWARE.md) calls wireless out of
scope; that stays true until this starts.

- The card is a softMAC device: scanning, association, rate control and the
  WPA handshake all happen in the host's software. OpenBSD drives it with
  `rtwn(4)` over its `net80211`, and this tree already ports from OpenBSD
  ([AZALIA.md](../../kernel/AZALIA.md)); that is the candidate source, pinned
  and mapped the same way.
- The driver and the 802.11 stack are **one IUD, userland-only by design**.
  It presents an Ethernet-shaped interface to `net::` through the same proxy
  as the RTL8139, plus a control channel (scan, join, status) for a SUN Wi-Fi
  tool.
- It needs from SDF: MMIO, MSI or INTx, DMA rings, timers, firmware loading,
  blocking work, and files for the saved network.
- The firmware image is Realtek's. Its redistribution terms are checked and
  recorded under `docs/third-party/` before it ships in an image; until then
  the user supplies the file.
- Order: the chip comes up and reports a scan; an open network carries
  traffic; WPA2-PSK; the tool. Each is verified on the Ryzen, since QEMU does
  not emulate the card.

## What the common API has to provide

The list is the analysis of what a driver in this tree already does by hand
(`rtl8139.cpp`, `ahci.cpp`, the virtio transport, the Azalia port and its
`bus_space`/`bus_dma` replacement in `kernel/azalia/azalia_port.hpp`), turned
into calls that have an implementation in all three hosts. Names are working
names; the areas are the commitment.

### Declaration, matching and lifecycle

| Piece | Contract |
| --- | --- |
| Driver descriptor | Name, version, the SDF version it was written for, device class, match table, declared capabilities, callback table |
| Match table | PCI: vendor, device, subsystem, class code, revision range. USB: vendor, product, class, subclass, protocol. Data only, readable without running the driver |
| `probe` | "Is this instance mine, and how well?" No side effects on the device. The best match wins; ties are refused, not guessed |
| `start` | Acquire resources and bring the device up. A failure rolls back exactly what was acquired; the host does it, not the driver |
| `quiesce` | Stop new work and stop the device's DMA and interrupts. Must be provable: it returns only when the device is quiet, or fails |
| `stop` / `remove` | Orderly release, or surprise removal where the hardware is already gone and nothing may touch it |
| `recover` | Reset and restart in place after a detected fault |
| Power | `suspend` and `resume` slots reserved in the table and unused until [D6](DRIVERS_ROADMAP.md#d6--management-actions-and-power-transitions) defines them |

The states are those of
[D3](DRIVERS_ROADMAP.md#d3--instance-binding-resources-and-lifecycle), not a
second set.

### Bus resources

- **Configuration space:** read anything of the driver's own function. Writes
  are filtered: the command register, the BARs, the MSI and MSI-X
  capabilities and power management belong to the host. Bus mastering is
  enabled by the host when DMA is first allocated and disabled at quiesce.
- **Memory regions:** map a BAR and get an accessor; `read8/16/32/64` and the
  matching writes. The accessors carry the rules of
  [VIRTIO.md](../../kernel/VIRTIO.md): a 64-bit register is two 32-bit
  halves, low first, unless the driver asks otherwise. Mapped uncached and
  never executable. A page that holds the MSI-X table is not mapped writable
  into an IUD.
- **Port I/O:** the same shape. In the user host each access is a checked
  system call, which is slow and acceptable for the legacy devices that need
  it. Using it marks the driver PC-only ([ARCH.md](../../kernel/ARCH.md)).
- **No physical addresses, no other function's registers, no BAR
  reassignment.** A driver sees handles to its own resources.

### Interrupts and polling

- Request MSI-X, MSI or INTx, in that order of preference, and get a callback
  per vector. The callback runs serialized like every other; the kernel host
  may call it from the interrupt path, the user host calls it from its loop.
- The contract is **mask, deliver, re-arm**: the source is masked when the
  interrupt fires and stays masked until the driver re-arms it, so a storm
  costs one delivery. A source left masked past a deadline is a fault.
- INTx for an IUD uses the function's own Interrupt Disable and Interrupt
  Status bits to quiet and identify it, since the kernel cannot acknowledge a
  device it does not understand. A function that lacks them and shares its
  line with a TKD cannot be an INTx IUD.
- **Polling is first class.** Much of this tree polls (storage, USB, Azalia's
  DMA position). A periodic timer callback is a supported driver shape, not a
  fallback.

### DMA

- Allocate device-visible memory with constraints: address width, alignment,
  boundary, contiguity. The result is a CPU pointer and a **device address**,
  which is an IOMMU address when there is a domain and a physical one when
  there is not. A driver never learns which.
- The memory belongs to the instance, is mapped only into its domain, and is
  the only memory the device can reach.
- Mapping a buffer the driver did not allocate (zero-copy) is left out until
  a consumer needs it. Until then data crosses by copy.
- Synchronization calls exist and do nothing on x86, so a second architecture
  does not change driver source.
- **Free only after quiesce.** If quiesce failed, the host keeps the memory
  and reports the instance failed
  ([D4](DRIVERS_ROADMAP.md#d4--dma-and-asynchronous-io-contracts)).

### Time, deferred work and memory

- Monotonic time in nanoseconds; one-shot and periodic timers.
- A bounded busy wait for the microsecond delays hardware manuals demand,
  capped by the host so it cannot become a hang.
- Deferred work: "call me back soon, serialized, outside the interrupt
  callback". Completions for multi-step sequences.
- Blocking work, only under the declared capability.
- A small allocator with a per-instance budget, and the memory and string
  primitives. Nothing else from a C library.

### Diagnostics

- Leveled logging into the system log, attributed to the driver and the
  instance ([SYSTEM_LOG.md](../../kernel/SYSTEM_LOG.md)).
- Named counters the inventory can read: restarts, faults, dropped requests.
- A driver reports a fault it detects itself, which triggers the same path as
  one the host detects.

### Class interfaces

How a driver offers its function upward. One per class, each added with its
first consumer, each a thin image of the class core that already exists.

| Class | SDF side | Kernel side today | First consumer |
| --- | --- | --- | --- |
| Network | Register an Ethernet-shaped interface: address, link state, a transmit callback, a receive call, statistics | `nic::Nic`, `nic::Events` | RTL8139 (F1) |
| Block | Sector read, write, flush; identity; deferred-write hint | `block::DeviceOps`, `DeviceIdentity` | When a storage driver moves |
| Audio | Streams, formats, position, jack state | `audio::Backend` | When an audio driver moves |
| Input | Keyboard and pointer reports | `input::Driver` | When an input driver moves |
| Display | Not planned as an IUD: it is on the boot path | `display::Backend` | — |

A driver may also offer a **private control channel** to its own SUN tool, as
the Wi-Fi driver must. It is a port the host creates, not part of a class.

### Versioning and what is refused

- The descriptor carries the SDF major and minor version. A host refuses a
  driver written for a newer major. The Sx calls under the user host are
  versioned separately and are not part of what a driver sees.
- A driver that declares `SDF_CAP_USERLAND_ONLY` may call SUN; no other
  driver may make a system call.
- There is no way to disable interrupts, take a kernel lock, map arbitrary
  memory, or reach another instance.

## The Sx surface under the user host

What the kernel adds for IUDs. Numbers and exact structs are assigned when
F2 lands; each call gets its SUN wrapper and `nativetest` assertion.

| Object or call | What it does |
| --- | --- |
| Device claim | A handle to one inventory instance, exclusive. Creating it needs the system. Closing its last handle runs the teardown |
| Configuration read and write | The filtered access described above |
| Region map | A device-memory section for one BAR, mapped as a view: uncached, read/write, never executable |
| Port access | A checked read or write within the function's I/O BARs |
| Interrupt object | Waitable, signaled when the vector fires; a re-arm call |
| DMA section | Pages mapped into the process and into the claim's IOMMU domain, with the device address returned |
| Reset | Function-level reset, with the fallbacks the function supports |
| Class channel | The shared rings and events that connect the host to its class proxy |

The teardown is the important part, and it runs in the kernel because the
process may already be dead: interrupts masked and unrouted, bus mastering
off, the function moved to the blocked domain, a reset attempted, then the
regions unmapped and the DMA pages freed.

## Order of work

| Step | Needs | Notes |
| --- | --- | --- |
| M1, M2 | Nothing | Can start now; M2 is where a real-hardware boot regression would show |
| M3, M4 | M2 | The gate for any confined PCI IUD |
| D1, D3 (claims), D4 (DMA) | [DRIVERS_ROADMAP.md](DRIVERS_ROADMAP.md) | Designed together with SDF's resource and DMA calls |
| F1 | D3 claims | SDF as a TKD only; no IOMMU needed yet, but the DMA calls are shaped by M3 |
| F2 | F1, M3, M4, late-arriving NICs | The first IUD |
| F3 | F2 | Containment proved, not assumed |
| M5 | M3 | Intel machines; independent of the rest |
| F4, Wi-Fi | F3 | Firmware first; the USB client API when a USB driver wants it |
| F5 | F3 | Applies to each driver separately, whenever it is ready |

The Ryzen's Ethernet (HW-1, an RTL8168) is not held back by any of this. If
it is written before F1 it lands as a legacy TKD and moves later; if after,
it is written against SDF from the start.

[KERNEL_SECURITY.md](../../kernel/KERNEL_SECURITY.md) gains the IUD trust
boundary when F2 lands: what a compromised host can reach (its device, and
through a NIC the traffic it carries) and what it cannot.

## Open questions

- **The host's identity.** A driver host should be weaker than the system:
  no system files, no raw devices. The token model has a user and the system
  and nothing in between. A third identity, or a handle-only process with no
  file access at all, is undecided.
- **How many instances per host.** One process per instance is the simplest
  to supervise. A multi-port device may want one process for all its
  functions; decide with the first such driver.
- **The unconfined mode's reach.** Whether it should exist outside
  development builds at all, given that most of the older targets
  ([REAL_HARDWARE.md](../../hardware/REAL_HARDWARE.md)) are unlikely to have
  an IOMMU.
- **Graphics.** A GPU driver is the classic case for isolation and also the
  one on the boot path. Nothing here plans for it.

## Out of scope

- Runtime kernel modules, a stable binary driver ABI, driver packages,
  update and rollback.
- Running Linux or Windows kernel drivers.
- Device passthrough to virtual machines, the other customary use of an
  IOMMU.
- Suspend and resume, until D6.
