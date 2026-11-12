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

This page describes the current implementation. The ordered work toward common
file contracts, object lifetime, on-demand lookup, mount policy and mapping
coherence is in [`VFS_ROADMAP.md`](../roadmaps/open/VFS_ROADMAP.md), including
acceptance gates shared by SxFS, FAT and future drivers.

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
  `EXDEV`.
- A write operation the driver does not provide is `EROFS`; the VFS checks
  before calling, so a read-only driver leaves those pointers null.
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

- **Mounting:** `probe` (recognise the device, load metadata, publish nothing
  and **write nothing**), `attach` (publish the tree; with `read_only`, still
  write nothing), `unmount` (optional). `can_be_root` says the
  format may hold the system's root: `fs::mount` tries only such drivers for
  `/`, so a FAT boot drive registered ahead of the system disk, as on AHCI
  port 0, never becomes the root. `adopt_owner` is for a format with no owners
  of its own (FAT): `fs::mount` hands it the `fs::Owner` it was given, between
  probe and attach, and the driver publishes everything as that owner's.
- **Publishing:** directories through `vfs::ensure_directory`, files through
  `vfs::install_volume_file`, which stamps the vnode with the driver. `data`
  is the driver's own record (an SxFS `FileRecord`). The VFS never reads it.
- **File operations:** `read`, `write`, `truncate`, `unlink`,
  `remove_directory`, `refresh`, `release`. The driver keeps `vnode.size`
  current.
- **Path operations:** `create_file`, `create_directory`, `rename`. They
  publish the vnode. `replaces_on_rename` says a rename onto an existing name
  replaces it atomically; without it the VFS answers `EEXIST` itself.
- **Volume operations:** `stats` (writable, file count, total/used/free bytes),
  `sync` (make metadata durable, mark clean) and `sync_deferred` (the same,
  queued behind the writeback thread).

Every operation that can fail returns an `fs::Status`: `ok`, `io_error`,
`no_space`, `read_only`, `invalid`, `exists`, `not_found`, `not_empty`,
`not_directory`, `is_directory`, `unsupported`.
`fs::error_of` is the one place a status becomes an errno, so a full volume,
a dead device and a refused name reach the program as different errors.

`read` and `refresh` are required. A driver without them is refused at
registration.

## The file contract

What a program can rely on, whatever the volume. The host test
`tests/host/sxfs_vfs_test.cpp` (`sxfs-vfs-test`) runs the real VFS over the
real SxFS driver against it, and `smoke` repeats the core of it in QEMU.

**Errors are the operation's own.** `ENOENT`, `EEXIST`, `EISDIR`, `ENOTDIR`,
`ENOTEMPTY`, `EXDEV` (a rename across volumes), `EROFS`, `ENOSPC`, `EIO`,
`EBUSY`. A read that fails returns the error, never a short count that looks
like end of file: `vfs::last_error()` is 0 after a read that simply ended.

**A rename onto an existing name replaces it**, in one step: every lookup
finds the old object or the new one. A file replaces a file, a directory an
empty directory; the mixes are `EISDIR` and `ENOTDIR`, a directory with
entries `ENOTEMPTY`. On a driver without `replaces_on_rename` (FAT) the
existing name is `EEXIST` and nothing changes.

**An unlinked file lives while something holds it.** `unlink`, or a rename
over it, removes the name at once: it is in no lookup and no listing, and the
name can be created again. Handles and sections that had the file keep
reading and writing its contents (`Vnode::unlinked`), and the driver's
`release` frees it when the last one goes. Holders are counted on the node
(`Vnode::references`): whoever keeps a vnode past the call that found it
calls `vfs::retain`, and `vfs::release` when done. An open handle does it in
`sx::open_file` and when its I/O object is destroyed; a file-backed section
when it is created and destroyed. The last release of an unlinked node frees
the file. A driver without `release` (FAT) refuses to unlink or replace a held
file with `EBUSY`. A directory that is some process's open handle or POSIX
current directory (which holds its vnode too) cannot be removed (`EBUSY`) and
keeps its volume mounted, but can be renamed: its holders keep the vnode, and
`getcwd` rebuilds the path from the tree (`vfs::path_of`).

**Identity.** A vnode's address is stable while anything holds it. A slot is
reused once the node is gone and released, and `content_generation` never
repeats across reuse: it moves on every write, truncate and reuse, so a cache
of a file's bytes keyed on (vnode, generation) cannot serve another file or an
older version ([SECTIONS.md](SECTIONS.md)).

**Durability.** A successful `write`, `close` or `rename` is in the cache, not
on the medium. `fsync(fd)` on a file or a directory, and `sync()`, return once
everything written before the call is on the medium, or fail with `EIO`. Both
are the same barrier: a volume's metadata and the writeback queue are shared
by all its files, so there is no cheaper per-file one to offer. To replace a
file without ever losing it:

1. write the new contents to a temporary name in the same directory;
2. `fsync` it;
3. `rename` it over the old name;
4. `fsync` the directory.

On SxFS a power cut at any sector of this leaves the name holding the whole
old file or the whole new one; before step 3 is asked for it is always the
old one, and after step 4 returns it is always the new one. The temporary may
survive a cut before step 3 and is the program's to remove. The test cuts at
every sector. On FAT step 3 fails with `EEXIST`: there the old file has to be
removed first, and a cut between the two leaves only the temporary.

## Who asks fs:: for what

- `sysinfo`: the `sxfs_*` fields of `savanxp_system_info` describe the system
  disk (`fs::kRootMountPoint`) through `fs::stats`, whatever its format. The
  names are ABI and stay.
