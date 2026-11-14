# Drivers, Plug and Play, and Device Manager

> **Status: open.** Native drivers, per-class registries and USB hotplug exist.
> A common device manager, general Plug and Play lifecycle and the Device Manager
> application below are planned, not implemented. The application is a SUN
> program inspired by Windows XP's Device Manager; it does not depend on the
> Win32 subsystem, MMC or Windows driver compatibility.

The goal is a common foundation for discovering hardware, binding drivers,
owning resources, submitting I/O and safely removing devices. Preserve the
existing class interfaces for blocks, display, audio, network and input. Share
identity and lifecycle mechanisms underneath them instead of making each new
driver implement its own version of Plug and Play.

## Current implementation

Drivers are compiled and linked into the kernel by `CMakeLists.txt`, registered
explicitly during `kernel/kernel_main.cpp`, and selected/enumerated through
interfaces of function pointers. There is no runtime driver-module loader,
external driver ABI or general device graph. `device::Device` is the small
read/write/ioctl/close interface used to publish `/dev` nodes, not a global
hardware manager. Not every hardware instance has a `/dev` node, and a class
node can aggregate several hardware instances.

| Layer / class | What exists |
| --- | --- |
| PCI | Boot-time enumeration into a flat table of 64 functions; IDs, class codes, BAR/capability helpers and configuration-space access |
| ACPI and architecture | uACPI namespace/routing integration, IOAPIC/legacy IRQ registration, MMIO mappings and `arch::` operations; parts remain PC/x86-shaped |
| Shared virtio transport | Modern virtio-pci configuration, feature negotiation, queues and MSI-X support shared by net, block, GPU, input and sound |
| Storage | All registered enumerators run; AHCI, ATA, virtio-blk, ramdisk, USB storage and partition devices coexist; block writeback is class-specific |
| Display | One selected backend by priority: virtio-gpu or the boot framebuffer |
| Audio | Multiple backends/instances, separate playback/capture defaults and endpoints: virtio-sound, AC'97 and HDA/azalia |
| Network | One selected NIC by priority: virtio-net or RTL8139 |
| Input | PS/2, virtio-input and USB HID report through the input core, with selection/coexistence policy |
| Filesystems | SxFS/FAT register format drivers; filesystem lookup, lifetime and mount contracts have their own VFS roadmap |
| Driver execution | Kernel-resident, with mixed IRQ/polling paths; storage and USB mainly poll, while PS/2, RTL8139 and virtio-gpu have interrupt paths |
| DMA | Physical/contiguous-page allocation, mappings and driver-specific handling; no general bus-DMA/IOMMU contract |

The architecture boundary is [`ARCH.md`](../../kernel/ARCH.md), shared transport
rules are in [`VIRTIO.md`](../../kernel/VIRTIO.md), and examples of existing
class contracts are `include/kernel/block.hpp`, `audio.hpp`, `display.hpp`,
`nic.hpp` and `input.hpp`. Existing recovery such as HDA's verified DMA stop is
useful groundwork, but it is not yet a lifecycle common to every class.

Drivers outside the kernel, the single API a driver is written against, and
the IOMMU work they need are a separate plan:
[DRIVER_FRAMEWORK_ROADMAP.md](DRIVER_FRAMEWORK_ROADMAP.md).

## Plug and Play already present and its limits

Boot-time discovery and initialization of supported devices is automatic.
Drivers built into the kernel can provide Plug and Play; loading executable
modules at runtime is a separate distribution feature, not a prerequisite.

USB already detects port changes, reads descriptors, configures devices and
offers interfaces to hub, HID and mass-storage drivers. It supports devices
behind hubs and connection/removal after boot. Newly attached keyboards and
pointers become usable; storage registers a block device and rescans partitions.
Mounting remains a Disk Management operation, not an automatic consequence of
plugging in a disk. Unsupported interfaces are reported as having no driver.

