# Removable media: CD and DVD drives, ISO9660, automount, and the desktop

Status: **planned; no phase has started.** Nothing in the tree reads optical
media. The LiveCD reaches the kernel as a Limine module, not through the drive
([AHCI.md](../../kernel/AHCI.md)), and USB mass storage refuses anything that is
not a direct-access device. A USB stick is read and written today, but only
once someone mounts it from Disk Management, elevated.

This page is the plan for reading a data CD or DVD after boot, mounting a disc
or a stick when it is inserted, and showing it on the desktop as an icon that
opens it and ejects it.

## The short version

- **The first drive is a USB one**, the development host's LG (HL-DT-ST)
  DVDRAM GP10NB20 behind a MediaTek MT1836 bridge (`0e8d:1836`). The Ryzen's
  log already shows it found and refused:
  `usb-storage: device type 5 not supported`
  ([RYZEN5_1600_VEGA56.md](../../hardware/RYZEN5_1600_VEGA56.md)).
- **The transport already exists.** `kernel/usb/umass.cpp` speaks Bulk-Only
  and SCSI; a CD/DVD drive is the same transport with MMC commands and
  removable media. ATAPI over AHCI or the legacy IDE channels is a second
  transport for the same layers above it, and comes later.
- **Read-only, data discs only.** ISO9660 with Joliet first; Rock Ridge and
  UDF after. Audio CDs and writing discs are out of scope here.
- **Mounting removable media is a service's policy, not the kernel's.** The
  kernel reports that a medium arrived or left; a SUN service mounts it under
  `/mnt` and releases it. A disc is mounted read-only. **A USB stick is
  mounted writable, as the session's user**, so the user reads and writes it
  with no prompt.
- **The desktop icon is not a file.** It is drawn by the desktop for each
  mounted removable volume, disc or stick, next to the Desktop folder's
  contents, and goes away with the volume.
- **Everything but the last phase runs in QEMU**, which emulates a USB CD-ROM
  (`usb-storage` with `media=cdrom`) and lets a smoke insert and eject a disc
  through QMP (`blockdev-change-medium`, `eject`).

## What already exists

| Piece | Where | What the disc needs from it |
|---|---|---|
| Bulk-Only Transport, SCSI `INQUIRY`, `READ CAPACITY`, `READ(10)`, `REQUEST SENSE`, `TEST UNIT READY` | `kernel/usb/umass.cpp` | Accept peripheral type 5; read 2048-byte blocks |
| USB hot plug, and a pulled stick released through `storage::forget_disk()` | [USB_ROADMAP.md](../done/USB_ROADMAP.md) | The same release when a disc leaves, with the drive still attached |
| The filesystem registry and read-only mounts | [FILESYSTEMS.md](../../kernel/FILESYSTEMS.md) | A new driver, registered in `kernel_main`; `fs::attach(mount, true)` |
| Run-time mount at `/mnt/<name>`, the storage snapshot and its generation | [DISK_MANAGEMENT.md](../../userland/DISK_MANAGEMENT.md) | The automount service and the desktop read the same snapshot |
| Desktop icons from the Desktop folder, re-read every 2 s | [DESKTOP_ICONS.md](../../desktop/DESKTOP_ICONS.md) | A second source of icons: mounted removable volumes |

What it does not have, and this plan adds: a block device with sectors other
than 512 bytes (`block::kSectorSize` is fixed), a medium that comes and goes
while its device stays, an ISO9660 driver, automount, a mount owned by someone
other than its caller, enough mount slots (`fs::kMaxMounts` is 4: `/`, `/boot`
and two more), and eject.

## Decisions

### 2048-byte sectors are translated in the drive's driver

`block::kSectorSize` is 512 and every consumer assumes it. The optical driver
presents the disc as 512-byte sectors: a request for logical sector `n` reads
device block `n / 4` and copies its quarter, with a small cache of the last
blocks read so that a sequential read does not fetch each block four times.
The device is read-only, so there is no read-modify-write. Teaching the block
layer about other sector sizes is a larger change that benefits 4Kn disks too;
it is left to [DRIVERS_ROADMAP.md](DRIVERS_ROADMAP.md) and does not block this.

### A medium is a block device; the drive is not

The drive stays attached while discs go in and out. Each disc that becomes
ready registers a block device; a disc that is removed, ejected, or reported
changed (`UNIT ATTENTION`, ASC `28h`) goes through the same release path as a
pulled stick. A new disc never revives the old device's handles, as the
reconnect rule in [DRIVERS_ROADMAP.md](DRIVERS_ROADMAP.md) requires. The
storage generation moves on each arrival and departure, so nothing new is
needed to notice it.

