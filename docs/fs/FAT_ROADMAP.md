# FAT — a second filesystem driver, on FatFs

Status: **phase 1 done**: a read-only FAT driver (`kernel/fat.cpp`), and the
boot volume mounted at `/boot`. Writes are phase 2.

Before this driver SavanXP read only SxFS, and the one FAT volume that matters,
the EFI System Partition, could only be written as a whole image
([SETUP_AND_UPDATES.md](../userland/SETUP_AND_UPDATES.md#what-the-target-disk-looks-like)).
This page is the driver's design: which code, how it plugs into `fs::`, what
had to change outside it, and the phases.

## The short version

- **Adopt FatFs** (ChaN) unmodified under `vendor/fatfs/`, as uACPI is, with
  glue of our own in `kernel/fat.cpp` that implements `fs::Driver`.
- **SxFS stays the only root.** Before any FAT driver is registered,
  `fs::mount_any` has to stop accepting a FAT volume as `/`.
- **The ESP is mounted at `/boot`**, owned by the system, read-only first,
  writable in a later phase. Nothing runs from it, and it is not unmounted.
- **Disk Management mounts FAT volumes at `/mnt/<name>`** once the driver
  exists: the storage calls already go through `fs::mount`.
- **FAT12, FAT16 and FAT32 with long names. No exFAT.**
- Effort: one to two weeks, most of it in the glue and the tests.

## Decision: FatFs, not fastfat, not our own

| Option | Why not / why |
|---|---|
| Microsoft's `fastfat` sample | 30–40k lines written against the NT executive: IRPs, the Cache Manager, `FsRtl` oplocks and notifications, `ERESOURCE`. Porting it means writing an NT compatibility layer larger than the driver. MS-PL on top. `Do not adopt`. |
| A driver of our own | 1500–2500 lines, two to four weeks, and the hard part (write ordering between the FAT and the directories, long-name edge cases) is exactly what FatFs has already been tested on for years. Kept as the fallback if FatFs does not fit. |
| **FatFs** | ~7k lines of portable C, no dependencies, a five-function disk layer, a one-clause BSD-style license compatible with MIT. `Adopt`. |

FatFs is published as a zip on elm-chan.org, not as a git repository. The pin
is the release name (R0.16) plus the zip's SHA-256, in
`vendor/fatfs/FATFS_RELEASE.txt`, next to its license file. Only the module
sources are vendored; `vendor/fatfs/README.md` lists what is left out and why. The adoption is
recorded in [`THIRD_PARTY_PROVENANCE.md`](../third-party/THIRD_PARTY_PROVENANCE.md).

## Prerequisite: the root is never FAT

`fs::mount_any(kRootMountPoint)` walks the block devices in order and keeps the
first one **any** registered driver claims. With a FAT driver registered:

- the `--no-disk` rescue boot would mount the FAT boot drive (AHCI port 0) as
  `/` instead of falling back to `/rescue`;
- an AHCI or IDE run, where the FAT boot drive is port 0 and `disk.img` is
  port 1, would mount the boot drive as `/`. The default virtio run survives
  only because virtio-blk registers before AHCI.

The boot drive does not even need to be claimed as its partition: block
devices are registered as `ahci0 ahci1 ahci0p1`, so the whole disk `ahci0`
comes first, and FatFs, when sector 0 is not a FAT boot sector, looks for a FAT
partition in the MBR and mounts that. See the next section.

So `fs::Driver` gains a `can_be_root` field (SxFS: true, FAT: false), and
`mount_any` skips drivers without it. This is a behavior change of its own,
lands first, and is checked by `rescue-smoke` and the AHCI smoke before the FAT
driver exists. The comment in `kernel_main.cpp` around `fs::attach` is still in
Spanish and is translated in the same commit.

## A whole disk is never mounted as FAT

With `FF_MULTI_PARTITION` 0, FatFs's `f_mount` falls back to the first FAT
partition in an MBR when sector 0 is not a FAT boot sector. On a partitioned
disk that would mount the same volume twice, once through the disk and once
through `<disk>p1`, with two independent copies of its FAT. So `probe` reads
sector 0 itself and claims the device only when it is a FAT boot sector, the
check `storage::detect` already makes, before calling `f_mount`.
`storage::mount` already refuses a disk whose sector 0 is a partition table,
but the boot path calls `fs::mount` directly and needs the driver's own check.

## How FatFs maps onto `fs::Driver`

### Configuration (`ffconf.h`, ours)

| Option | Value | Why |
|---|---|---|
| `FF_VOLUMES` | `fs::kMaxMounts` (4) | One FatFs volume per possible mount |
| `FF_MULTI_PARTITION` | 0 | `partition::` already slices disks into block devices with their own LBA 0 |
| `FF_MIN_SS` / `FF_MAX_SS` | 512 | `block::kSectorSize` |
| `FF_USE_LFN` | 3 | Long names, buffer on the kernel heap (`ff_memalloc`/`ff_memfree`) |
| `FF_LFN_UNICODE` | 2 | UTF-8 in and out, as every path in the system is |
| `FF_CODE_PAGE` | 437 | Only for short names; avoids carrying every code page table |
| `FF_FS_EXFAT` | 0 | No exFAT |
| `FF_USE_FASTSEEK` | 1 | Lets a later per-vnode open file keep a cluster link map; unused while every `read` reopens the file |
| `FF_USE_MKFS` | 1 | The host test formats its images with it; small |
| `FF_FS_NORTC` | 0 | `get_fattime` from `rtc::epoch_seconds` |
| `FF_FS_REENTRANT` | 0 | Every driver call runs under the big kernel lock ([SMP_ROADMAP.md](../kernel/SMP_ROADMAP.md)). Splitting the filesystem off the BKL means turning this on, with `ff_mutex_*` over a kernel lock |
| `FF_LBA64` | 0 | `block::` addresses sectors with 32 bits |

FatFs is C. It builds as an object library with the kernel's freestanding
flags (`savanxp_fatfs_objects`), the way `savanxp_uacpi_objects` does. The
configuration lives in `kernel/fatfs/`, outside the vendored tree, and the
include path finds it because `vendor/fatfs/source` has no `ffconf.h` of its
own. Next to it, `kernel/fatfs/string.h` stands in for the `<string.h>` a
freestanding compiler does not ship: the kernel's `mem*` (C linkage) and an
inline `strchr`, which `ff.c` uses to reject characters FAT does not allow.

What the compiled module asks of the driver, measured on the phase 0 build:
`disk_initialize`, `disk_status`, `disk_read`, `disk_write`, `disk_ioctl`,
`ff_memalloc`, `ff_memfree` and `get_fattime`. It is about 31 KB of code.

### The disk layer (`diskio`)

| FatFs | SavanXP |
|---|---|
| `disk_initialize`, `disk_status` | `block::device_info`; `STA_PROTECT` when the device is not writable |
| `disk_read` | `block::read(device_index, lba, count, buffer)` |
| `disk_write` | `block::write` (synchronous; the ESP is not where write-back pays) |
| `disk_ioctl(CTRL_SYNC)` | `block::sync(device_index)` |
| `disk_ioctl(GET_SECTOR_COUNT / GET_SECTOR_SIZE)` | `DeviceInfo::sector_count`, 512 |

FatFs's physical drive number is our FatFs volume slot; the glue keeps the
slot → `device_index` table.

### The driver operations

| `fs::Driver` | FatFs |
|---|---|
| `probe` | `f_mount(..., 1)`. `FR_NO_FILESYSTEM` → `kInvalidVolume` ("not my format") |
| `attach` | Walk the tree with `f_opendir`/`f_readdir`; `vfs::ensure_directory` for directories, `vfs::install_volume_file` for files |
| `read` | `f_open` + `f_lseek` + `f_read` + `f_close` |
| `write`, `truncate` | `f_open(FA_WRITE)` + `f_lseek` + `f_write` / `f_truncate` + `f_close` |
| `create_file`, `create_directory` | `f_open(FA_CREATE_NEW)`, `f_mkdir` |
| `unlink`, `remove_directory` | `f_unlink` |
| `rename` | `f_rename` (fails when the target exists, as SxFS does) |
| `refresh` | `f_stat`: size and the read-only attribute |
| `stats` | `f_getfree` and the volume size |
| `sync` | `CTRL_SYNC`; files are already flushed on `f_close` |
| `set_ownership` | `nullptr`: FAT has no owners, so `chmod`/`chown` are refused |
| `unmount` | `f_unmount` and free the slot. `fs::release` has already synced the volume and detached its tree (refused while a file is open), as for SxFS ([FILESYSTEMS.md](../kernel/FILESYSTEMS.md#mounting-and-unmounting-at-run-time)) |

Points worth knowing before writing it:

- **A vnode's record is a path, not a cluster.** FatFs opens files by name, so
  `vnode.data` holds the FatFs volume and the path inside it. Renaming a
  directory has to rewrite the stored path of every vnode below it.
- **Every `read` opens the file again**, which walks the directories on the
  way. Fine for `limine.conf` and a kernel image; a per-vnode open `FIL` cache
  is a later optimisation, not part of the first version.
- **Ownership is synthesized.** Everything is owned by the system: directories
  `0755`, files `0644`, minus the write bits when the volume or the file
  (`AM_RDO`) is read-only. No execute bit on files, so nothing runs from a FAT
  volume. While the driver is read-only, that is `0555` and `0444` everywhere;
  the system passes the permission check for writing anyway, so what refuses
  it is the vnode published without `writable` and the write operations, which
  exist (the VFS calls them unchecked) and fail. `mtime` is the FAT date, local
  time like the RTC's, in 2-second steps.
- **What attach leaves out.** A name longer than a VFS path component (63
  bytes), a path longer than 255, and anything below 24 levels of directories.
  They are counted and logged (`fat: /boot: N entries left out ...`), never
  truncated. Each directory level being published is on the heap, not on the
  32 KiB kernel stack.
- **The whole tree is published at attach, and the VFS has 2048 nodes**
  shared by the initramfs, `/dev` and every mounted volume
  ([FILESYSTEMS.md](../kernel/FILESYSTEMS.md#limits)). The ESP has a few
  dozen; a FAT stick mounted from Disk Management can have far more than what
  is left. `attach` fails cleanly when the VFS runs out, and `fs::release`
  takes back the part already published, as it does for a volume that never
  attached. Publishing lazily, a directory when it is first opened, is the real
  fix and a VFS change of its own.
- **Priority below SxFS** (SxFS is 100). A FAT32 boot sector and an SxFS
  superblock must not be mistaken for each other; SxFS probes first.

## Where the ESP is mounted

At boot, right after the root, `storage::mount_boot_volume` mounts at `/boot`
the first FAT volume that holds `boot/kernel.elf`, trying the partitions
`partition::info` flags as `esp` first. In the development run there is no ESP
partition; the boot drive is QEMU's virtual FAT (`-drive file=fat:rw:...`),
which presents an MBR disk with one FAT16 partition (`ahci0p1`), not of type
`0xEF`, and the boot log says `storage: /boot is ahci0p1`. Requiring
`boot/kernel.elf` keeps another system's ESP from being taken for ours.

Only the FAT driver is tried at `/boot` (`fs::mount` with a driver name): a
candidate that is not the boot volume is attached, looked at and released, and
an SxFS volume must not go through that, since mounting one can replay its
journal onto the disk.

**`/boot` is not unmounted.** `storage::unmount` refuses it as it refuses `/`
(`EINVAL`), and Disk Management does not offer Unmount on it.

The SxFS image has no top-level `boot`, so the mount point does not hide
anything. `fs::kMaxMounts` is 4, so with the ESP held for the whole session
two slots are left for `/mnt` mounts, the installer's target among them.

**Raw writes to the mounted ESP are refused.** `storage::write` returns
`EBUSY` for a range that overlaps a mounted volume. Setup writes the ESP image
of another disk and is not affected; the updater cannot rewrite the running
system's ESP image while `/boot` is mounted, which is one more reason it
replaces files instead (phase 3).

**The development boot drive must not be written from the guest.** QEMU's
`fat:rw:` writes back into the host's staging directory, and its write support
is known to be fragile. Before the write phase, `run_qemu.py` drops `rw` (Limine
does not write), the block device becomes read-only, and the driver mounts it
read-only. Writes are tested on a disposable FAT image instead.

## Phases

Each phase builds, boots and is verified on its own.

0. **Root guard and vendoring. Done.** `can_be_root`, checked in `fs::mount`
   so no path can mount `/` with another driver, and `fs-registry-test` (a FAT
   boot drive ahead of the system disk, a boot with only FAT, mounts outside
   `/`); `vendor/fatfs/`, `kernel/fatfs/ffconf.h`, the CMake object library
   and the provenance record. Nothing calls FatFs yet.
1. **Read-only driver. Done.** `probe`, `attach`, `read`, `refresh`, `stats`,
   the boot volume at `/boot`, its unmount refused. `fat-volume-test` formats
   FAT12, FAT16 and FAT32 images with FatFs through the kernel's own disk layer
   (`fat_disk::bind`), mounts them at `/boot` and reads them back: long UTF-8
   names, reads at any offset of a multi-cluster file, ownership, refused
   writes, a name left out, a partitioned disk not claimed although FatFs alone
   would mount it, a tree larger than the VFS, dates. The core `smoke` reads
   `/boot/limine.conf` and the ELF magic of `/boot/boot/kernel.elf` and is
   refused creating or writing there; `native-test` checks `/boot` is the
   `fat` partition and that its unmount is `EINVAL`.
2. **Writes.** The rest of the operations; the read-only development drive.
   The host test covers create, write, truncate, rename of a directory with
   files inside, unlink, a full volume and a read-only file. A smoke that
   writes to a disposable FAT image on a second disk, syncs, reboots and reads
   it back. Two to four days.
3. **Consumers.** The updater replaces `boot/kernel.elf` and
   `boot/initramfs.cpio` as files and edits `limine.conf`, instead of rewriting
   the ESP image ([SETUP_AND_UPDATES.md](../userland/SETUP_AND_UPDATES.md),
   phase 5). Disk Management's Mount accepts FAT volumes, not only SxFS (the
   check in `diskmgmt.c`; the kernel side needs nothing), and its
   `diskmgmt-smoke` mounts a FAT partition on the blank disk. Formatting as
   FAT from Disk Management would need FatFs in userland and is not planned.
   Removable media wait for a USB mass-storage driver, which does not exist.

## Limits accepted

- No exFAT, no FAT volume as `/`, no executables on FAT.
- Volumes up to 2 TiB (32-bit sector numbers).
- No `chmod`/`chown` on FAT; ownership is fixed.
- Unmount follows SxFS's (`fs::release`); the ESP at `/boot` stays mounted.
- A FAT volume with more files and directories than the VFS has free nodes
  does not mount.

## Open questions

- Whether the ESP should stay read-only for everyone and be remounted writable
  only while the updater runs, rather than writable by the system all the time.
  Userland can only mount at `/mnt/<name>`, so that remount would be a kernel
  path of its own.
- Whether `df` and `sysinfo` should list every mounted volume. Today they
  describe the root only.
