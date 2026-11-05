# FAT — a second filesystem driver, on FatFs

Status: **planned**. Nothing here is implemented yet.

SavanXP reads and writes only SxFS. The one FAT volume that matters today, the
EFI System Partition, is written as a whole image because "the kernel has no
FAT driver" ([SETUP_AND_UPDATES.md](../userland/SETUP_AND_UPDATES.md#what-the-target-disk-looks-like)).
This page plans that driver: which code, how it plugs into `fs::`, what has to
change outside it, and the phases.

## The short version

- **Adopt FatFs** (ChaN) unmodified under `vendor/fatfs/`, as uACPI is, with
  glue of our own in `kernel/fat.cpp` that implements `fs::Driver`.
- **SxFS stays the only root.** Before any FAT driver is registered,
  `fs::mount_any` has to stop accepting a FAT volume as `/`.
- **The ESP is mounted at `/boot`**, owned by the system, read-only first,
  writable in a later phase. Nothing runs from it.
- **FAT12, FAT16 and FAT32 with long names. No exFAT.**
- Effort: one to two weeks, most of it in the glue and the tests.

## Decision: FatFs, not fastfat, not our own

| Option | Why not / why |
|---|---|
| Microsoft's `fastfat` sample | 30–40k lines written against the NT executive: IRPs, the Cache Manager, `FsRtl` oplocks and notifications, `ERESOURCE`. Porting it means writing an NT compatibility layer larger than the driver. MS-PL on top. `Do not adopt`. |
| A driver of our own | 1500–2500 lines, two to four weeks, and the hard part (write ordering between the FAT and the directories, long-name edge cases) is exactly what FatFs has already been tested on for years. Kept as the fallback if FatFs does not fit. |
| **FatFs** | ~7k lines of portable C, no dependencies, a five-function disk layer, a one-clause BSD-style license compatible with MIT. `Adopt`. |

FatFs is published as a zip on elm-chan.org, not as a git repository. The pin
is the release name (R0.15a as of this writing) plus the zip's SHA-256, in
`vendor/fatfs/FATFS_RELEASE.txt`, next to its license file. The adoption is
recorded in [`THIRD_PARTY_PROVENANCE.md`](../third-party/THIRD_PARTY_PROVENANCE.md).

## Prerequisite: the root is never FAT

`fs::mount_any(kRootMountPoint)` walks the block devices in order and keeps the
first one **any** registered driver claims. With a FAT driver registered:

- the `--no-disk` rescue boot would mount the FAT boot drive (AHCI port 0) as
  `/` instead of falling back to `/rescue`;
- an AHCI or IDE run, where the FAT boot drive is port 0 and `disk.img` is
  port 1, would mount the boot drive as `/`. The default virtio run survives
  only because virtio-blk registers before AHCI.

So `fs::Driver` gains a `can_be_root` field (SxFS: true, FAT: false), and
`mount_any` skips drivers without it. This is a behavior change of its own,
lands first, and is checked by `rescue-smoke` and the AHCI smoke before the FAT
driver exists. The comment in `kernel_main.cpp` around `fs::attach` is still in
Spanish and is translated in the same commit.

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
| `FF_USE_FASTSEEK` | 1 | `read` at an offset without walking the cluster chain every time |
| `FF_USE_MKFS` | 1 | The host test formats its images with it; small |
| `FF_FS_NORTC` | 0 | `get_fattime` from `rtc::epoch_seconds` |
| `FF_FS_REENTRANT` | 0 | Every driver call runs under the big kernel lock ([SMP_ROADMAP.md](../kernel/SMP_ROADMAP.md)). Splitting the filesystem off the BKL means turning this on, with `ff_mutex_*` over a kernel lock |
| `FF_LBA64` | 0 | `block::` addresses sectors with 32 bits |

FatFs is C. It builds as an object library with the kernel's freestanding
flags, the way `savanxp_uacpi_objects` does.

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
  volume.
- **Priority below SxFS** (SxFS is 100). A FAT32 boot sector and an SxFS
  superblock must not be mistaken for each other; SxFS probes first.

## Where the ESP is mounted

At boot, after the root: the first partition `partition::info` flags as `esp`,
mounted at `/boot`. In the development run there is no ESP partition; the boot
drive is QEMU's virtual FAT (`-drive file=fat:rw:...`), a whole device. The
rule there: the first FAT device that holds `boot/kernel.elf`.

The SxFS image has no top-level `boot`, so the mount point does not hide
anything. `fs::kMaxMounts` (4) holds root, ESP, the installer's target and one
removable medium.

**The development boot drive must not be written from the guest.** QEMU's
`fat:rw:` writes back into the host's staging directory, and its write support
is known to be fragile. Before the write phase, `run_qemu.py` drops `rw` (Limine
does not write), the block device becomes read-only, and the driver mounts it
read-only. Writes are tested on a disposable FAT image instead.

## Phases

Each phase builds, boots and is verified on its own.

0. **Root guard and vendoring.** `can_be_root` and `mount_any` (with its
   changelog entry); `vendor/fatfs/`, `ffconf.h`, the CMake object library and
   the provenance record. Nothing calls FatFs yet. Checked by `rescue-smoke`,
   the AHCI smoke and the `disk.img` persistence check.
1. **Read-only driver.** `probe`, `attach`, `read`, `refresh`, `stats`; the ESP
   (or the development boot drive) at `/boot`. A host test,
   `savanxp-fat-volume-test`, that formats FAT12, FAT16 and FAT32 images in
   memory with `f_mkfs`, mounts them at `/boot` and reads them back, long UTF-8
   names included. A smoke asserting that `cat /boot/limine.conf` works and the
   root is still SxFS. About a week.
2. **Writes.** The rest of the operations; the read-only development drive.
   The host test covers create, write, truncate, rename of a directory with
   files inside, unlink, a full volume and a read-only file. A smoke that
   writes to a disposable FAT image on a second disk, syncs, reboots and reads
   it back. Two to four days.
3. **Consumers.** The updater replaces `boot/kernel.elf` and
   `boot/initramfs.cpio` as files and edits `limine.conf`, instead of rewriting
   the ESP image ([SETUP_AND_UPDATES.md](../userland/SETUP_AND_UPDATES.md),
   phase 5). Removable media wait for a USB mass-storage driver, which does not
   exist.

When phase 1 lands, [`FILESYSTEMS.md`](../kernel/FILESYSTEMS.md) gains FAT in
its tables and limits, and `SETUP_AND_UPDATES.md` stops saying the kernel has
no FAT driver.

## Limits accepted

- No exFAT, no FAT volume as `/`, no executables on FAT.
- Volumes up to 2 TiB (32-bit sector numbers).
- No `chmod`/`chown` on FAT; ownership is fixed.
- Unmount follows SxFS's (`fs::release`); the ESP at `/boot` stays mounted.

## Open questions

- Whether the ESP should stay read-only for everyone and be remounted writable
  only while the updater runs, rather than writable by the system all the time.
- Whether `df` and `sysinfo` should list every mounted volume. Today they
  describe the root only.
