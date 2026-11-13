# SavanXP documentation

Design documents, decisions, and specifications. `README.md` at the repository
root covers how to build and run; this directory explains how the system is put
together and why.

Anything that has to stay true for the future of the OS lives here, not in a
changelog entry. See the rules in [`AGENTS.md`](../AGENTS.md).

## Build — `build/`

- [`BUILD_CMAKE.md`](build/BUILD_CMAKE.md) — the native Bash/CMake build graph, tools,
  targets, external applications, version and build number, and persistent-image rules.
- [`BUILD_LINUX.md`](build/BUILD_LINUX.md) — host packages, QEMU setup, official
  ports, smoke tests, KVM configuration, and the GitHub Actions CI.

## Kernel — `kernel/`

- [`ARCH.md`](kernel/ARCH.md) — the architecture boundary: what lives in `arch/x86_64`
  and `include/arch/x86_64`, the named CPU operations drivers call instead of
  inline assembly, and what a second architecture would change.
- [`KERNEL_NATIVE_API.md`](kernel/KERNEL_NATIVE_API.md) — the NT-style kernel API:
  the native dispatcher boundary, the slice plan, and what stays invariant.
- [`KERNEL_SECURITY.md`](kernel/KERNEL_SECURITY.md) — the kernel trust boundary and
  audit record.
- [`USERS_AND_PERMISSIONS.md`](kernel/USERS_AND_PERMISSIONS.md) — the planned user model:
  one everyday account, NT-style elevation through a broker and a prompt `windowd`
  owns, owner/group/mode in the SxFS inode, and the phases to build it.
- [`USER_ADDRESS_SPACE.md`](kernel/USER_ADDRESS_SPACE.md) — the user half's layout
  (image, views, stack, kernel boundary) and why programs load at `0x400000`.
- [`SECTIONS.md`](kernel/SECTIONS.md) — section objects and views: the grant model,
  execution as a view permission, W^X, the view and section budgets, and where
  shared library images belong.
- [`SHARED_LIBRARIES.md`](kernel/SHARED_LIBRARIES.md) — the shared-library subsystem: what
  is built and measured, the decisions and the bugs behind them, and the remaining
  work as ordered phases with the dependencies between them.
- [`SYSTEM_LOG.md`](kernel/SYSTEM_LOG.md) — the system log: the ring of lines sent
  to the serial port, who each line is attributed to, and who may read it
  (`SXN_SYS_LOG_READ`).
- [`VIRTIO.md`](kernel/VIRTIO.md) — the virtio-pci transport and its MMIO rules.
- [`FILESYSTEMS.md`](kernel/FILESYSTEMS.md) — the VFS, the `fs::` registry and
  what a filesystem driver provides: the system disk as the root, `/dev` and
  `/rescue` in memory and the `/disk` alias, the rescue system, how a request
  finds its driver, the rules the VFS enforces for all of them, and what still
  names SxFS.
- [`INPUT.md`](kernel/INPUT.md) — the input layer: how drivers register, why
  registration order decides which keyboard and pointer win, and the keyboard
  core that turns set 1 key positions into keys, text and modifiers.
- [`USB_HOST_CONTROLLERS.md`](kernel/USB_HOST_CONTROLLERS.md) — xHCI, EHCI, UHCI and OHCI
  behind one controller table: start order, taking them from the firmware,
  EHCI's companion routing, each one's schedule, and what QEMU cannot test.
- [`AHCI.md`](kernel/AHCI.md) — the AHCI (SATA, DMA) disk driver: which QEMU port
  holds what, how a request runs, and what it measured against PIO.
- [`BLOCK_WRITEBACK.md`](kernel/BLOCK_WRITEBACK.md) — deferred disk writes: the
  queue's contract, how it keeps write order, when it drains, and what durability
  it gives up.
- [`AZALIA.md`](kernel/AZALIA.md) — pinned OpenBSD analog HDA playback/capture port, source map,
  codec routing, DMA lifetime, controller recovery and verification boundaries.