The driver finds out by polling `TEST UNIT READY` about once a second while
no disc is present or a disc is mounted, and reads the sense:

| Sense | Meaning | Action |
|---|---|---|
| `02/3A` | No medium | Nothing, or release the old one |
| `02/04/01` | Becoming ready (spinning up) | Wait |
| `06/28` | Medium may have changed | Release the old one, read the new one |
| Good status, no medium registered | A disc is ready | `READ CAPACITY`, register |

### ISO9660 is written from ECMA-119, not ported

The format is small and read-only: volume descriptors, a path table that can
be ignored, and directory records that hold extents. OpenBSD's `cd9660` is
written against its vnode layer and buffer cache, so porting it costs more
than the driver. The driver is written against `fs::Driver` from ECMA-119 and
the Joliet specification, and is checked by a host test against images made
by `xorriso`, which is already a build dependency.

Joliet comes with the first version, because most discs made on Windows carry
their long names only there. Rock Ridge (POSIX names, modes, symlinks) and
UDF (most DVDs written after 2000) are later phases.

### The automount policy is a SUN service

Mounting is a privileged call (`SXN_SYS_STORAGE_MOUNT`), and the policy of
what to mount, where and how is not the kernel's. A small service (working
name `mediad`), started by `init` as the system and written against SUN, reads
the storage snapshot when its generation moves and:

- mounts a newly arrived removable volume at `/mnt/<label>`, the volume label
  lowercased and reduced to `[a-z0-9._-]`, or `/mnt/cdrom` and `/mnt/usb`
  when there is none, with `-2`, `-3` on a collision;
- releases a volume whose medium has gone;
- carries out an eject request: unmount, then eject.

**What counts as removable:** an optical medium, and each SxFS or FAT volume
on a USB mass storage device. Internal disks (AHCI, ATA, NVMe), the ESP, and
any disk holding `/` or `/boot` are never automounted; Disk Management stays
the way to mount those. A stick with several partitions gets one mount and one
icon per volume.

There is no change notification yet; `mediad` polls the snapshot's generation.
When [DRIVERS_ROADMAP.md](DRIVERS_ROADMAP.md) D5 adds generation-based
notifications, it waits on them instead.

### A stick belongs to the session's user

A disc is read-only and its files are published readable by everyone. A stick
is mounted **writable, owned by the user of the current session**, the one
`sessiond` started the desktop as, so the person at the screen reads, writes
and deletes on it without elevating:

- **FAT** has no owners, and the kernel today publishes its files as the real
  uid and gid of the caller of the mount
  ([DISK_MANAGEMENT.md](../../userland/DISK_MANAGEMENT.md)). `mediad` runs as the
  system, so the mount call gains an owner argument that only a system caller
  may set; `mediad` passes the session's user. Disk Management keeps the
  current rule.
- **SxFS** keeps the owners it records, as on any mount. A stick written on
  another system may carry uids that are not the user's; the mount does not
  rewrite them.
- **No session, no stick.** Before `sessiond` starts the desktop, sticks wait;
  they are mounted when the session starts and released (synced, then
  unmounted) when it ends, so the next session's user owns them afresh. Discs
  do not depend on the session.
- A stick is already written through (`deferred_writes = false`,
  [USB_ROADMAP.md](../done/USB_ROADMAP.md)), so pulling it without ejecting
  loses at most the write in flight. Eject is still the safe way, and what the
  icon offers.

The system is single-user today. When there is more than one user, the owner
is whoever holds the display session, the same rule as the screen
([USERS_AND_PERMISSIONS.md](../../kernel/USERS_AND_PERMISSIONS.md)).

### Mount slots grow

`fs::kMaxMounts` is 4, and FatFs's `FF_VOLUMES` follows it. With `/` and
`/boot` taken, a disc and a stick fill the rest, and Disk Management can no
longer mount anything. O3 raises the limit (16 is the working figure) and
gives the matching FatFs volumes, and a full table makes `mediad` log and skip
the volume rather than fail.

### Eject is a storage operation, in both subsystems

A new storage call ejects a removable device, refused while any volume of it
is mounted. For an optical drive it opens the tray (`START STOP UNIT` with
`LoEj`). For a stick it is a safe removal: the volumes are already released,
the device is stopped, and the desktop says it can be pulled out. Under feature parity it is reachable
from POSIX as well as SUN; which POSIX shape it takes (an ioctl on the device
node, as `CDIOCEJECT` on the BSDs, is the candidate) is decided in the phase.