- `SAVANXP_SYS_SYNC` and `SAVANXP_SYS_FSYNC`: `fs::sync_all_deferred()` (every
  mounted volume), then a `block::writeback_checkpoint` ticket the caller
  sleeps on until the queue has drained past it.
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
any node is still held (`vfs::is_held`): an open file or directory handle, or
a live section read from a file, which is what a running program from the
volume has. Under the kernel lock nothing else can be holding a vnode between
two system calls, so when nothing is found nothing points at the subtree, and
its node slots go to a free list `add_node` takes from first. The root
(`/`) and the boot volume (`/boot`) cannot be released.

## Read-only mounts

`fs::attach(mount, /*read_only=*/true)` mounts a volume that is never written:
the driver is told (`Driver::attach`'s flag) and publishes its vnodes without
`writable`, and the VFS refuses every change on the volume with `EROFS` before
any driver is called: create, open for writing, mkdir, unlink, rmdir, rename,
truncate, chmod and chown. `fs::read_only(mount)` is the question; it is also
true for a volume its driver could only bring up read-only (a device that
refuses writes, a recovery that could not be persisted). `sync` on such a
volume succeeds and writes nothing.

From userland it is `sxn_storage_mount_flags(id, path,
SXN_STORAGE_MOUNT_READ_ONLY)`; the device is then listed with
`SXN_STORAGE_READ_ONLY`. On SxFS it is the way to look at a volume that needs
recovery without changing it ([SXFS_CHECK.md](../fs/SXFS_CHECK.md#looking-without-touching)).
The root is always mounted writable.

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
- **The published tree comes out of one pool,** 2048 nodes (`kMaxNodes`) for
  the initramfs, `/dev` and every mounted volume together. A driver may
  publish a directory on demand (`vfs::install_volume_directory` and
  `fs::Driver::populate`): its entries enter the tree the first time it is
  looked up or listed, and a failed populate is taken back so the next look
  tries again. FAT and SxFS both do: a mount publishes its mount point only.
  When free slots drop below 256,
  the entry of the next system call gives back the on-demand directories
  looked into longest ago (`vfs::trim_if_low`): their entries leave the tree,
  the driver `forget`s each one, and the next look publishes them again.
  Nothing under a held node goes. **A `Vnode*` kept from one system call to
  the next must be retained** (`vfs::retain`); an unretained one may point at
  a reused slot
  ([VFS_ROADMAP.md](../roadmaps/open/VFS_ROADMAP.md#vfs-c--component-resolution-and-bounded-directory-caches)).
- **Names are at most 255 bytes, paths at most 1023 and 64 components**
  (`kMaxPathComponentLength`, `kMaxPathLength`; `NAME_MAX` and `PATH_MAX` in
  the SDK). A node keeps a name of up to 47 bytes in its slot and a longer
  one on the heap, at its size; a rename takes the new name's room before the
  driver changes the disk. FAT's long names fit; the FAT driver leaves out
  what does not and logs it, never truncating a name. SxFS keeps its own
  format limits below its mount point: 63-byte names, 255-byte paths. A path is too big for the 32 KiB kernel stack
  more than once or twice: the VFS and the system-call dispatchers keep theirs
  in `vfs::PathBuffer`, on the heap, and a failed allocation is `ENOMEM`.

### Semantics

- **Names compare by the format's rules.** The child lookup asks the
  directory's driver (`fs::Driver::same_name`) and falls back to byte for
  byte. FAT folds case with FatFs's own table (`ff_wtoupper`), so
  `/boot/LIMINE.CONF` finds `limine.conf`, creating `Readme` next to `README`
  opens `README`, and renaming `a.txt` to `A.TXT` changes the case on the
  disk. SxFS, `/dev`, `/rescue` and everything above a mount point stay
  case-sensitive. FAT's short 8.3 aliases (`PROGRA~1`) are not names the VFS
  knows. A driver that keeps paths is handed the existing directories as the
  tree spells them and the new leaf as the caller did, never the caller's
  spelling of the directories above it.
- **A handle opened before a volume went read-only keeps its vnode.** The
  policy is checked at the operations that start from a path; a write through
  an already open handle reaches the driver, which refuses it. Nothing remounts
  a volume read-only today, so it cannot happen yet.
- **A driver sees the first holder and the last, not each opening.**
  `fs::Driver::open` and `close` run when a vnode's reference count leaves
  and returns to zero, so state is per file, not per handle: FAT keeps one
  FatFs file open for a held file ([FAT_ROADMAP.md](../roadmaps/open/FAT_ROADMAP.md#how-fatfs-maps-onto-fsdriver)).
  A driver that needs state per opening (an enumeration cursor, a lock)
  needs the open descriptions VFS-B still plans.
- **`fsync` is the whole-system barrier.** Correct, and as slow as `sync` on a
  machine with several busy volumes.
- **A deferred write error reaches whoever synchronizes next**, as `EIO` from
  `sync`/`fsync`, not the handle that wrote the bytes.
- **File-backed sections are private copies.** A section is the file as it was
  at one generation; a later write makes the next `section_open` read again
  but does not change views already mapped. There is no shared writable
  mapping.
- **A rename reaches the driver first, then the tree.** If the VFS failed to
  rename its own node after the driver succeeded, the disk and the tree would
  disagree. Not seen; it is the order in `vfs::rename`. The VFS does refuse a
  directory moved into its own subtree before any driver is called, which
  FatFs on its own lets through.
- **Directories on a volume are plain in-memory vnodes** (`Backend::memory`).
  They find their driver by path, not by a stamp, which is enough while every
  directory operation starts from a path; but a driver holding a record per
  directory finds it by searching for the vnode, linearly (FAT's
  `directory_from_vnode`, SxFS's `file_from_vnode`). A file's vnode carries
  its record in `data`, which both drivers read back without searching.