- [`TIME.md`](kernel/TIME.md) — kernel clocks, virtualization, and diagnosis; plus the
  calendar date: the CMOS RTC, manual setting (`date`, `SET_REALTIME`), network
  sync (`ntpsync`, SNTP), and the taskbar clock with its calendar popup.

## Hardware targets — `hardware/`

- [`REAL_HARDWARE.md`](hardware/REAL_HARDWARE.md) — the targets and their status,
  what `master` requires of any machine, the instruction-set baseline rule, how the
  ISO boots on metal, the shared driver work packages (HW-1 to HW-15) and their
  order, and how to inventory a machine and record a run.
- [`RYZEN5_1600_VEGA56.md`](hardware/RYZEN5_1600_VEGA56.md) — the development host:
  inventory, what was verified on it, the Vega 56 on the firmware framebuffer and
  why no native driver is planned, the ALC892 notes and the pending audio checks.
- [`PENTIUM4_EVO_N1020V.md`](hardware/PENTIUM4_EVO_N1020V.md) — the Compaq laptop:
  why it cannot boot `master`, whether `master` could support it, the state of the
  i386 port on `legacy`, and the 64-bit Pentium 4s that are `master` targets.
- [`ATHLON_II.md`](hardware/ATHLON_II.md) — AM3 desktops with AMD 7/8-series
  chipset graphics: the C1E timer check, SB7x0 devices and the work in order.
- [`CORE2_DUO.md`](hardware/CORE2_DUO.md) — Core 2 machines with Intel GMA
  graphics: telling Core 2 from the 32-bit Core Duo, the XD bit, ICH devices, and
  why native mode setting is most plausible here.
- [`ATHLON_64.md`](hardware/ATHLON_64.md) — K8 desktops, the oldest CPU `master`
  can boot: the untrustworthy TSC, the four chipset families and their devices.
- [`COMPAQ_21N2F7AR.md`](hardware/COMPAQ_21N2F7AR.md) — a Skylake-U laptop (Core i7-6500U,
  HD Graphics 520) verified on a legacy boot: the unit against the retail listing,
  what the boot log shows device by device, performance, and what is left.

## Userland — `userland/`

- [`SYSTEM_LAYERING.md`](userland/SYSTEM_LAYERING.md) — the layering: the platform,
  apps on the POSIX SDK, when an app may be a SUN program instead, and the
  Win32 (SSW) and Linux binary-compatibility subsystems and their roadmaps.
- [`SUN.md`](userland/SUN.md) — SUN, the Savan Userland Native API: what it is
  next to Sx and POSIX, its pieces, the system's own API rather than a Win32
  replica, the rules that grow it, its Win32 counterparts as a reading aid, and
  the slices the Command Prompt drives.
- [`WIN32.md`](userland/WIN32.md) — SSW, the Savan Subsystem for Windows (the
  Win32 subsystem), as built: the PE32
  parser and what it refuses by name, the loader and its NT statuses, the
  gate and process start and exit, the first DLLs, the reproducible fixture
  build, the C runtime, `kernel32` and registry functions of W2 and what they
  refuse, the registry file, `winhost` on SavanXP and the host harness, the
  view budget, and the fixtures' output recorded on Windows XP.
- [`WIN32_SOLITAIRE_PROFILE.md`](userland/WIN32_SOLITAIRE_PROFILE.md) — the
  measured scope of the first Win32 profile: the user's Windows XP `sol.exe`
  and `cards.dll` by hash, their headers, the 124 imported functions by DLL,
  their resources, a Wine trace that classifies each function by when it is
  needed, and the Windows XP reference captures.
- [`NATIVE_APPS.md`](userland/NATIVE_APPS.md) — how to write a SUN program:
  headers, connecting a window, manifests, and the worked examples.
- [`NIGHTWOOD.md`](userland/NIGHTWOOD.md) — Nightwood, the forest game: the first game
  written against SUN and SxGL's second consumer; its rules, its frame and its tests.
- [`NATIVE_HEAP.md`](userland/NATIVE_HEAP.md) — `malloc` for native programs: the
  allocator's replaceable page source, what a native process has today, the plan.
- [`SYSTEM_MONITORING.md`](userland/SYSTEM_MONITORING.md) — process, memory, and system
  metrics exposed by the kernel.
