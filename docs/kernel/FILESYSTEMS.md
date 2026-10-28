# Filesystems: the VFS, the registry, and what a driver provides

Three pieces, each knowing only the one below it:

| Piece | Owns | Does not know |
|---|---|---|
| `vfs::` (`kernel/vfs.cpp`) | The tree of vnodes: the initramfs, devices under `/dev`, and every mounted volume's files | Any on-disk format |
| `fs::` (`kernel/fs.cpp`) | The driver registry and the mount table: which device is mounted where, by which driver | How a driver stores files |
| A filesystem driver (`kernel/sxfs.cpp`) | One on-disk format: probing it, publishing its tree, reading and writing it | Other formats, other volumes' drivers |

The only place that names a driver is the boot sequence in `kernel_main`,
which registers it (`fs::register_driver(sxfs::driver())`). Adding a format
(FAT for a USB stick, ISO9660 for a CD) means writing a driver and registering
it. `vfs.cpp` does not change.

## How a request reaches a driver

A vnode with `Backend::volume` carries `volume_driver`, the `fs::Driver` that
published it. Operations on an open file (read, write, truncate, unlink,
refresh) go there.

Operations that start from a path (create a file, mkdir, rmdir, rename) go to
the volume that owns the path: `fs::owner_of(path)` returns the attached mount
whose mount point is the longest whole-component prefix of it. A path no volume
owns is in the in-memory tree, where the VFS handles it itself.

Rules the VFS enforces for every driver, so drivers do not repeat them:

- A mount point cannot be removed or renamed from inside (`rmdir /disk`,
  `rename /disk ...` are `EINVAL`). This used to be a literal `"/disk"` check.
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
