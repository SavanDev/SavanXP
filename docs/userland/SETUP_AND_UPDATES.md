# Setup and updates — installing SavanXP and keeping it current

> **Status: planned (2026-11-05). Nothing here is built.** Programs are
> installed from the host (`tools/build-user.sh`, `tools/sxfs_sync.py`), the
> LiveCD forgets everything on reboot, and there is no way to move from one
> version to the next other than rebuilding `build/disk.img`. There is also no
> settings page of any kind for updates: no source, no channel, no "check now".
> This document records the decision on the shape of both tools, what the
> system already has for them, and the order to build the rest in.

## The short version

- **Setup is a program that runs on the system**, from the LiveCD: a
  Windows 2000-style wizard on the live desktop, not a separate text-mode setup
  booted before the OS. The LiveCD's boot menu gets an *Install SavanXP* entry
  that opens the wizard instead of the desktop.
- **Setup is an engine with front ends.** A library and a `setup` command do the
  work (partition, format, boot files, copy, record). The wizard only asks
  questions and shows progress. A text-mode front end stays possible later and
  would change nothing below it.
- **The first updater is setup in upgrade mode.** You boot a newer LiveCD,
  setup finds the installed system and replaces the system's files while keeping
  the user's own. That works with no network and no cryptography.
- **The updater inside the system comes after that.** It stages an update and
  applies it at the next boot, before the session starts. It gets its own
  settings, which do not exist today.

## Decision: a program on the system, not a text-mode setup

Windows XP's setup started in text mode because Windows could not run its own
GUI from a CD in 2001. SavanXP can: the ISO already boots the full desktop from
the LiveCD volume. A program on the system is the cheaper way, and the kernel
was already built expecting it:

- The SxFS driver keeps its state per volume, and `kMaxVolumes` is 2,
  documented as "root + setup's target": the LiveCD volume as the source and the
  target partition as the destination, mounted at the same time
  (`kernel/sxfs.cpp`). A volume can hang anywhere, such as `/mnt/target`.
- The partition driver (`kernel/partition.cpp`) reads GPT and MBR and marks the
  ESP. It already assumes that setup writes GPT.
- The wizard reuses the SXGUI toolkit, elevation through `elevationd` and the
  `windowd` prompt, and the QMP-driven smokes. A text-mode setup would need a
  text UI over the console that does not exist.
- The updater has to be a program on the system anyway. Writing setup the
  same way lets both share the engine and the UI kit.