- [`SXE_FORMAT.md`](userland/SXE_FORMAT.md) — the executable format, resource sections,
  generator, stamping, runtime rules, and why SUN programs are named `.sxe`.
- [`DISK_MANAGEMENT.md`](userland/DISK_MANAGEMENT.md) — Disk Management: the window,
  why storage is reached through calls and not `/dev/disk<n>`, the SxFS GPT type GUID,
  the rules for the operations that write, and its tests.
- [`LOG_VIEWER.md`](userland/LOG_VIEWER.md) — Log Viewer: the system log on screen,
  by subsystem and by program, how it spots problems, and `logview --print`.
- [`SETUP_AND_UPDATES.md`](userland/SETUP_AND_UPDATES.md) — the planned setup and
  updater: a wizard on the LiveCD over a scriptable engine, the target disk
  layout, the kernel pieces still missing, upgrade from newer media, the update
  settings that do not exist yet, staging and apply at boot, and the phases.

## Desktop and graphics — `desktop/`

- [`DESKTOP_ARCHITECTURE.md`](desktop/DESKTOP_ARCHITECTURE.md) — the target split
  between `compositord`, `windowd` and `desktop` (NT model), window roles,
  global hotkeys, the Task List removal, the move onto the native API, and the
  phases to get there.
- [`WM_SUBSYSTEM.md`](desktop/WM_SUBSYSTEM.md) — the window-manager subsystem, its
  client protocol, and compositor model.
- [`OWNED_WINDOWS.md`](desktop/OWNED_WINDOWS.md) — ownership rules for dialogs and
  other windows created by an application.
- [`PLATFORM_GRAPHICS.md`](desktop/PLATFORM_GRAPHICS.md) — SXGFX, SXCHROME and
  SXGUI-C as platform code: the no-system-call rule, what stays in each
  subsystem (the window client, the application frame), and the toolkit's host.
- [`SXGL.md`](desktop/SXGL.md) — SxGL, the 3D rasterizer: the GL 1.1 subset call by
  call, who presents, and every deliberate difference from the specification.
- [`SXGL_DIAG.md`](desktop/SXGL_DIAG.md) — SxGL Diagnostic Tool (`sxgldiag`):
  where each page's figures come from, the 3D test of each backend and how it
  judges a frame, the saved report and `--selftest`.
- [`GRAPHICS_PERF.md`](desktop/GRAPHICS_PERF.md) — graphics measurements and the
  interpretation of the performance tables.
- [`UI_FONTS.md`](desktop/UI_FONTS.md) — the UI, caption and console faces, the
  rule that every UI font is 1-bit, and how to change a face.
- [`START_MENU.md`](desktop/START_MENU.md) — the Start menu: Programs and Settings
  cascades from progman's catalog, the cascade's timing and placement, why it is one
  level deep, the banner and its edition switch.
- [`DESKTOP_ICONS.md`](desktop/DESKTOP_ICONS.md) — desktop icons, shortcuts and
  context menus: the Desktop folder, the `.sxlnk` format and what "open"
  means, the icon grid and its keyboard, the desktop's focus in `windowd`,
  and context menus in applications (popups owned by any window,
  `sxgui_context_menu_open`).
