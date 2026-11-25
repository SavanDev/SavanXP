# Device Manager

Device Manager (`/system/devmgmt.sxe`, *System* in the Start menu) shows the
computer's hardware, the driver using each piece and its state, after Windows
XP's `devmgmt.msc`. It is a SUN program (`system/devmgmt`), the consumer of the
[hardware inventory](../kernel/DEVICE_INVENTORY.md), and the initial D2 window
of [DRIVERS_ROADMAP.md](../roadmaps/open/DRIVERS_ROADMAP.md). **It is read-only:**
the kernel has no operation to disable, update or remove a driver yet, so the
window offers none.

## The window

A menu bar (File, Action, View, Help), the tree, and a status bar with the
device count, how many have no working driver, and the inventory's generation.

- **Devices by type** (the default): the computer, then the groups that have
  something in them, alphabetically, each device sorted by name inside its
  group. A group opens by itself the first time it shows if a device in it has
  a problem; after that it stays as the user left it.
- **Devices by connection**: the computer, then every device under what it
  hangs from (bridges, controllers, hubs, USB devices), every branch open.

Mouse: a click selects, a click on the box opens or closes, a double click
opens a group or shows a device's Properties, the right button opens the
row's context menu (Expand/Collapse, Refresh, Properties). Keys: arrows,
Home, End, Page Up and Down, Left and Right to close, open and climb, `+` and
`-`, Enter (a device's Properties, a group open or closed), Alt+Enter, F5,
the menu key and Shift+F10.

The window re-reads the inventory every second and rebuilds only when its
generation changed. **The selection is kept by instance id, never by row**:
a device that went away leaves its selection on its group (by type) or on
what it hung from (by connection), and the status bar says it was removed. An
id is never reused, so the instance that takes its place is never selected in
its stead.

## Where a device goes

| Group | Holds |
| --- | --- |
| Disk drives | USB mass storage, the LiveCD's RAM disk |
| Display adapters | display controllers, the firmware framebuffer |
| Human Interface Devices | input that is neither a boot keyboard nor a boot mouse (QEMU's tablet) |
| Keyboards, Mice and other pointing devices | PS/2, USB boot protocol, PCI input controllers |
| Network adapters, Sound, video and game controllers, Storage controllers | by class |
| System devices | bridges, SMBus, IOMMU, the virtio entropy source |
| Universal Serial Bus controllers | host controllers, hubs, composite USB devices |
| Other devices | what has no known class |

A device **without a driver stays in its group** with a problem mark: having
no driver is a state, not a kind of hardware, and the class codes still say
what it is. A USB device with a single interface is shown as that interface,
under the device's name, as Windows does; a composite one is listed under the
USB controllers and each of its functions in its own group.

Marks over the icon, drawn rather than loaded so a missing icon file never
hides a problem: a yellow `!` for a device that failed to start, a `?` badge
for one no driver is using. The status sentence in Properties says the same
in words.

## Names

What the hardware reports wins (a USB product string, an NVMe model, the
RTL8168 revision, an HD Audio codec). Otherwise the name is built from the
IDs: the vendor from a short table of PCI vendor ids followed by what the class
codes say ("Intel SATA AHCI controller"), "VirtIO …" from the virtio device
type, "Generic USB hub", "USB composite device", "USB keyboard". An unknown
function says what is known of it: "PCI device (class ff.00)", "USB interface
1 (class ff)". No vendor, product or problem code is made up.

## Properties

Four tabs of what the record holds; a field that was not recorded says so.

- **General**: the name, its group, the manufacturer (USB string, vendor table,
  or "Not reported"), the location, the state, and one or two sentences on what
  the state means and why (working; cannot start; no driver, with the reason:
  none took it, `nousb`, nothing supported on it).
- **Driver**: the built-in driver's name or None, "Built into the kernel";
  provider, version and date "Not recorded", since the kernel records none.
- **Details**: the bus, instance and parent ids, the hardware ids (PCI
  vendor:device, subsystem, class code and revision; USB vendor:product,
  class, versions, interface number, speed) and the flags.
- **Resources**: memory and I/O ranges and IRQs as recorded. A range its
  driver claimed shows where it ends, and one used by other devices too says
  "(shared)". A BAR no driver claimed has no recorded size (reading it writes
  to the device), so it shows its base, and the tab says why.

The sheet shows a copy of the record taken when it opened, so it stays
readable if the device goes away meanwhile. Its title is "`<name>`
Properties", ending in "..." when the frame is narrower than it.

## Icons

House icons in `/icons`, emitted by `tools/gen_mime_icons.py` (`HOUSE_ICONS`):
`device-network`, `device-storage`, `device-usb`, `device-system`,
`device-hid` and `device-unknown` are drawn for it; the computer, display,
sound, keyboard, mouse and disk reuse the system's `desktop`, `app-display`,
`app-sound`, `app-keyboard-settings`, `app-mouse` and `drive-harddisk`. The
program's own icon is `app-devmgmt`.

## Tests

- `./build.sh smoke devmgmt-smoke`: `devmgmt --selftest usb`. First the model
  against fixtures holding every state and placement (an unknown PCI function,
  a failed NIC, a display without a driver, a single-function and a composite
  USB device, a hub, a controller kept by `nousb`): both views list each device
  once and every child under its parent, names and status sentences, groups
  with problems open, the selection surviving a rebuild, a removed selection
  never moving to the instance that took its slot, collapsing and expanding.
  Then the live inventory through both views and every tab of every device,
  with QEMU's USB keyboard and mouse in their groups.
- `./tools/shoot.sh --scenario devmgmt`: the tree by type with the computer
  selected and its groups, View > Devices by connection, then the AHCI
  controller's Properties on General ("working properly") and Resources.

## Pending

- A compact toolbar under the menu bar (Properties, Refresh, the two views).
- A live scenario that plugs and pulls a USB device while the window is open
  (the model's removal handling is covered by the fixtures; `usb-hotplug-smoke`
  covers the kernel side).
- **Scan for hardware changes** waits for a bus rescan operation; it is not to
  be a renamed Refresh.
- Disconnected-device history, and every control action (disable, update,
  uninstall, resource editing), wait for D3 to D6 in the kernel.
- A POSIX program reading the same list has `device_snapshot()`; there is no
  POSIX Device Manager, and none is planned.
