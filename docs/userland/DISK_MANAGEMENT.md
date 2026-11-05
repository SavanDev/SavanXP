# Disk Management

Disk Management (`/bin/diskmgmt`, *System* in the Start menu) is the system's
tool for disks, partitions and volumes, in the shape of Windows 2000's
`diskmgmt.msc`. It is a SUN program (`system/diskmgmt`) and the consumer of the
storage calls (slice 28 in [KERNEL_NATIVE_API.md](../kernel/KERNEL_NATIVE_API.md)).
It shares its partition-table code, `libsxdisk`, with the planned installer
([SETUP_AND_UPDATES.md](SETUP_AND_UPDATES.md)).

## Status

| Piece | State |
| --- | --- |
| Disk model and serial, GPT metadata, GPT CRC checks in the kernel | done |
| `libsxdisk` (GPT read and write) and `sxfs_mkfs` | done |
| `SXN_SYS_STORAGE_SNAPSHOT` and the read-only window | done |
| Raw sector access, rescan; Initialize, New Partition, Format, Delete | planned |
| Mount and unmount | planned |

## The window

- **Top: the volume list.** One row per partition and per whole-disk volume
  (a disk or the LiveCD holding a file system with no partition table):
  Volume, Layout, Type, File System, Status, Capacity, Free Space, % Free.
  The volume is named by where it is mounted, else its label, else its device
  name in brackets.
- **Bottom: the disk map.** One row per disk: a grey box (*Disk N*, *Basic*,
  size, *Online* / *Not Initialized* / *Unreadable*; *LiveCD N* for the
  memory volume) and its segments in disk order. Each segment gets a minimum
  width and shares the rest by size, so a small partition stays clickable next
  to a large one. The header colour says what it is: dark blue a partition,
  teal a whole-disk volume, green free space, black an unallocated disk, dark
  red a damaged table. The selection is hatched, as in the original.
- **Free space is what libsxdisk would offer**: the map builds the table the
  kernel sliced and asks `sxdisk_gpt_free_extents`, so free means 1 MiB
  aligned and at least 1 MiB long, exactly what New Partition can use.
- A right click opens the menu of what is under the pointer; Alt+Enter or a
  double click opens Properties (device, model, serial, partition style and
  GUIDs, type, mount point and usage). The view refreshes every two seconds.

## Why calls, not `/dev/disk<n>`

The installer's plan asked for system-only `/dev/disk<n>` nodes. Device nodes
cannot carry it today: `device::Device` has `read` and `write` with no offset
and no per-device context, so a sector-addressed node would need the device
layer redesigned. And SUN has no file API, so a SUN program would reach the
nodes through `SXN_SYS_OPEN_DEVICE` and ioctls anyway. The storage calls are
one call per operation, shaped like the process snapshot, and the permission
check is the caller's token (`cred::acts_as_system`) instead of a node's mode.

Devices are named by an **id**, stable for the device's life and never reused,
not by their index in the block table: re-reading a disk's partitions removes
and adds slots. Every entry carries the storage **generation**, which moves
whenever a device or a mount changes.

## The SxFS partition type

GPT partitions holding SxFS use the type GUID
**`277C8079-900E-477E-99D8-4FEE1496D370`** (`sxdisk_type_sxfs`), chosen on
2026-11-05. It never changes: it is how setup will recognise an installed
system's root. Other types are shown by name when known (EFI System, Basic
data, Linux filesystem; the common MBR types) and as *Unknown* otherwise.

SxFS has no volume label. The label Disk Management shows and sets is the GPT
partition name (36 UTF-16 units, kept as ASCII).

## Rules for the operations that write

- Everything that changes a disk runs in a second copy of the program started
  through the elevation broker (`sxn_elevate`), which re-reads the snapshot
  and refuses if anything it was told no longer matches (the device's name,
  start or size).
- **The ESP, a disk holding `/`, and MBR disks are never written.** MBR tables
  are shown but not edited (no MBR writer, no extended partitions).
- **Initialize** is offered only for a disk with no partition table and nothing
  on it. A disk past 2 TiB (`SXN_STORAGE_TRUNCATED`) is refused: its backup
  GPT would land in the wrong place.
- SxFS v2 volumes stop at 1 GiB and 4096 files
  ([SXFS_ROADMAP.md](../fs/SXFS_ROADMAP.md)); a larger partition is formatted
  to its first 1 GiB, and the dialog says so.
- Every confirmation has *Cancel* as its default button.

## Tests

- `./build.sh test`: `partition-test` (the kernel reader, including a table
  libsxdisk wrote) and `sxdisk-test` (the library and `sxfs_mkfs`).
- `./build.sh smoke diskmgmt-smoke`: `diskmgmt --selftest` lists the devices
  and checks that ids are unique, partitions sit inside their disks, the root
  is mounted, flagged and adds up, and the map covers each disk without
  overlaps.
- `./build.sh smoke native-test`: the snapshot assertions of slice 28.
- `./tools/shoot.sh --scenario diskmgmt`: the window, the root volume drawn as
  the system's, and Properties.
