# Disk Management

Disk Management (`/bin/diskmgmt`, *System* in the Start menu) is the system's
tool for disks, partitions and volumes, in the shape of Windows 2000's
`diskmgmt.msc`. It is a SUN program (`system/diskmgmt`) and the consumer of the
storage calls (slice 28 in [KERNEL_NATIVE_API.md](../kernel/KERNEL_NATIVE_API.md)).
It shares its partition-table code, `libsxdisk`, with the planned installer
([SETUP_AND_UPDATES.md](SETUP_AND_UPDATES.md)).

The planned SUN Device Manager in
[`DRIVERS_ROADMAP.md`](../roadmaps/open/DRIVERS_ROADMAP.md) owns general hardware
inventory and supported device lifecycle actions. Disk Management keeps disk,
partition and volume operations; the two tools will share stable associations
rather than duplicate formatting or mounting controls.

## Status

| Piece | State |
| --- | --- |
| Disk model and serial, GPT metadata, GPT CRC checks in the kernel | done |
| `libsxdisk` (GPT read and write) and `sxfs_mkfs` | done |
| `SXN_SYS_STORAGE_SNAPSHOT` and the read-only window | done |
| Raw sector access, rescan; Initialize, New Partition, Format, Delete | done |
| Mount and unmount | done |

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
- A right click opens the menu of what is under the pointer (Shift+F10 or the
  Menu key for the selection); Alt+Enter or a double click opens Properties
  (device, model, serial, partition style and GUIDs, type, mount point and
  usage). The view refreshes every two seconds.

## The operations

| Operation | On | What it does |
| --- | --- | --- |
| Initialize Disk | a disk with no table and nothing on it | writes an empty GPT (protective MBR, both headers and arrays) |
| New Partition | free space, or a GPT disk's box (its first free range) | adds a GPT entry, 1 MiB aligned; *Format as SxFS* (default) formats it and gives it the SxFS type, otherwise its type is Basic data |
| Format | a partition | `sxfs_mkfs`, the root owned by the person who asked (uid, group `users`), the SxFS type, and the label as the GPT name |
| Delete Partition | a partition | empties its GPT entry |
| Mount | an SxFS or FAT volume not mounted (not the ESP) | mounts it at `/mnt/<name>`, by default its label in lower case |
| Unmount | a mounted volume but `/` and `/boot` | syncs and releases it; refused while a file of it is open |
| Rescan Disks | every disk with nothing mounted | reads its table again |

Each one is the program run again as `diskmgmt --init <id> <name>`,
`--create <disk id> <name> <start> <sectors> [--format] [--label <text>]`,
`--format <id> <name> [--label <text>]`, `--delete <id> <name> <start>`,
`--mount <id> <name> <path>`, `--unmount <id> <name>` or `--rescan`, through
`sxn_elevate`; the prompt shows those arguments. The
elevated copy exits with 0 or the errno, and the window says what went wrong
(in use, changed since shown, access denied, no space). After writing a table
it asks the kernel to read it again (`SXN_SYS_STORAGE_RESCAN`), so the new
partitions appear at once with new ids.

Sector I/O goes through `libsxdisk`'s and `sxfs_core`'s callbacks, split into
the calls' 128-sector transfers. New GUIDs come from a xorshift generator
seeded from the clock and the pid: they have to be unique, not secret.

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
  ([SXFS_ROADMAP.md](../roadmaps/open/SXFS_ROADMAP.md)); a larger partition is formatted
  to its first 1 GiB, and the dialog says so.
- Every confirmation has *Cancel* as its default button.
- **A disk with a mounted partition is not repartitioned**, and a mounted
  partition is neither formatted nor deleted: unmount it first. The kernel
  refuses the writes anyway; the window does not offer them.
- **A mounted FAT volume is the person's who mounted it.** FAT has no owners;
  the kernel publishes everything on it as the real uid and gid of the caller,
  which the elevated copy still carries
  ([FAT_ROADMAP.md](../roadmaps/open/FAT_ROADMAP.md)). SxFS keeps the owners it records.
- **Mount points are `/mnt/<name>`**, one level, `[A-Za-z0-9._-]`, at most 24
  characters, not existing yet. Mounts do not survive a reboot.

## Tests

- `./build.sh test`: `partition-test` (the kernel reader, including a table
  libsxdisk wrote) and `sxdisk-test` (the library and `sxfs_mkfs`).
- `./build.sh smoke diskmgmt-smoke`: `diskmgmt --selftest` lists the devices
  and checks that ids are unique, partitions sit inside their disks, the root
  is mounted, flagged and adds up, and the map covers each disk without
  overlaps. The scenario attaches a blank 64 MiB disk with the serial
  `SXBLANK0` (`extra_disk_mib` in `tools/smoke_catalog.py`), and on it alone the
  self-test checks that the user's `--init` is refused (`EPERM`), then
  initializes, creates and formats a 32 MiB `SXTEST`, reformats it with a new
  label, mounts it at `/mnt/sxtest`, writes a file through `/bin/sh`, is
  refused unmounting while `cat` holds a file open, unmounts, mounts again and
  finds both files, refuses an overlapping partition and a wrong start, and
  deletes it. It also attaches a 16 MiB FAT disk, serial `SXFAT0`, that the
  smoke runner builds with `build/tools/fat-image` (`fat_disk_mib`): the
  self-test mounts it at `/mnt/fattest` as an elevated user 1000 would, checks
  that user writes it and user 1001 cannot, unmounts, mounts again and finds
  the file.
- `./build.sh smoke native-test`: the slice 28 assertions.
- `./build.sh smoke elevation-smoke`: `diskmgmt --rescan` through the real
  consent prompt.
- `./tools/shoot.sh --scenario diskmgmt`: the window, the root volume drawn as
  the system's, Properties, then Initialize, New Partition and Mount on a
  blank disk (`--extra-disk`, only for this scenario) through the consent
  prompt.