Preserve the limitations recorded in the completed
[`USB_ROADMAP.md`](../done/USB_ROADMAP.md#phase-6-hot-plug):

- Long enumeration/removal work runs from `usb::service()` when a core is idle;
  discovery latency is not bounded while every core stays busy.
- Removal detaches children first and releases keys held by a pulled keyboard.
- Storage removal tries to release volumes and partitions before unregistering
  the block device. Open files/mappings can keep the failed volume present while
  cleanup is retried. A removed SxFS volume can remain until reboot because its
  current release path requires writes to a device that no longer exists.
- `usb-hotplug-smoke` covers keyboard replacement and storage insertion/removal;
  removal of a mounted volume held by an open file/mapping is not covered.

PCI currently has boot enumeration, not a general hotplug/rebinding service.
There is no shared event stream, safe-removal policy, resource ownership or
suspend/resume protocol spanning all classes. Do not label those capabilities
complete because a USB device or one class-specific reset works.

## Responsibility and identity model

| Piece | Responsibility |
| --- | --- |
| Bus enumerator | Discover functions/interfaces, describe identity and topology, report arrival/removal; perform bus-specific configuration |
| Common device core | Track instances, relationships, states, references, resource ownership, driver binding and change generations |
| Hardware driver | Match and operate an instance through bus/resource APIs; start, quiesce, stop, recover and release supported hardware |
| Class core | Present block/audio/display/network/input operations and their class-specific selection/session policy |
| Sx / SUN management API | Validated snapshots, properties, capabilities, changes and authorized control operations |
| Device Manager | Display that API and request supported operations; no independent hardware probing or PCI/MMIO access |

Distinguish physical functions/controllers, child devices, USB interfaces and
class endpoints. Represent their relationships without showing an aggregate
`/dev/audio0` as a second physical sound card. Keep synthetic devices such as
ramdisks and the boot framebuffer identifiable as such. Filesystems and mounts
can refer to a backing device without pretending to be bus hardware.

An instance id is stable for that instance's lifetime and is never reused for a
different attachment. A reconnect creates a new attachment identity; a serial
number or physical path may provide a separate best-effort persistent identity,
with absence/duplication handled explicitly. Slot indexes, pointers and display
names are not control-operation identities. Snapshots and events carry a
generation so callers detect changes and obtain a consistent refreshed view.

Define states and transitions before freezing API structs: discovered, unbound,
starting, started, quiescing, disabled, failed and removed are the required
distinctions, not final ABI enum names. Keep reason/status and supported actions
separate. Unknown hardware, no matching driver, disabled hardware and a driver
that failed to start must not all appear as "working properly" or "no driver".

## D1 — common inventory and the first management API

**Open, first phase.** Add a shared device inventory fed by PCI, USB and current
class bindings. Include unbound hardware and supported synthetic instances;
registration must not require a successful driver start. Track parent/child and
endpoint associations with explicit budgets and cleanup. Agree ownership of
each current binding rather than reprobe working hardware just to list it.

Expose versioned, bounded snapshots and properties through Sx and typed SUN
wrappers. Include instance/parent ids, class, bus location, hardware IDs, reported
name, driver binding, state/reason and known capabilities/resources. Return
unknown/unavailable fields honestly. Copy validated data, not kernel pointers or
arbitrary memory. Reading properties must not resize BARs, reset devices or
write configuration space; use recorded resources for live-device inspection.

Device Manager's read-only window in D2 is the native consumer of this slice,
with `nativetest` assertions per [`SUN.md`](../../userland/SUN.md). Inventory
reads are available without elevation subject to defined property visibility;
mutating APIs are separate and enforce credentials in the kernel.
Ship D1 with the initial D2 window; the richer D2 views/properties can follow.

**Done when:** a native caller enumerates bound/unbound PCI functions, USB
topology and class associations with stable ids across refreshes; removal during
a read causes a defined retry/result rather than stale memory access. Table-full,
unknown IDs and missing fields have tests. No duplicated physical instances or
hardware writes are introduced by opening or refreshing the inventory.
The initial SUN Device Manager consumes the API; assertions alone do not close
the native API slice.

## D2 — a Windows XP-style Device Manager, read-only first

**Planned; consumes D1 and can ship before general driver lifecycle work.**
Proposed command/image: `devmgmt` / `/bin/devmgmt.sxe`, source under
`system/devmgmt/`. It is a C/SUN application using the existing SXGUI-C host,
desktop services and resource/icon conventions. Its title is **Device Manager**
and it appears in the Start menu's System group. This specifies the planned
program, not a command or path already installed.

Use the familiar XP management layout within SavanXP's desktop style:

- A menu bar and compact toolbar above a computer-rooted tree. Default to
  **Devices by type**; provide **Devices by connection** from real parent ids.
  Categories cover display, sound, network, input, storage and bus controllers,
  with **Other devices** for unidentified/unbound hardware. Populate real data,
  not a fixed list of presumed devices.
- Class/device icons and distinct overlays for a problem or disabled device;
  disconnected history is visually subdued when available. No driver is a
  problem state, not absence from the tree. Status text also conveys the state
  without relying on icon color.
- Properties opened by double click, Alt+Enter or the context menu. Planned
  tabs are **General** (type, location, status/reason), **Driver** (bound driver,
  built-in/module origin and version/provider only when recorded), **Details**
  (ids, parent, bus path and capabilities), and **Resources** (recorded IRQ,
  MMIO/port and other applicable assignments). Unsupported or unknown fields
  say so; do not fabricate Windows problem codes, vendors or driver dates.
- **Refresh** re-reads inventory and preserves selection by instance id. Reserve
  **Scan for hardware changes** for a supported bus-rescan operation; it must
  not be a renamed refresh or a call to boot-only initialization.
- Keyboard navigation, expand/collapse, context menus and properties remain
  usable while devices change. A removed selection becomes an explicit removed
  view or moves predictably; it must never silently select a reused slot.

Initial properties/actions are read-only. Do not present active update, rollback,
uninstall, disable or resource-editing controls before the corresponding kernel
operation exists. A built-in driver can be inspected without pretending its
binary can be uninstalled separately from the kernel.

**Done when:** deterministic inventory fixtures exercise categories, connection
view, unknown/failed/disabled/removed states and properties, with screenshots
and keyboard assertions against the rebuilt SUN image. A live QEMU scenario
shows current bindings and updates after USB insertion/removal without a stale
selection, crash or elevation prompt for normal viewing. Missing IDs or resource
information is visibly honest. Follow the repository's `sxgui-smoke` verification
rule for a drawing program change.

## D3 — instance binding, resources and lifecycle

**Open design work; builds on D1, independent of the D2 window.** Bind a driver
to a specific discovered instance rather than letting unrelated probes find the
same device independently. Define match priority, exclusivity and multi-function
ownership. Bus drivers enumerate; function drivers match instances; class cores
retain their existing default/selection rules. Start by adapting current drivers
incrementally; do not require every registry to be replaced in one change.

Introduce common start/quiesce/stop/remove/recover contracts, with references
retained by handles, class sessions, queued operations and callbacks. Failed
start rolls back only resources actually acquired. Define claims for MMIO,
I/O ports and interrupt routes, and provide release/synchronization operations
so no handler can execute against released state. Record assignments for D1's
inventory; detecting a conflict does not imply automatic resource reassignment.

**Done when:** competing probes cannot double-bind an exclusive function;
multiple supported instances remain independent. Inject failure at each start
stage and test repeated start/stop without resource leaks or callbacks into freed
state. Preserve existing display/network selection and multi-backend audio.
Unavailable operations have explicit capability/reason results.

## D4 — DMA and asynchronous I/O contracts

**Planned; consumes D3 ownership/lifetime.** Provide a bus-DMA API for address
masks, alignment/boundaries, coherent allocations, mapping/unmapping, segment
lists and synchronization; retain constrained contiguous/bounce-buffer paths
where hardware needs them. Architecture cache/barrier rules belong below driver
code. IOMMU isolation is a separate capability, not an implication of allocating
DMA pages or a promise that exists today.

Define common request ownership, submission, completion, timeout and cancellation
mechanisms, while preserving class-specific payloads and queues. Completion
must happen once, including timeout/removal races. Keep polling as a supported
implementation/fallback, and add IRQ/deferred work with bounded processing where
it pays. Waiting must not hold the global kernel lock through the whole device
operation; do not assume the current kernel can sleep on its syscall stack.

A driver may free/reuse DMA buffers only after device access has been stopped
and proved quiescent. If reset/stop fails, retain unsafe resources and report the
failed state rather than free memory the hardware may still write. Reuse the
lessons of current azalia recovery without making HDA-specific rules universal.

**Done when:** suitable real/fake backends exercise address constraints, mapping
failure, late completion, cancel/timeout/removal and repeated reset. No double
completion, freed-memory DMA or leaked request references occurs. Slow-device
I/O yields execution to other runnable work; class performance and durability
tests retain their guarantees.

## D5 — general Plug and Play and safe removal

**Planned; builds on D3/D4 and VFS lifetime/removal contracts.** Adapt USB's
working discovery/hotplug to the common graph and events; retain its class
drivers and hub topology. Schedule discovery/cleanup with defined budgets so a
fully busy machine cannot indefinitely starve it. Add other bus rescans/hotplug
only with supporting hardware and tests; a generic API must report unsupported
PCIe/SATA hotplug rather than imply it from USB support.

Provide both orderly removal and surprise removal. Orderly removal prevents
new opens/submissions, checks dependencies, flushes where applicable, quiesces
I/O and DMA, then releases children and resources. Surprise removal stops new
work and reports errors for lost I/O; it cannot flush nonexistent hardware or
claim lost writes became durable. Retain referenced objects until their final
users and completions finish, while detaching the absent device from use.

Coordinate with [VFS-B/E](VFS_ROADMAP.md) and SxFS/FAT so removed media does not
remain forever solely because normal unmount requires a write. Define a separate
device-gone teardown path that preserves error evidence and permits later disk
recovery. Reconnecting an apparently identical stick must not revive old handles
against its new attachment automatically.

Expose generation-based change notifications to SUN. Slow consumers can detect
overflow/missed events and refresh a snapshot; events are not the sole source
of truth. Device Manager and other consumers observe the same state and reasons.
Notification, driver matching and volume automount are distinct policies.

**Done when:** extend `usb-hotplug-smoke` with mounted FAT/SxFS media held by open
files and mappings, pending I/O, nested hub removal, repeated reconnect and full
CPU load. Removed devices fail predictably and eventually release after their
references end; no reboot is required just to free an absent volume. Unknown
interfaces remain visible. Event overflow, stale control ids and resource
exhaustion do not crash the kernel or affect unrelated devices.

## D6 — management actions and power transitions

**Planned; expose actions individually after lifecycle support.** Add supported
bus rescan, enable/disable, recovery/restart and orderly remove/eject operations
to the management API and Device Manager. Revalidate instance id, current state,
capability and caller credentials at execution, including elevated requests.
Disabling or removing a controller considers all descendants and active users;
protect the system disk/boot path and refuse unsafe operations with a concrete
reason. A disabled device's driver code may remain linked in the kernel.

Use the established SUN elevation broker for privileged actions, with the
requested instance/action visible and a refreshed result afterwards. Unsupported
actions remain unavailable; failures leave the actual state visible instead of
optimistically removing the row. A bus rescan must be safe at runtime, respect
existing bindings and not rerun `pci::initialize()` over live state.

Introduce per-device power transitions and coordinated suspend/resume only with
consumers and hardware coverage. Save/restore, parent ordering, wake sources,
DMA and pending-request behavior require explicit contracts; ACPI power-off or
bringing one xHCI controller to D0 does not establish general suspend support.

**Done when:** read-only users cannot mutate devices through the UI or direct
calls; stale ids, busy devices and unsupported operations return precise results.
Enable/disable/restart/rescan/removal tests preserve other devices and recover
from partial failures. Supported power transitions retain references, restore
working hardware and expose failures rather than a false success state.

## D7 — qualify the common driver foundation

**Planned; gates apply throughout D1-D6.** Add reusable fault-injecting tests for
discovery, matching, resource claims, request lifetime and removal; combine them
with class-specific integration and physical-hardware checks. Pin hardware and
workload coverage instead of inferring compatibility from an ID match. Existing
virtio changes still require VirtualBox queue bring-up as documented in
[`VIRTIO.md`](../../kernel/VIRTIO.md); QEMU smokes alone do not prove MMIO access
correctness. Retain USB and HDA real-hardware acceptance alongside automation.

**Done when:** repeated attach/start/fail/recover/remove cycles and concurrent
I/O/control queries have bounded resources and correct identities. Core changes
preserve boot, rescue, all existing classes and external installations. Run
`./tools/verify_doom_persistence.sh` for changes affecting persistent disks and
the required native/graphics smokes for exposed API/UI slices. Record tested
capabilities, remaining bus/class limits and diagnostic evidence.

User-mode driver hosts and IOMMU isolation are planned in
[DRIVER_FRAMEWORK_ROADMAP.md](DRIVER_FRAMEWORK_ROADMAP.md), which builds on D1,
D3 and D4. Runtime modules, stable external driver packaging/ABI and foreign
Linux/Windows kernel-driver compatibility are separate future projects. Driver update/rollback UI waits for a real packaging
and recovery mechanism; existing kernel/system updates are not an independent
driver installer. Initial Device Manager and general Plug and Play do not wait
for those projects.

## Dependencies and delivery order

| Work | Coordination |
| --- | --- |
| D1 + initial D2 | Native inventory API ships with its SUN Device Manager consumer; richer read-only views can follow before lifecycle replacement |
| D3 then D4 | Incremental common ownership and I/O under existing class interfaces; snapshots continue to work during migration |
| D5 | USB first, with VFS-B/E and SxFS/FAT device-gone handling; other buses require their own acceptance |
| D6 | Each control/power action ships only after its D3-D5 mechanisms, permissions and tests exist |
| D7 | Common qualification accumulated across phases, plus class/hardware regressions |

Device Manager is the common hardware inventory/control window.
[`DISK_MANAGEMENT.md`](../../userland/DISK_MANAGEMENT.md) continues to own
partitions, formatting and volume mounting; Sound owns audio endpoint choices.
Use stable device associations to link to those tools rather than duplicating
their operations. [Linux](LINUX_SUBSYSTEM_ROADMAP.md) and
[Win32](WIN32_SUBSYSTEM_ROADMAP.md) consume native mechanisms through their
translation layers; application compatibility does not grant access to arbitrary
device controls or imply support for foreign kernel drivers.
