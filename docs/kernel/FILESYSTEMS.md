# Filesystems: the VFS, the registry, and what a driver provides

Three pieces, each knowing only the one below it:

| Piece | Owns | Does not know |
|---|---|---|
| `vfs::` (`kernel/vfs.cpp`) | The tree of vnodes: the root volume's files, devices under `/dev`, the initramfs under `/rescue` | Any on-disk format |
| `fs::` (`kernel/fs.cpp`) | The driver registry and the mount table: which device is mounted where, by which driver | How a driver stores files |
| A filesystem driver (`kernel/sxfs.cpp`) | One on-disk format: probing it, publishing its tree, reading and writing it | Other formats, other volumes' drivers |

The only place that names a driver is the boot sequence in `kernel_main`,
which registers them: `sxfs::driver()` and `fat::driver()`
([FAT_ROADMAP.md](../roadmaps/open/FAT_ROADMAP.md)). Adding a format (ISO9660 for a CD)
means writing a driver and registering it. `vfs.cpp` does not change.

## The root is the system disk

The system runs from its disk. `fs::kRootMountPoint` is `/`: the first volume a
driver claims at boot is mounted there, and `/bin`, `/lib`, `/games` and the
user's files are that volume's.

| path | what it is | backed by |
| --- | --- | --- |
| `/` and everything below, except the rows under it | the system disk | SxFS (`build/disk.img`, or the LiveCD module) |
| `/boot` | the boot volume: what Limine loaded the kernel from; only the system writes there | FAT (the ESP, or the development boot drive) |
| `/dev` | devices | in memory, published by the drivers |
| `/rescue` | the initramfs: a rescue system | in memory, loaded by Limine |

**Why.** The initramfs came first, when there was no disk driver, and held
`/bin`. Once SxFS existed, every program was staged twice: in the initramfs and
in the disk's `bin`. The disk was already required — the shared libraries live
only in its `lib`, so no graphical program runs without it — and the copy in
memory cost 45 MB of RAM for the whole session, and lost the section cache's
sharing, which keys on a volume's files. The initramfs is now the rescue
system, 8.4 MB (below).

**`/dev` and `/rescue` are never the volume's.** `vfs::is_memory_path` names
them, and `fs::owner_of` returns no volume for a path below either, even with
one mounted at `/`. A volume carrying a top-level `dev` or `rescue` directory
would have it hidden; the system's image has neither.

**Without a disk, init comes from `/rescue`.** `kernel_main` starts `/bin/init`
when the root mounted and `/rescue/bin/init` when it did not. `PATH` is
`/bin:/rescue/bin`, and `sh` searches the same two, so the rescue shell finds
its commands either way.

## The rescue system

The initramfs holds what is left when no disk mounts — the disk is missing, its
controller is one there is no driver for, or the volume does not mount: a shell
on the text console and the tools to look at and repair a volume. Nothing
graphical, because the desktop needs `/lib`.

| | |
| --- | --- |
| Programs | `SAVANXP_RESCUE_PROGRAMS` in `CMakeLists.txt`: `init`, `sh`, the coreutils (`ls cat cp mv rm mkdir rmdir echo true false sleep ps truncate`), `df`, `fscheck`, `sync`, `sysinfo`, `uname`, and `imagetest` |
| Size | 8.4 MB, from 45 MB when it carried every program |
| Started by | the kernel, as `/rescue/bin/init`, when no volume mounted at `/` |
| What init does | sees that it was started from `/rescue` (its `argv[0]`), skips `audiod` and the session, and keeps `/rescue/bin/sh` alive on the console, starting it again when it exits |
| Checked by | `rescue-smoke`: QEMU with no disk at all (`run_smoke.py --no-disk`, scenario field `no_disk`), asserting `ls`, `cat`, `sh` resolving commands through `PATH`, `df` reporting no volume, and `imagetest` |

`imagetest` is not a tool. It tests the kernel's in-memory executable reader,
and `/rescue` is the only place that reader runs from, so it lives there; the
core `smoke` runs it from `/rescue/bin` too, next to the copy on the disk.

Every program, the rescue set included, is staged onto the disk as well. The
disk also carries `/system/programs.txt`, the build's list of the programs it
shipped: before, Add or Remove Programs told "came with the OS" by subtracting
the initramfs's `/bin`, which stopped working the moment the initramfs no
longer held every program (`appwiz_catalog.h`).

The smoke trigger (`/rescue/SMOKE`) is still staged into the initramfs, never
onto the disk, so a smoke build leaves the persistent image alone. A rescue
boot runs only the `rescue` scenario; any other one fails, since it needs the
disk.