- [`DISPLAY_PROPERTIES.md`](desktop/DISPLAY_PROPERTIES.md) — Display Properties:
  the wallpaper configuration (`desktop.cfg`) and how the desktop follows it,
  the shipped wallpapers, and changing the resolution while the session runs
  (the session's largest mode, the layout after a change, `display.cfg`).
- [`FILES.md`](desktop/FILES.md) — Files, the file browser: the toolbar,
  address bar, history and Properties, the toolbar widgets
  (`SXGUI_FLAG_TOOLBAR`), the Large Icons and Details views over one widget
  (`SXGUI_ICONVIEW`), the per-size icon caches and their RAM, and what is next.
- [`FILE_DIALOG.md`](desktop/FILE_DIALOG.md) — the common Open and Save As
  dialogs for SUN programs (`sxnfiledlg.h`): the window, what its buttons do,
  the non-blocking API, and what it leaves out.
- [`ICON_STYLE.md`](desktop/ICON_STYLE.md) — the Windows 2000 look of the house
  icon set: the drawing rules, the shared kit and generator, and what is exempt.
- [`CONSOLE.md`](desktop/CONSOLE.md) — the Command Prompt: the console host (SUN)
  and `sh` as two programs, their handles and control channel, line editing,
  Ctrl+C without signals, and how the smokes open programs through it.
- [`PAINT.md`](desktop/PAINT.md) — Paint, the Accessories sketchbook: toolbox
  layout without widgets, the 8 tools, 8-deep undo, BMP/PNG files, and the
  selftest plus visual scenario.
- [`WEBVIEWER.md`](desktop/WEBVIEWER.md) — Web Viewer, the in-tree browser prototype:
  its rendering contract, the CSS subset, and what each milestone leaves out.

## Media — `media/`

- [`MEDIA_PLAYER.md`](media/MEDIA_PLAYER.md) — Media Player, the optional FFmpeg
  backend, playback timing, and the synchronization selftest.
- [`SOUND.md`](media/SOUND.md) — SUN Sound panel, popup shortcut, master and
  application controls, input selection, recording test, live meters and preferences.
- [`AUDIO_DAEMON.md`](media/AUDIO_DAEMON.md) — `audiod`: native and legacy UDP
  transports, shared recording, trusted application identities and durable settings.
- [`MIDI.md`](media/MIDI.md) — `libsxmidi`, the General MIDI synthesizer: the SMF
  parser, the built-in instrument bank, and how the Doom port plays music
  through it.
- [`SXMEDIA.md`](media/SXMEDIA.md) — **withdrawn.** What the multimedia layer was, what
  building it taught, and what has to exist before trying again: a way for one
  program to use a codec library it was not built with.

## Filesystem — `fs/`

- [`SXFS_CHECK.md`](fs/SXFS_CHECK.md) — checking, recovery and repair: the
  `fscheck` report, looking at a volume without writing to it, journal
  recovery on the host, and `sxfs-cli repair` with what it does and leaves.
- [`SXFS_JOURNAL.md`](fs/SXFS_JOURNAL.md) — the sparse metadata journal: format,
  the write order that makes a commit crash-safe, and the snapshot invariant that
  tells the driver which sectors changed.
- [`SXFS_NAMESPACE.md`](fs/SXFS_NAMESPACE.md) — how create, delete and rename are
  atomic across a power cut: directories copied on change, blocks a transaction
  frees left alone until it commits, doomed inodes, and deleting on a full volume.

## Networking — `net/`

- [`NETWORKING.md`](net/NETWORKING.md) — TCP guarantees, retransmission, windows,
  fault injection, and the minimal DNS client.

## Roadmaps — `roadmaps/`

The order of work for the larger efforts, phase by phase. A roadmap lives in
`roadmaps/open/` while any phase is pending and moves to `roadmaps/done/` once
all its phases have landed; the links to it move with it.

### Done — `roadmaps/done/`

- [`THREADS_ROADMAP.md`](roadmaps/done/THREADS_ROADMAP.md) — threads: what moves from the
  process to the thread, the decisions taken, and the phases to get there.
- [`SXGFX_ROADMAP.md`](roadmaps/done/SXGFX_ROADMAP.md) — hardening of the 2D rasterization
  layer and its planned role in the desktop stack.
- [`SXGL_ROADMAP.md`](roadmaps/done/SXGL_ROADMAP.md) — why the 3D layer sits beside SxGFX, the
  design it was built against, and how its batches landed.
- [`USB_ROADMAP.md`](roadmaps/done/USB_ROADMAP.md) — the USB stack: xHCI first and polled,
  HID, mass storage, hubs and hot plug, what was copied from OpenBSD and what
  was only read, the QEMU harness, and the run on real hardware.

### Open — `roadmaps/open/`

- [`SYSTEM_LIMITS_ROADMAP.md`](roadmaps/open/SYSTEM_LIMITS_ROADMAP.md) — how many windows,
  descriptors and kernel objects fit today, the chain of limits from the owned windows
  per process down to the kernel tables, what each raise costs, and the order to do them.
- [`SMP_ROADMAP.md`](roadmaps/open/SMP_ROADMAP.md) — more than one core: startup, the big
  kernel lock, TLB shootdown and the lock's counters, all done; and phase 5,
  taking the lock apart, planned with the measurement that says when.
- [`DRIVERS_ROADMAP.md`](roadmaps/open/DRIVERS_ROADMAP.md) — native driver registries,
  common device/resource/I/O lifetime, general Plug and Play and safe removal,
  plus a SUN Device Manager inspired by Windows XP, with staged acceptance gates.
- [`LINUX_SUBSYSTEM_ROADMAP.md`](roadmaps/open/LINUX_SUBSYSTEM_ROADMAP.md) — Linux
  binary compatibility on Sx under `/system/linux`, for the first application
  goal, Xeyes: a translation layer in the kernel entered by trapping
  `syscall`, memory regions, a musl profile, a small X11 server (`x11d`), the
  paths rejected, the kernel slices, and phases L0-L7 with their gates.
- [`WIN32_SUBSYSTEM_ROADMAP.md`](roadmaps/open/WIN32_SUBSYSTEM_ROADMAP.md) — SSW,
  Windows binary compatibility under `/system/win`, for the first application goal,
  Windows XP Solitaire: the chosen architecture (own PE32 DLLs over a 64-bit
  SUN host, no Windows code in the kernel), the paths rejected, the two
  kernel slices, and phases W0-W7 with their acceptance gates.
- [`WEBVIEWER_ROADMAP.md`](roadmaps/open/WEBVIEWER_ROADMAP.md) — Web Viewer's network-first
  SUN migration and HTTP/HTTPS plan, implementation checkpoint and verification;
  later integration of the pinned Ladybird engine and its acceptance gates.
- [`FAT_ROADMAP.md`](roadmaps/open/FAT_ROADMAP.md) — the planned FAT driver: FatFs adopted
  under `vendor/`, how it maps onto `fs::Driver`, why the root stays SxFS, the
  ESP at `/boot`, and the phases.
- [`VFS_ROADMAP.md`](roadmaps/open/VFS_ROADMAP.md) — the shared filesystem foundation:
  native I/O, identity and lifetime, on-demand lookup, mutation/durability,
  mounts and VM coherence, with conformance gates for current and future drivers.
- [`SXFS_ROADMAP.md`](roadmaps/open/SXFS_ROADMAP.md) — what closed v2 (consistency, file
  contract, recovery and repair, driver-side costs) and who owns what it left;
  the planned stable v3 format, subsystem boundaries, migration and production
  acceptance gates, with historical performance measurements.
- [`XASH3D_ROADMAP.md`](roadmaps/open/XASH3D_ROADMAP.md) — porting Xash3D FWGS: what the
  engine needs from the platform, and the order that makes it pay for `dlopen` and
  SxGL without pulling in threads or SDL.
- [`VIRGL_ROADMAP.md`](roadmaps/open/VIRGL_ROADMAP.md) — a hardware backend for SxGL:
  virgl in `virtio_gpu`, per-process 3D ioctls on `/dev/gpu0`, an in-tree virgl
  encoder, readback into the window surface, and the `--gl` QEMU machine.

- [`AZALIA_ROADMAP.md`](roadmaps/open/AZALIA_ROADMAP.md) — OpenBSD HDA port for
  the Ryzen/ALC892 host, six-stage completion contract, software evidence, the
  2026-11-11 audit with the remaining physical checks and software backlog;
  HDMI is a separate future objective.

## Third-party code — `third-party/`

- [`THIRD_PARTY_ADOPTION.md`](third-party/THIRD_PARTY_ADOPTION.md) — the policy for adopting
  external code.
- [`THIRD_PARTY_PROVENANCE.md`](third-party/THIRD_PARTY_PROVENANCE.md) — origin, pinned
  version, and verified license for distributed third-party components.

- [Desktop cursors](desktop/CURSORS.md): Mouse Properties, selectable CUR packs, licensing and rendering fallbacks.