What a text-mode setup would have given, and why it is not enough: less RAM (no
desktop; but it still needs the LiveCD volume as the source of the files),
running as the system without a prompt, and the authentic XP look. Its second,
graphical phase on first boot would have little to do here: the user is fixed
as `user` and neither setup nor the first boot asks for a name
([USERS_AND_PERMISSIONS.md](../kernel/USERS_AND_PERMISSIONS.md#decided)).

## What already exists

| Piece | Where | What it gives setup |
| --- | --- | --- |
| GPT/MBR reader, ESP detection | `kernel/partition.cpp` | Partitions become block devices of their own (`ahci0p1`, ...) |
| Two SxFS volumes at once, any mount point | `kernel/sxfs.cpp`, `kernel/fs.cpp` | Source and target mounted together |
| The LiveCD volume | `boot/limine.conf`, `ramdisk::attach_image` | A writable, throwaway root to run from, 256 MiB (`SAVANXP_ISO_SECTORS`) |
| A portable SxFS core | `libsxfs/sxfs_core.c` | Formatting and writing SxFS without the kernel driver; freestanding, no `malloc` |
| Elevation | `system/elevationd`, `windowd` | The wizard asks once and runs elevated |
| The version and the build number | `include/shared/version.h` | Comparing an installed system against new media |
| The list of the system's programs | `/system/programs.txt` | The seed of the install record |
| Disk drivers that write | `ahci`, `ata`, `virtio_blk` | The targets setup can install to |
| Power control | `/dev/power` (`POWER_IOC_REBOOT`) | Rebooting at the end |

## What the target disk looks like

Setup takes a whole disk, after an explicit confirmation that it erases it.
There is no dual boot and no resizing of other systems' partitions.

| Partition | Format | Size | Holds |
| --- | --- | --- | --- |
| 1, ESP | FAT32 | tens of MiB, fixed when the image is built | Limine (`EFI/BOOT/BOOTX64.EFI`), `limine.conf`, `boot/kernel.elf`, `boot/initramfs.cpio` |
| 2, root | SxFS v2 | at most 1 GiB | Everything mounted at `/` |
| rest | — | — | Unused |

**The root is capped at 1 GiB and 4096 inodes** by the format: 512 bitmap
sectors (`SXFS_BLOCK_BITMAP_SECTORS`) cover 2^21 sectors, and
`SXFS_MAX_INODES` is 4096. Lifting either is a format change
([SXFS_ROADMAP.md](../roadmaps/open/SXFS_ROADMAP.md)), not setup's job.

**The ESP is written as an image, not as files.** Limine reads only FAT and
ISO9660, so the kernel and the initramfs must sit on FAT. The build produces
the ESP as a ready image (next to `disk.img` in the ISO) and setup copies it to
partition 1 sector by sector. The kernel has a FAT driver and mounts the
running system's ESP at `/boot`, writable by the system, so the updater can
replace the kernel there as files ([FAT_ROADMAP.md](../roadmaps/open/FAT_ROADMAP.md)).

**The installed `limine.conf` has no `module_path` for `disk.img`.** It is the
development configuration (`boot/limine-dev.conf`): the root comes from the
disk, found by `fs::mount_any`, never loaded into RAM.

**Legacy BIOS** needs what `limine bios-install` does to the ISO, replicated on
the target: the stage 1 in the MBR and the stage 2 where it can find it on a GPT
disk. Whether setup runs that logic in the guest or copies sectors the build
prepared is an open question. UEFI comes first; BIOS may land a phase later.

## What setup needs that does not exist

### In the kernel

1. **Disks visible to the system, and only to it.** Done for Disk Management,
   as calls rather than `/dev/disk<n>` nodes
   ([DISK_MANAGEMENT.md](DISK_MANAGEMENT.md#why-calls-not-devdiskn)): any
   process lists the disks, only the system reaches their sectors, and a write
   overlapping a mounted volume is refused.
2. **Re-reading a partition table.** Done: `SXN_SYS_STORAGE_RESCAN`
   (`partition::rescan`).
3. **Mounting from userland.** Done: `SXN_SYS_STORAGE_MOUNT` mounts at
   `/mnt/<name>`, system only, and `SXN_SYS_STORAGE_UNMOUNT` releases it
   ([FILESYSTEMS.md](../kernel/FILESYSTEMS.md#mounting-and-unmounting-at-run-time)).
4. **Choosing the root from the command line.** `fs::mount_any` keeps the first
   volume a driver claims, in priority order: virtio-blk 110, AHCI and ATA 100,
   the LiveCD ramdisk 10. Booting the LiveCD on a machine that already has
   SavanXP installed mounts the installed disk as `/`, so a reinstall or an
   upgrade would run from its own target. The LiveCD's `limine.conf` has to say
   `root=livecd`, and the kernel has to honour it.
5. **The command line reaching userland.** The kernel does not read the Limine
   command line at all today. The *Install SavanXP* boot entry needs it
   (`setup=1`, or similar) so that `sessiond` starts the wizard instead of the
   desktop.

### In userland

- **The engine** (`libsxsetup`, name not final) and the **`setup` command**
  over it, scriptable and testable without a screen. The engine builds on
  `libsxdisk` (GPT) and `sxfs_mkfs`, which Disk Management already uses.
  Steps, each one resumable or safe to redo: choose the disk, write GPT,
  write the ESP image, format the root with `sxfs_core`, copy the files,
  write the install record, `sync`.
- **The wizard**: welcome, choose a disk (model and size), confirm the erase,
  progress, reboot. Opened full screen by the boot entry, and from an *Install
  SavanXP* icon on the live desktop.
- **The install record**, written into `/system` while elevated. It is the
  registry that `USERS_AND_PERMISSIONS.md` and `SXE_FORMAT.md` say belongs to
  the first installer: what setup installed, the version and build, and which
  files are the system's. Add or Remove Programs stops subtracting
  `programs.txt` and reads it.

## Updates

### There are no update settings today

The settings that exist are per user, files in the profile through
`savanxp_settings_path` (`display.cfg`, `desktop.cfg`, the Start menu's
registry), each owned by the program that uses it. There is no Control Panel,
and nothing system-wide. Update settings are system-wide, so they live in
`/system/update.cfg`, readable by everyone and written only while elevated:

- the **source**: none (default), installation media, or a URL;
- **automatic check**: off by default;
- the **last check** and its result;
- the **installed version and build**, read from the install record, never typed.

They get a program of their own, *Updates*, under *System* in the Start menu,
next to Add/Remove Programs. When a Control Panel exists, it becomes one of its
pages. The Windows 2000 model applies: check, list what would change, apply on
the next restart.

### Upgrade from newer media first

The first way to update is booting a newer LiveCD. Setup sees an installed
system on the disk (its install record), compares versions, and offers
**Upgrade** next to **Install**. The upgrade:

- replaces only the files the old install record lists as the system's, and
  writes the new record;
- never touches `/users`, `/games`, programs installed from outside, or
  anything else the record does not list. This is the same rule the host build
  follows for `build/disk.img` ([AGENTS.md](../../AGENTS.md)), moved into the
  system;
- rewrites the ESP image, which brings the new kernel and initramfs.

It needs nothing from the network and no signatures: the media is trusted
exactly as much as the media that installed the system. And it runs from the
LiveCD, so nothing on the target is in use.

### Then the updater inside the system

Updating the system it is running on brings three problems setup does not have:

- **Files cannot be replaced in place.** `rename` onto an existing file fails
  with `EEXIST`, and `unlink` frees a file at once, even an open one.
- **The section cache would serve stale images.** It keys on the vnode, and a
  write keeps the vnode, so a library rewritten in place keeps loading the old
  bytes. `SECTIONS.md` records this as unreachable only because nothing writes
  executables in place, and asks for a content generation and its test the
  moment something does
  ([SECTIONS.md](../kernel/SECTIONS.md#section_open-and-what-it-does-not-do-yet)).
  The updater is that moment.
- **`/bin` and `/lib` are in use** while the session runs.

So the updater **stages and applies at boot**, as Windows does with pending
file renames: it downloads or copies the update into `/system/update/pending`,
checks it, and asks for a restart. `init` applies it before it starts
`sessiond`, running as the system, when nothing from `/bin` or `/lib` beyond
`init` itself is loaded. Progress is written down as it goes, so an
interrupted apply resumes on the next boot instead of leaving a mixed system.
The SxFS journal keeps metadata consistent, but nothing inside the running
system repairs a volume: `fscheck` reports, and repair is `sxfs-cli repair` on
the host ([SXFS_CHECK.md](../fs/SXFS_CHECK.md)). So the updater has to avoid
the damage rather than count on fixing it.

**An update package** is a manifest (from version, to version, every file with
its size and SHA-256, the ESP image if the kernel changes) plus the payload.
The format is not decided.

**Before any network source:** SHA-256 for every file, checked before anything
is applied, and an Ed25519 signature over the manifest. There is no TLS, so
plain HTTP over the existing TCP and DNS stack is acceptable only because the
signature, not the connection, is what is trusted. Neither algorithm exists in
the tree today.

## Phases

Each phase boots and is verified on its own.

0. **Kernel.** System-only block devices in `/dev`, partition rescan, mount from
   userland, `root=` on the command line, the command line reaching userland.
1. **Engine and `setup`.** Checked by a new smoke: boot the ISO with a blank
   second disk, run `setup` from the console, boot from that disk, and assert
   the root, a program and the session.
2. **The wizard and the boot entry.** *Install SavanXP* in the LiveCD's Limine
   menu and on its desktop; a QMP scenario that walks the wizard.
3. **The install record.** Add or Remove Programs reads it.
4. **Upgrade from newer media.** The persistence check moves into the guest:
   an external program and a file in `/users` survive an upgrade.
5. **Updates and its settings.** `/system/update.cfg`, the *Updates* program,
   staging and apply at boot, the section cache generation with its test. Local
   sources only.
6. **Network source.** SHA-256, Ed25519, HTTP.

Legacy BIOS boot for an installed disk fits anywhere after phase 1.

## Limits accepted

- One root of at most 1 GiB and 4096 files, until the SxFS format moves.
- Targets on AHCI, IDE, virtio-blk and NVMe only, and NVMe only when booted
  with `nvme-rw` ([NVME.md](../kernel/NVME.md)). No USB, so in practice
  virtual machines and SATA, IDE or NVMe machines
  ([REAL_HARDWARE.md](../hardware/REAL_HARDWARE.md), HW-3 to HW-5).
- The whole disk, no dual boot.
- One user, `user`.

## Open questions

- The size of the ESP, and whether the ISO carries the ESP image as a file or
  setup builds it from the files.
- How legacy BIOS boot is laid down on a GPT target.
- The update package format.
- Where network updates would be published.