The session's user may ask `mediad` to eject anything it automounted.
Unmounting something that Disk Management mounted still needs elevation.

### The disc on the desktop

The desktop shows two sources of icons: the Desktop folder's contents, and one
icon for each mounted removable volume. A volume icon:

- is not a file, so it cannot go stale in the folder after a crash, and Files
  does not show it inside `Desktop`;
- shows the volume's label (or "CD Drive", "Removable Disk") and a disc or a
  removable-disk icon from `/icons`, drawn to
  [ICON_STYLE.md](../../desktop/ICON_STYLE.md);
- opens Files at the mount point on a double click;
- has a context menu with Open, Eject (Safely Remove for a stick) and
  Properties;
- keeps its cell in `desktopicons.ini` under a key that cannot be a file name
  (for example `:volume:<label>`), so the same disc returns to the same place;
- appears and disappears when the desktop's 2-second refresh sees the
  storage generation move.

Delete, Rename and Cut do not apply to a volume icon and are not offered.

## Phases

### O1 — the drive and the medium

`umass` accepts peripheral type 5 as a read-only removable device:
2048-byte blocks translated to 512, the `TEST UNIT READY` poll, media arrival
and departure as above. Boot log lines name the drive and each disc
(`usb-storage: optical drive "<model>"`, `... medium ready, <n> blocks`,
`... medium removed`). Disk Management lists the disc as a read-only device
and offers no partition or format actions on it.

**Done when** a smoke boots QEMU with a USB CD-ROM holding an image, reads
known sectors through the storage calls, ejects and inserts another image
through QMP, and sees the device go and come back with a new id.

### O2 — ISO9660 and Joliet

A read-only `fs::Driver`: primary and supplementary volume descriptors,
directories, files spread over one extent or several, the volume label for
`stats`. Registered in `kernel_main`; mountable from Disk Management with
Read-only forced. Never eligible as the root (`can_be_root` false).

**Done when** the host test reads names, sizes and contents from `xorriso`
images (plain ISO9660, Joliet, deep trees, a file over 4 GiB in several
extents), and a smoke mounts the QEMU disc and runs `ls` and `cat` on it.

### O3 — automount and eject

`mediad`; the mount owner argument; more mount slots; the eject storage call in
SUN and POSIX, each with its assertion (`nativetest` for SUN). A disc mounts
read-only on insertion, a stick mounts writable as the session's user, and
both are released on removal or eject; ejecting while a file on it is open is
refused with a reason.

**Done when** the smoke inserts a disc and plugs in a FAT stick through QMP,
finds both mounted under `/mnt`, writes a file on the stick as uid 1000 with
no elevation, ejects both from each subsystem's calls, and finds nothing left
mounted; an internal disk in the same machine is not mounted.

### O4 — the desktop icon

The volume icons, their menu and the disc icon. Files shows the mount point
like any folder.

**Done when** a `tools/shoot.sh` scenario inserts a disc and a stick, captures
both icons, opens each into Files, ejects them from the menu, and captures the
desktop without them; `windowd-smoke` asserts that the icons follow the
volumes.

### O5 — the real drive

The GP10NB20 on the Ryzen: the MT1836 bridge's quirks, slow spin-up, a
pressed eject button (the drive reports `06/28` or `02/3A`), and discs made
on other systems; a FAT32 stick formatted elsewhere, plugged in and safely
removed. Results are recorded in
[RYZEN5_1600_VEGA56.md](../../hardware/RYZEN5_1600_VEGA56.md).

### Later

| Phase | What | Needs |
|---|---|---|
| O6 | Rock Ridge | O2 |
| O7 | UDF, read-only | O2's mount and test pattern |
| O8 | ATAPI on AHCI and legacy IDE, for internal drives | O1's medium model; `AHCI.md` |

## Open questions

- Whether a disc inserted before boot is mounted by `mediad` at startup like
  any other arrival (the expected answer), or left to the user.
- Autorun: deliberately not planned. Nothing on a disc runs because it was
  inserted.

## Out of scope

Audio CDs (`READ TOC`, `READ CD`, playback through `audiod`), writing or
erasing discs, DVD-Video copy protection (CSS), Blu-ray, and booting from the
drive, which is the firmware's and Limine's job.
