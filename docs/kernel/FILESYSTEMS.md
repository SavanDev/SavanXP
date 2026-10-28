# Filesystems: the VFS, the registry, and what a driver provides

Three pieces, each knowing only the one below it:

| Piece | Owns | Does not know |
|---|---|---|
| `vfs::` (`kernel/vfs.cpp`) | The tree of vnodes: the root volume's files, devices under `/dev`, the initramfs under `/rescue` | Any on-disk format |
| `fs::` (`kernel/fs.cpp`) | The driver registry and the mount table: which device is mounted where, by which driver | How a driver stores files |
| A filesystem driver (`kernel/sxfs.cpp`) | One on-disk format: probing it, publishing its tree, reading and writing it | Other formats, other volumes' drivers |

The only place that names a driver is the boot sequence in `kernel_main`,
which registers it (`fs::register_driver(sxfs::driver())`). Adding a format
(FAT for a USB stick, ISO9660 for a CD) means writing a driver and registering
it. `vfs.cpp` does not change.

## The root is the system disk

The system runs from its disk. `fs::kRootMountPoint` is `/`: the first volume a
driver claims at boot is mounted there, and `/bin`, `/lib`, `/games` and the
user's files are that volume's.

| path | what it is | backed by |
| --- | --- | --- |
| `/` and everything below, except the two rows under it | the system disk | SxFS (`build/disk.img`, or the LiveCD module) |
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

## What a driver provides

`fs::Driver` in `include/kernel/fs.hpp`:

- **Mounting:** `probe` (recognise the device, load metadata, publish nothing),
  `attach` (publish the tree), `unmount` (optional).
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

## Limits

- No `unmount` for SxFS: the VFS cannot detach a subtree yet, so a released
  volume would leave vnodes pointing at freed records.
- `fs::kMaxDrivers` is 4 and `fs::kMaxMounts` is 4.
- Directories on a volume are plain in-memory vnodes (`Backend::memory`). They
  find their driver by path, not by a stamp. That is enough while every
  directory operation starts from a path.