**`/disk` is an alias of `/`.** `normalize_path` drops a first `disk`
component. Binaries built before the move carry `/disk/...` inside them — an
installed Doom's loader looks for `libsxmidi` in `/disk/lib/`, and its WADs in
`/disk/games/doom` — and they have to keep running (`AGENTS.md`, persistence of
`build/disk.img`). New code uses the plain paths. The alias can go when nothing
installed carries the old prefix; nothing checks that yet.

**Mount points are compared with `fs::path_below`**, not as a string prefix: a
mount point of `/` contains every absolute path, which is the one case a prefix
test followed by "`/` or end of string" gets wrong. `fs::join_mount_path` is its
inverse. Both live in `kernel/fs_path.cpp` so the host SxFS test links the same
code the kernel runs, and that test now mounts at `/`.

## How a request reaches a driver

A vnode with `Backend::volume` carries `volume_driver`, the `fs::Driver` that
published it. Operations on an open file (read, write, truncate, unlink,
refresh) go there.

Operations that start from a path (create a file, mkdir, rmdir, rename) go to
the volume that owns the path: `fs::owner_of(path)` returns the attached mount
whose mount point is the longest whole-component prefix of it. A path no volume
owns is in the in-memory tree, where the VFS handles it itself.

Rules the VFS enforces for every driver, so drivers do not repeat them:

- A mount point cannot be removed or renamed from inside (`rmdir /`,
  `rename / ...` are `EINVAL`). This used to be a literal `"/disk"` check.
  It now holds for any mount point.
- A rename stays on one volume, or within the in-memory tree. Crossing is
  `EINVAL`.
- Type, writability and range are checked before a driver is called, and
  `rmdir` checks the directory is empty first.
- Permissions are checked before a driver is called, on the vnode, with the
  caller's credentials: every operation that can change the tree takes them
  (`vfs::open`, `mkdir`, `unlink`, ...). The driver publishes uid, gid, mode
  and mtime on its vnodes, receives the owner of what it creates, and records
  `chmod`/`chown` through `set_ownership`. See
  [`USERS_AND_PERMISSIONS.md`](USERS_AND_PERMISSIONS.md#what-the-check-covers).

## What a driver provides

`fs::Driver` in `include/kernel/fs.hpp`:

- **Mounting:** `probe` (recognise the device, load metadata, publish nothing),
  `attach` (publish the tree), `unmount` (optional). `can_be_root` says the
  format may hold the system's root: `fs::mount` tries only such drivers for
  `/`, so a FAT boot drive registered ahead of the system disk, as on AHCI
  port 0, never becomes the root. `adopt_owner` is for a format with no owners
  of its own (FAT): `fs::mount` hands it the `fs::Owner` it was given, between
  probe and attach, and the driver publishes everything as that owner's.
- **Publishing:** directories through `vfs::ensure_directory`, files through
  `vfs::install_volume_file`, which stamps the vnode with the driver. `data`
  is the driver's own record (an SxFS `FileRecord`). The VFS never reads it.
- **File operations:** `read`, `write`, `truncate`, `unlink`,
  `remove_directory`, `refresh`. The driver keeps `vnode.size` current.
- **Path operations:** `create_file`, `create_directory`, `rename`. They
  return the published vnode, or fail when the volume is full or read-only.
- **Volume operations:** `stats` (writable, file count, total/used/free bytes)
  and `sync` (make metadata durable, mark clean).

`read` and `refresh` are required. A driver without them is refused at
registration.

## Who asks fs:: for what

- `sysinfo`: the `sxfs_*` fields of `savanxp_system_info` describe the system
  disk (`fs::kRootMountPoint`) through `fs::stats`, whatever its format. The
  names are ABI and stay.
- `SAVANXP_SYS_SYNC`: `fs::sync_all()` (every mounted volume), then
  `block::sync_all()` to drain the writeback queue.
- `SAVANXP_SYS_FSCHECK` stays SxFS-specific (`sxfs::check`): its report is
  the SxFS consistency model ([SXFS_CHECK.md](../fs/SXFS_CHECK.md)), not a
  generic one.
- The section cache keys file images on the vnode, not on an SxFS inode
  number, so two volumes cannot collide
  ([SECTIONS.md](SECTIONS.md#section_open-and-what-it-does-not-do-yet)).

Block-device diagnostics that used to be called from the VFS write path (ATA's
polling margins) are now `block::report_health`, called by the filesystem
driver and answered by whichever block driver has something to say.

## Mounting and unmounting at run time

Besides the root at boot, the system can mount a volume at `/mnt/<name>` and
release it (`SXN_SYS_STORAGE_MOUNT`, `_UNMOUNT`;
[DISK_MANAGEMENT.md](../userland/DISK_MANAGEMENT.md)). Mount slots are
reused: `fs::mount` takes the first free one.

`fs::release` is the unmount. It syncs the volume, then asks
`vfs::detach_subtree` to take the mount point and everything below it out of
the tree, and only then lets the driver free its state (`unmount`). The detach
first walks the subtree, unlinked nodes included, and refuses with `EBUSY` if
any node is still held: an open file or directory handle
(`process::detail::vnode_has_open_references`) or a live section mapped from a
file (`object::section_uses_vnode`), which is what a running program from the
volume has. Under the kernel lock nothing else can be holding a vnode between
two system calls, so when nothing is found nothing points at the subtree, and
its node slots go to a free list `add_node` takes from first. The root
(`/`) and the boot volume (`/boot`) cannot be released.

## The boot volume at `/boot`

Right after the root, `storage::mount_boot_volume` mounts the first FAT volume
holding `boot/kernel.elf` at `fs::kBootMountPoint`, trying EFI System
Partitions before anything else, and logs `storage: /boot is <device>`. On the
development path that is the QEMU boot drive's partition (`ahci0p1`); on an
installed system, the ESP; a LiveCD boot has none. Only the FAT driver is tried
there (`fs::mount` with a driver name), so no other volume is opened, let alone
written, to look for it. A root that carries its own `boot` keeps it and no
boot volume is mounted. Everything on it is the system's (FAT has no owners),
so only the system writes there, and it is never unmounted.

## Limits

What the VFS and the registry cannot do today. Most of it showed up writing the
second driver, FAT ([FAT_ROADMAP.md](../roadmaps/open/FAT_ROADMAP.md)): SxFS was shaped
with the VFS and never ran into it. Each entry says what breaks and, where it
is known, what the fix is. The SxFS side is in
[SXFS_ROADMAP.md](../roadmaps/open/SXFS_ROADMAP.md#what-a-second-driver-showed).

### Capacity

- `fs::kMaxDrivers` is 4 and `fs::kMaxMounts` is 4: root, `/boot`, and two for
  `/mnt` (the installer's target among them). SxFS holds 3 volumes at once
  (`kMaxVolumes`, about 3 MiB of kernel memory each).
- **Every volume's whole tree is published at attach, out of one pool.** There
  is no lookup on demand: a driver publishes every file and directory before
  the volume is usable, and the VFS holds 2048 nodes (`kMaxNodes`) for the
  initramfs, `/dev` and every mounted volume together. A volume whose tree
  does not fit does not mount (FAT fails attach cleanly and `fs::release`
  takes back what was published). A USB stick with a few thousand files is
  enough. The fix is publishing a directory's children when it is first
  looked up, which changes how every driver attaches.
- **Names are at most 63 bytes, paths at most 255** (`kMaxPathComponentLength`,
  `kMaxPathLength`). FAT allows 255-character names; the FAT driver leaves out
  what does not fit and logs it, never truncating a name.

### Semantics

- **Names are compared byte for byte** (`strcmp` in the child lookup). FAT is
  case-insensitive, the VFS is not: `/boot/LIMINE.CONF` is not found although
  FAT would find it, and creating `Readme` next to `README` passes the VFS's
  check and is then refused by the format, reported as `ENOSPC`. Read in the
  code, not yet exercised. A per-volume flag asking for case-insensitive
  lookup would be the smallest fix.
- **A driver's failure is a `bool`, and the VFS picks the errno per
  operation**: a failed create or write is `ENOSPC`, a failed truncate
  `ENOSPC` when growing and `EINVAL` when shrinking, and a failed mkdir,
  rename, unlink or rmdir `EINVAL`. A full volume, a read-only file, an I/O
  error and a name the format refuses all look alike to the program. The fix
  is driver operations that return a status.
- **There is no read-only mount.** Acting as the system passes the write bits
  ([USERS_AND_PERMISSIONS.md](USERS_AND_PERMISSIONS.md#what-the-check-covers)),
  so what protects a read-only volume is each vnode published without
  `writable` and the driver refusing. And the VFS calls the write operations
  without checking them for `nullptr`: a driver that cannot write still has to
  provide each one, returning failure. A mount flag the VFS checks before any
  driver call would cover both.
- **A driver never sees an open, a close or a vnode released.** It cannot keep
  state per open file, so FAT opens the file again on every `read`, walking
  its cluster chain from the start each time. And it is never told when the
  last handle on an unlinked file goes: both drivers free an unlinked file at
  once, so a handle still open on it reads nothing, where POSIX keeps it alive
  until the last close.
- **A rename reaches the driver first, then the tree.** If the VFS failed to
  rename its own node after the driver succeeded, the disk and the tree would
  disagree. Not seen; it is the order in `vfs::rename`. The VFS does refuse a
  directory moved into its own subtree before any driver is called, which
  FatFs on its own lets through.
- **Directories on a volume are plain in-memory vnodes** (`Backend::memory`).
  They find their driver by path, not by a stamp, which is enough while every
  directory operation starts from a path; but a driver holding a record per
  directory finds it by searching for the vnode, linearly (FAT's
  `directory_from_vnode`, SxFS's `file_from_vnode`).
