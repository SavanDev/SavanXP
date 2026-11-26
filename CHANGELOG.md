# Changelog

Record of user-visible changes per version for SavanXP. This file holds the
changes not released yet; each released version has its own file in
[`docs/changelog/`](docs/changelog/), listed under "Previous versions".

## [Unreleased]

### Added

- **Display Properties gains an Appearance tab: interface fonts and font smoothing, applied live (SDK 2.0).** Face and
  size for window text and title bars in `appearance.cfg`; Effects moved there. `sxnwm_settings_changed`,
  `gfx_settings_changed`, `SAVANXP_WM_EVENT_SETTINGS`; `appearance-smoke`. [Appearance](docs/desktop/DISPLAY_PROPERTIES.md#appearance).
- **The interface's body and caption faces are files in `/system/fonts` (SDK 2.0).** `windowd` (now a PIE), the desktop
  and toolkit programs draw from `MSSansSerif.ttf` and `RetroSans.ttf`, pixel for pixel the baked tables, which static
  programs keep. Test `ui-face-files-test`. [Interface faces from files](docs/desktop/UI_FONTS.md#interface-faces-from-files).
- **The UI, caption and monospace faces can be replaced at run time (SDK 2.0).** `gfx_face_install` puts another
  face in a slot of the text primitives; `sx_font_gfx_face` makes one from a `libsxfont` strike. The baked faces stay
  the default, pixel for pixel. Host test `pixels-text-test`. [Faces at run time](docs/desktop/UI_FONTS.md#faces-at-run-time).
- **Scalable TrueType fonts at run time: `libsxfont.so.0.4` over FreeType 2.15.0 (SDK 2.0).** `savanxp/font.h`
  for SUN and POSIX: faces at any size, 1-bit or grey as each face's gasp says, Windows-style metrics, face
  matching with Arial/Times New Roman/Courier New substitutes. Liberation Sans, Serif and Mono ship in
  `/system/fonts`. Scenario `font-test`, host test `sxfont-test`. [The font library](docs/desktop/FONT_LIBRARY.md).
- **Drivers can start and stop cleanly: the RTL8139, the AC'97 and every virtio driver.** A
  failed start gives back what it took; a stop quiesces, then releases. Boot option
  `lifecycle-selftest=<driver>`. [Driver start and stop](docs/kernel/DRIVER_LIFECYCLE.md).

- **Drivers claim the resources they use, and Device Manager shows them (SDK 2.0).**
  Claimed ranges have their length and `SXN_DEVICE_RESOURCE_CLAIMED`, shared ones
  `..._SHARED`; overlaps are refused. [Claims](docs/kernel/DEVICE_INVENTORY.md#claims).

- **Disks on every AHCI controller are found, not only the first one's.** Up to
  four HBAs and eight disks, numbered in PCI scan order; `smoke ahci-two-hba`.
  [Several controllers](docs/kernel/AHCI.md#several-controllers).

- **A PCI function has one driver at a time.** `inventory::pci_claim` refuses a
  function another driver holds and `pci_release` gives one back; boot option
  `inventory-selftest` and `smoke inventory-claims` check it. [Ownership](docs/kernel/DEVICE_INVENTORY.md#ownership).

- **The Xash3D client plays sound.** The engine's mix reaches `audiod`, or
  `/dev/audio0` without the daemon. [Port notes](ports/xash3d/README.md#graphical-client).

- **Xash3D appears under Games in All Programs.** Started with no arguments,
  `xash3d-client` saves to the desktop profile and reads the game and `ref_soft.so`
  from `/games/xash3d`. [Port notes](ports/xash3d/README.md#graphical-client).

- **Xash3D installs Half-Life data with `--valve PATH`.** It packs loose
  sounds for SxFS and uses the desktop profile for settings and saves.
  [Port notes](ports/xash3d/README.md#graphical-client).

- **Xash3D offers a graphical Half-Life client for POSIX.** Build with
  `ports/xash3d/build.sh --client` for MainUI, the HLSDK client and software
  rendering. [Port notes](ports/xash3d/README.md#graphical-client).

- **POSIX windows can capture the pointer (SDK 2.0).** Use
  `gfx_set_pointer_capture` and `gfx_poll_window_motion` to read relative mouse
  input while the window is active. [SDK reference](subsystems/posix/sdk/v1/REFERENCE.md#graphics-runtime).

- **Commit hooks check the mechanical rules of `AGENTS.md`.** `git config core.hooksPath tools/git-hooks`
  enables them: binaries, WADs and logs, machine paths in tooling, changelog entry size and released notes.
  [Commit checks](docs/build/BUILD_CMAKE.md#commit-checks).

- **`build.sh --quiet` (or `SAVANXP_QUIET=1`) prints a summary instead of the full build output.**
  Logs go to `build/build-logs/`; failures show Ninja's errors and ctest's failed or skipped tests, and a
  build of 300+ steps says why. `--verbose` overrides it. [Quiet output](docs/build/BUILD_CMAKE.md#quiet-output).

- **Device Manager and the hardware inventory.** `devmgmt` (System menu) lists the
  hardware by type or by connection, with each device's driver, state, problem
  marks and Properties; read-only. It reads `SXN_SYS_DEVICE_SNAPSHOT`
  (`sxn_device_snapshot`; POSIX `device_snapshot`): PCI, platform and USB
  instances with stable ids, parents and recorded resources.
  [Inventory](docs/kernel/DEVICE_INVENTORY.md), [Device Manager](docs/userland/DEVICE_MANAGER.md).

- **Xash3D's software renderer draws in a POSIX window.** `ref_soft.so` and
  `xash3d-refprobe` provide a rendering check without game data. [Port notes](ports/xash3d/README.md).

- **Xash3D starts a dedicated Half-Life server over IPv4 UDP.** `xash3d-dedicated`
  loads `c0a0` and answers server-info queries using POSIX. [Port notes](ports/xash3d/README.md).

- **The Xash3D port reads loose files and PAK archives on SavanXP.** The port builds
  `filesystem_stdio.so` and `xash3d-fsprobe` for POSIX. [Port notes](ports/xash3d/README.md).

- **Linux can prepare a UEFI SavanXP disk.** `build.sh prepare-disk` creates GPT, EFI and SxFS on a new image or
  an explicitly selected whole disk. `SAVANXP_DISK_IMAGE` also accepts an existing SxFS partition for port installs.

- **QEMU can use a personal SxFS volume.** `build.sh run` and `debug` accept `--disk-image` or `--disk-device`.
  Physical volumes use a temporary overlay unless `--disk-write` is given.

- **The Xash3D port can build and load Half-Life's server module.** `ports/xash3d/build.sh` builds HLSDK Portable
  and `xash3d-module` verifies game initialization through SavanXP's POSIX loader. [Port notes](ports/xash3d/README.md).

- **Web Viewer uses the CSS cascade** (E3). Class, ID, descendant, child and static pseudo-class selectors
  affect rendering. Linked CSS loads in the background and honors media and stylesheet order.
  [Web Viewer](docs/desktop/WEBVIEWER.md#selectors-and-origins)

- **Web Viewer opens large HTML documents** (E2). HTML is parsed through libhubbub and libdom. The old 32 KiB,
  256-paragraph and 64-link limits are gone for local files and remote pages, including links at the end of a document.
- **Win32 programs can run several threads** (WordPad WP3). `CreateThread`, `_beginthreadex`, suspend counts,
  thread notifications to DLLs, events, mutexes, semaphores, waits and a message queue per thread; guest threads take
  turns. Fixture `threads.exe`. See [WIN32.md](docs/userland/WIN32.md#threads).
- **Win32 has files, private heaps, atoms and the Spanish (Argentina) locale** (WordPad WP4). `CreateFileW` and
  searches over `C:` and `Z:`, `HeapCreate`, local and global atoms, date and time formats, `CompareStringW` and
  win.ini's defaults. Fixtures `files.exe`, `memory.exe`, `locale.exe`. See [WIN32.md](docs/userland/WIN32.md#files).
- **Win32 has hooks, window positions, carets, scroll bars, Edit, ListBox and ComboBox, and the clipboard**
  (WordPad WP5). The clipboard is SavanXP's (`CF_UNICODETEXT`, RTF, `CF_HDROP`); `CF_OEMTEXT` is code page 437.
  Fixtures `hooks.exe`, `controls.exe`, `clip.exe`; `smoke win32-controls`. See [WIN32.md](docs/userland/WIN32.md#hooks-and-subclassing).
- **SUN has typed clipboard calls (SDK 2.0).** `sxn_clipboard_open`, `_info`, `_set`, `_get`, `_format` in `sxn/sxnclip.h`;
  `winhost` is the consumer. [CLIPBOARD.md](docs/desktop/CLIPBOARD.md#how-each-subsystem-reaches-it).
- **Drag and drop between programs (SDK 2.0).** Files drags an entry onto Notepad, which opens it, or onto a
  Win32 window with `WS_EX_ACCEPTFILES` (`WM_DROPFILES`). `/dev/dragdata`, `sxnwm_drag_start`, `gfx_drag_start`,
  toolkit `on_drop`; `smoke win32-drop`. [DRAG_AND_DROP.md](docs/desktop/DRAG_AND_DROP.md).
- **SUN seeks, sizes and queries open files, and removes directories.** `sxn_seek_file`, `sxn_set_file_size`,
  `sxn_query_file`, `sxn_delete_directory`, `SXN_FILE_WRITE` and `SXN_CREATE_READ`; `winhost` is the consumer.
  [Slice 31](docs/kernel/KERNEL_NATIVE_API.md#slice-31-positions-sizes-and-directories).
- **Win32 handles x86 software exceptions** (WordPad WP2). `RaiseException` checks SafeSEH and supports continuing
  execution or searching older frames. `RtlUnwind` runs `__finally` blocks and C++ destructors, and `msvcrt` supports
  typed catches, `catch (...)` and rethrows. See [WIN32.md](docs/userland/WIN32.md#software-exceptions).
- **Web Viewer's engine libraries** (E1): NetSurf's MIT libparserutils, libwapcaplet, libhubbub, libdom and libcss,
  pinned and unmodified under `vendor/netsurf/`, built as static archives for SUN and the host, with the libdom/libcss
  glue in `system/webview/engine/`. Host test and scenario `webengine-test`; Perl and gperf are now build requirements
  ([WEBVIEWER_ENGINE.md](docs/desktop/WEBVIEWER_ENGINE.md)).
- **SDK:** `<string.h>` declares `strcasecmp` and `strncasecmp` under `_DEFAULT_SOURCE`, `_BSD_SOURCE` or `_GNU_SOURCE`.
- **Web Viewer downloads** (D1): an attachment or a type it does not show is saved instead: Save As with the server's
  name, a download worker beside the page's, a `.part` renamed only when whole, and a modeless File Download window with
  size, rate, Cancel and Open Folder while the page stays usable ([WEBVIEWER.md](docs/desktop/WEBVIEWER.md#downloads)).
- **Modeless windows:** `SAVANXP_WM_WINDOW_MODELESS` owned windows stay above their owner without disabling it; SUN's
  frame opens them with `sxngui_window_open` ([OWNED_WINDOWS.md](docs/desktop/OWNED_WINDOWS.md#modeless-windows)).
- **SDK:** SUN `sxn_delete_path` (`SXN_SYS_DELETE_PATH`), `DeleteFile` for files, durable when it returns.
- **Downloads in `sxnhttp`** (D1a): a request's `head` callback sees the final response before its body and may stop
  there, and its `sink` takes a body of any length piece by piece, held to the idle budget only (`SXNHTTP_ERROR_SINK`);
  `Content-Disposition` is read and `sxnhttp_file_name` makes a safe name. The progress callback gains the expected
  length (a new argument) and lengths up to 1 TiB are accepted. `native-network` and `https-smoke` check a 3 MiB blob
  byte-exact by SHA-256 ([HTTP.md](docs/net/HTTP.md#downloads-the-head-and-the-sink)).
- **Grouped icon view (SDK 2.0):** an `SXGUI_ICONVIEW` item starting with `SXGUI_GROUP_HEADER` is a group header, as
  in XP's grouped views; host test `sxgui-iconview-test`. Files' My Computer groups the user's folder ("user's
  Documents") and the disks ([FILES.md](docs/desktop/FILES.md#groups)).
- **Files opens on My Computer:** with no folder it lists the mounted volumes as drives (`Local Disk (/)`), with capacity
  and free space in Properties; Up from `/` goes there. `/boot` is hidden. House icons `drive-harddisk`, `drive-optical`
  and `drive-removable-media` in `/icons`. Scenario `filescomputer` ([FILES.md](docs/desktop/FILES.md#my-computer)).
- **SDK 2.0:** POSIX `storage_snapshot` (`SAVANXP_SYS_STORAGE_SNAPSHOT`) lists disks, partitions and volumes with where
  each is mounted and its sizes, SUN's `sxn_storage_snapshot` record for POSIX programs
  ([DISK_MANAGEMENT.md](docs/userland/DISK_MANAGEMENT.md#why-calls-not-devdiskn)).
- **HTTPS** (N5): `sxnhttp` opens `https://` over TLS 1.2/1.3 through the OpenSSL port, validating chain, name or
  address and dates, with SNI and only `http/1.1`, and distinct `SXNHTTP_ERROR_TLS_*` errors and no bypass; Web Viewer
  shows a lock in its status bar. Smoke `https-smoke` (fixture `tools/tls_fixture.py`) and `https-unseeded`
  ([HTTP.md](docs/net/HTTP.md#https)).
- **OpenSSL 3.5.3 port** (N5): `ports/openssl` builds `/lib/libcrypto.so.0.4` and `/lib/libssl.so.0.4` for SUN and
  POSIX programs alike and installs the Mozilla CA store as `/system/ssl/cert.pem`, both pinned in the provenance
  registry; smoke `openssl-sun` and `openssl-posix` ([ports/openssl/README.md](ports/openssl/README.md)).
- **SDK:** `atexit`, and libc time, sleep, randomness and identity in SUN programs: `time`, `gettimeofday`,
  `clock_gettime`, `nanosleep`, `getentropy`, `getrandom`, `getpid` and `getuid` stand on runtime hooks with native
  definitions instead of failing on `ENOSYS` ([NATIVE_HEAP.md](docs/userland/NATIVE_HEAP.md)).
- **Clock validity** (N5): SUN `sxn_clock_status` and POSIX `clock_status` say whether the date can be believed -- set,
  after the release floor (2026-11-01) and before 2100 -- whether it came from the RTC or was set, and why not
  ([TIME.md](docs/kernel/TIME.md#can-the-date-be-believed)).
- **A kernel CSPRNG** (N5): BLAKE2s pool and ChaCha20 generator seeded only from credited sources, virtio-rng (new
  driver) or RDSEED; unseeded it refuses with `EAGAIN` and logs `random: not seeded`. SUN `sxn_random_bytes` and
  `sxn_random_status`, POSIX `getrandom`, `getentropy` and `random_status`; Linux `getrandom` and `AT_RANDOM` use it.
  QEMU machines get `virtio-rng-pci` unless `--no-rng`; new host test `random-crypto-test` and smoke
  `random-unseeded` ([RANDOM.md](docs/kernel/RANDOM.md)).
- **Win32: the user's Windows XP WordPad starts up to its first window** (WP1). Under `win32-harness`, WordPad and
  its MFC load, initialize, run OLE's start-up and show their own "cannot load the RichEdit DLL" box. New host test
  `win32-wordpad-test` (`SAVANXP_WIN32_WORDPAD`), fixture `wstartup.exe`, host service ABI 6 ([WIN32.md](docs/userland/WIN32.md#wordpad)).
- **Win32 declared entry points: every load-time import of Windows XP WordPad and MFC resolves** (WP1). A
  function imported but not yet written is a line in its DLL's `declared.txt`; it reports itself once on the debug
  output and fails cleanly. New DLLs `ole32`, `comdlg32` and `shlwapi` ([WIN32.md](docs/userland/WIN32.md#declared-entry-points)).
- **Win32 (SSW) loads images with delay-load imports** (WordPad WP1): the PE parser checks the descriptors
  (`pe_for_each_delay_imported_dll`) and leaves binding to the image's own helper; descriptors with VAs are refused
  by name. `mfc42u.dll` has thirteen ([WIN32.md](docs/userland/WIN32.md#what-it-accepts)).
- **SUN HTTP loader, `sxn/sxnhttp.h` (`sxnhttp_get`)** (N4): HTTP/1.1 GET with Content-Length, chunked and
  close-delimited bodies, five relative redirects, 30 s total and 10 s idle budgets and a cancel event; contradictory
  framing, truncated bodies, oversized heads and redirects to files or from https fail explicitly. New host test
  `sxnhttp-message-test`; `native-network` checks it against `tools/http_fixture.py` ([HTTP.md](docs/net/HTTP.md)).
- **Web Viewer loads pages in the background, with Stop** (N3 complete): a network worker per window, so slow names
  and stalled downloads leave the window responsive; Stop (the Go button, Esc, Go > Stop), a newer navigation or
  closing the window cancels the load and keeps the page on screen. New SUN frame call `sxngui_app_wake` with the
  `on_wake` hook ([WEBVIEWER.md](docs/desktop/WEBVIEWER.md#loading-in-the-background-n3)).
- **SUN DNS resolver, `sxn/sxndns.h` (`sxndns_resolve`), and Web Viewer resolves through it** (N3, first half): TTL
  cache, replies believed only from the server's address and port, TCP retry for truncated replies, CNAME chains and a
  cancel event. New host test `sxndns-message-test`; `native-network` checks it against `tools/dns_fixture.py`
  ([NETWORKING.md](docs/net/NETWORKING.md#name-resolution-sxndns)).
- **Linux programs get a penguin icon:** a Linux ELF under `/system/linux/` (Xeyes, `jq`) shows it on the desktop,
  in Files and in the Start menu, and its window carries it in the title bar and the taskbar. A program's own
  icon still wins ([LINUX.md](docs/userland/LINUX.md#the-linux-program-icon)).
- **SUN file locks: `sxn_lock_file` and `sxn_unlock_file`** (`SXN_SYS_LOCK_FILE`/`UNLOCK_FILE`), `LockFileEx`'s
  model over the same kernel table as POSIX's `fcntl` locks; `nativetest` covers them.
- **POSIX record locks: `fcntl` `F_GETLK`, `F_SETLK` and `F_SETLKW` with `struct flock`.** Advisory and per
  process; `F_SETLKW` waits, `EDEADLK` refuses a cycle. Kernel table and rules in
  [docs/kernel/FILE_LOCKS.md](docs/kernel/FILE_LOCKS.md); new guest test `locktest` in `smoke smoke`.
- **TinyCC port: a C compiler on the system** (`ports/tinycc/build.sh`). `/bin/tcc hello.c -o hello` builds a static
  POSIX program in the guest, and `tcc -run` runs one from memory. New smoke `tinycc-smoke`
  ([ports/tinycc/README.md](ports/tinycc/README.md)).
- **`tcc` builds SUN programs:** an output named `<name>.sxe` links the SUN runtime and comes out with
  `EI_OSABI` native and a `.sxmeta` holding its `NAME`, as from `build-user.sh --native`. `tinycc-smoke` checks one.
- **The POSIX libc gains `realpath`, `freopen`, `strtold` and `ldexpl`, and `math.h` works without GNU builtins (SDK 2.0).**
  What a C compiler running on the system (TCC) needs from the libc; `libctest` covers them. Limits are in the
  [SDK reference](subsystems/posix/sdk/v1/REFERENCE.md).
- **The kernel reports the IOMMU at boot** (phase M1): it reads the IVRS or DMAR and logs `iommu: AMD-Vi, 1 unit, ...`,
  `iommu: VT-d, ...`, `iommu: none`, or why a malformed table was refused; nothing is enabled yet. New smokes
  `iommu-amd`, `iommu-intel`, `iommu-none` and `run_qemu.py --iommu` ([IOMMU.md](docs/kernel/IOMMU.md)).
- **AMD-Vi translation behind the `iommu` boot option** (phase M2): every device in one identity domain over memory,
  I/O page faults logged as `iommu: I/O page fault from bb:dd.f ...`; `noiommu` always wins. New `build.sh --iommu`
  and `--boot-options` (`SAVANXP_IOMMU`, `SAVANXP_BOOT_OPTIONS`), boot option `iommu-selftest` and smoke
  `iommu-amd-on` ([IOMMU.md](docs/kernel/IOMMU.md#translation-one-identity-domain-m2)).
- **Per-device IOMMU domains** (phase M3): `iommu::create_domain`, `map`, `unmap`, `attach` (whole isolation groups
  from the IVRS aliases), `block` and a blocked domain; faults reach the domain's owner. The RTL8139 runs in its own
  domain with `iommu`. New boot option `rtl8139-unmapped-tx` and smoke `iommu-rtl8139-fault` ([IOMMU.md](docs/kernel/IOMMU.md#domains-of-their-own-m3)).
- **AMD-Vi interrupt remapping** (phase M4): with `iommu`, a device or I/O APIC raises only the vectors the kernel gave
  it (`iommu::allow_interrupt`); refusals log `iommu: interrupt from bb:dd.f refused`. Boot option `nointremap` turns it off. Level-triggered I/O APIC
  lines get a directed EOI when their interrupt arrives edge-triggered ([IOMMU.md](docs/kernel/IOMMU.md#interrupt-remapping-m4)).
- **`iommu` turns on an AMD-Vi register window the firmware left off**, from the unit's PCI capability:
  `iommu: unit 0: firmware left its register window off; turned on at ...`. VirtualBox's EFI is such a firmware
  ([IOMMU.md](docs/kernel/IOMMU.md#virtualboxs-amd-iommu-as-measured-virtualbox-7222)).
- **ISO boot entry SavanXP (IOMMU on)**: boots with `iommu`, for trying AMD-Vi translation and interrupt remapping on
  real hardware without editing the entry in Limine ([BUILD_LINUX.md](docs/build/BUILD_LINUX.md)).
- **SUN programs find the person's settings**: `sxn_profile_directory` and `sxn_settings_path` (`sxn/sxnprofile.h`),
  the same rules as POSIX's `savanxp_settings_path`, over the new `sxn_query_identity` (`SXN_SYS_QUERY_IDENTITY`).
  Display Properties uses them ([USERS_AND_PERMISSIONS.md](docs/kernel/USERS_AND_PERMISSIONS.md#settings-the-users-and-the-machines)).

- **New Folder in Files and in the Open/Save As dialog.** Files has **New Folder...** in its File menu and the folder's
  context menu; the SUN file dialog has a Create New Folder button beside Up One Level. Both ask for the name, offer the
  first free "New Folder", and show the new folder selected ([FILE_DIALOG.md](docs/desktop/FILE_DIALOG.md)).
- **Savan BASIC's debugger.** Every Start runs under it: breakpoints (F9 or the gutter), Break, Step Into/Over/Out
  (F8, Shift+F8, Ctrl+Shift+F8), Locals and Watch panes, and a runtime error nothing catches stops at its line before
  the program ends ([SXBASIC_IDE.md](docs/userland/SXBASIC_IDE.md#the-debugger)). New `sxbasic --debug`, the VM's
  `struct sxb_debug_host` and `sxgui_textedit_line_at`. Checked by `sxbasic-test` and `smoke sxbide-debug-smoke`.
- **Savan BASIC's form designer.** A form opens drawn: a toolbox, controls placed by dragging, moved, resized and
  deleted on an 8-pixel grid, the property grid following the selection, and a double-click writing the default event's
  `Sub` ([SXBASIC_IDE.md](docs/userland/SXBASIC_IDE.md#the-form-designer)); F7 shows the text. SXGUI gains a design
  surface: `sxgui_paint_widgets`, `sxgui_paint_grab_handles`, `sxgui_grab_at`, `sxgui_grab_resize`,
  `sxgui_paint_snap_grid`, `sxgui_snap`. Checked by `sxgui-design-test` and `smoke sxbide-design-smoke`.
- **Savan BASIC, the SxBASIC environment.** `/system/sxbide.sxe` (All Programs > Development) opens a project, edits its
  files with line numbers and colours, sets values in a property grid on a form's text, and runs Start and Make SXE in
  a child process, opening an error at its line ([SXBASIC_IDE.md](docs/userland/SXBASIC_IDE.md)). New
  `sxgui_textedit_replace` and `sxgui_textedit_set_caret`. Checked by `smoke sxbide-smoke` and `shoot.sh --scenario sxbide`.
- **SXGUI text editor documents with undo.** `sxgui_textedit_document` gives an `SXGUI_TEXTEDIT` a buffer the toolkit owns and grows, with
  undo/redo by step (Ctrl+Z, Ctrl+Y or Ctrl+Shift+Z, `sxgui_textedit_undo`/`redo`); fixed caller buffers behave as before.
  `libsxgui` now also asks the program for `malloc`/`realloc`/`free`.
- **SXGUI text editor code options.** `SXGUI_TEXTEDIT` takes `edit_flags` (`SXGUI_TEXTEDIT_LINE_NUMBERS` gutter,
  `SXGUI_TEXTEDIT_AUTO_INDENT`), `edit_colorize` (colour by span, per visible line) and `edit_marks` (breakpoint and
  current-line marks); all off by default. Loading a document now also scrolls back to its first line.
- **SXGUI property grid.** `SXGUI_PROPGRID` (`sxgui_propgrid`, `struct sxgui_property`) shows name and value rows and edits
  them in place: text (Enter keeps, Esc reverts), true/false and list values stepped by click or Space, colours with a
  swatch; `SXGUI_ACTION_CHANGE` fires per kept value, and `sxgui_propgrid_commit` keeps an edit in progress.
- **SxBASIC runs Savan BASIC programs, console and Windows.** `/system/sxbasic.sxe <project.sbp>` compiles a
  project to bytecode and runs it, its forms as SXGUI windows with events, `ShowModal`, `MsgBox` and `InputBox`
  ([SXBASIC.md](docs/userland/SXBASIC.md)); `--selftest <script>` drives one with no window. Checked by
  `sxbasic-test`, `smoke sxbasic-smoke`, `sxbasic-greeter-smoke` and `sxbasic-converter-smoke`.
- **SxBASIC Make SXE.** `sxbasic --make <project.sbp> <program.sxe>` makes a program that runs on its own, the stub
  `sxbasic.sxe` (over the new `libsxbasic.so`) plus `.sxbasic`, `.sxmeta` and `.sxicon` sections
  ([SXBASIC_IMAGE.md](docs/userland/SXBASIC_IMAGE.md)); SUN gains `SXN_CREATE_EXECUTABLE`. Checked by `sxbasic-make-smoke`.
- **Mouse Properties sets the pointer speed: a Motion tab with a Slow–Fast slider (1–20, 10 = one to one).** The kernel
  scales relative mice (PS/2, USB) for every reader; tablets are not scaled. New `/dev/pointer` with
  `INPUT_IOC_SET/GET_POINTER_SPEED`, `sxn/sxnpointer.h` and POSIX `input_*_pointer_speed`; saved in `mouse.cfg` and applied
  by `init` at boot ([INPUT.md](docs/kernel/INPUT.md#pointer-speed)).
- **SUN makes directories and renames.** `sxn_make_directory` (0700) and `sxn_rename_path`, which replaces a file at
  the new path; both are durable when they return, so a save can be written aside and renamed over the old file.

- **Other build systems build against the SDK: `share/sdk/sxcc` and `share/sdk/savanxp-toolchain.cmake`.**
  `sxcc` is clang with the sysroot's flags, runtime and linker script (`--sx-profile=nosse|sse|pie`, `--sx-native`); the
  sysroot gains archives for `-l` (`libsavanxp.a`, `libsxgui.a`, ...). The FFmpeg port now builds through it. New example
  `sdk/cmakesmoke` and scenario `cmake-smoke`; the build now needs `llvm-ar`. See the [SDK README](subsystems/posix/sdk/v1/README.md#other-build-systems-sxcc-and-cmake).

- **`build-user.sh --pie` builds a program that maps shared libraries; `--needs libmath.so.0.4` links one.**
  The SDK sysroot gains a PIE runtime with the loader and `<dlfcn.h>`, and the system libraries (`lib/shared`). The Doom
  and ccleste ports now link those instead of compiling their own copies. New example `sdk/piesmoke` and scenario
  `pie-smoke`; which units each library replaces is in `runtime/libraries.txt`. Phase 4 of [SDK_V2_ROADMAP.md](docs/roadmaps/done/SDK_V2_ROADMAP.md).

- **An external SUN program can open an SXGUI window: `build-user.sh --native --gui` links SUN's application frame.**
  `sxn/sxngui.h` and `sxn/sxnfiledlg.h` come from the SDK sysroot, and every `--native` build gets `<name>.sunres.h`.
  The SUN runtime is listed once, in `system/runtime/units.txt`. New example `sdk/sungui`, run by `windowd-smoke`; see [NATIVE_APPS.md](docs/userland/NATIVE_APPS.md).

- **The Media Player plays AV1, through dav1d 1.5.1 (BSD-2-Clause) linked into `libffmpeg.so.0.4`.** 8- and 10-bit.
  The FFmpeg port now needs Meson and `pkg-config` (new `ports/ffmpeg/dav1d.sh`); POSIX gains `ENOPROTOOPT`.
  Release ISOs carry the FFmpeg port; see [MEDIA_PLAYER.md](docs/media/MEDIA_PLAYER.md#av1).

- **The Media Player names a codec it cannot decode instead of silently dropping the stream.** An AV1 WebM plays its
  sound and says `av1 video is not supported`; a file with nothing playable is refused naming the codecs. New
  `mediaplayer --selftest --unsupported <codec>`; see [MEDIA_PLAYER.md](docs/media/MEDIA_PLAYER.md#what-it-does-with-a-codec-it-cannot-decode).

- **Shut Down and Restart end the session first, as Windows does.** `init` serves a `shutdown` port
  (`power_shutdown()`/`power_reboot()`, SUN `sxn_request_shutdown()`); `sessiond` and `windowd` ask every window to close,
  a Cancel to "save changes?" cancels the shutdown, and `init` stops its services before `/dev/power`.
  Programs answer with `on_close` and `gfx_decline_close`/`sxnwm_decline_close`; new errno `ECANCELED`.
  `session-end-smoke` and `shoot.sh --scenario notepadclose` cover it; see [POWER.md](docs/kernel/POWER.md#ending-the-session).

- **At shutdown, `init` asks its services to stop instead of killing them.** `audiod` and `elevationd` get
  `SAVANXP_SERVICE_CONTROL_SHUTDOWN` on their control channel (`--service`, stdin) and five seconds; `audiod` saves a
  volume changed just before. See [POWER.md](docs/kernel/POWER.md#services).

- **Power-off and restart show a "SavanXP is shutting down..." (or "restarting...") screen.**
  When every power-off method fails, it becomes "It is now safe to turn off your computer." and the CPU halts.
  See [POWER.md](docs/kernel/POWER.md#the-screen).

- **The power button ends the session through `init`, as Shut Down does; an application can still cancel it.**
  `init` watches it with `POWER_IOC_WATCH_BUTTON` on `/dev/power`. With no watcher, or a press still unread, the kernel
  powers off at once. `power-button-smoke` covers it; see [POWER.md](docs/kernel/POWER.md#the-power-button).

- **`power-smoke` scenario: the power button with unflushed writes leaves the volume clean, and the data reads back after the next boot.**
  `nativetest power` is the guest half; QMP callbacks now get the run's disk copy (`SAVANXP_SMOKE_DISK_IMAGE`).

- **The boot framebuffer descriptor now carries Limine's memory model and channel masks, with a checked validator.**
  `framebuffer::validate` and host test `framebuffer-layout-test`; nothing calls it yet, output is unchanged.
  It accepts 32-bit BGR and names it so callers can warn. FB1a of [FRAMEBUFFER_ROADMAP.md](docs/roadmaps/open/FRAMEBUFFER_ROADMAP.md).

### Changed

- **Win32 programs run in English (United States), LCID 0x0409, as the rest of the system.** `GetLocaleInfoW`, the
  date and time formats and `CompareStringW` give an English Windows XP's answers; ñ sorts as n with a tilde.
  [The locale](docs/userland/WIN32.md#the-locale).

- **Window titles hold 127 characters instead of 31.** `sxnwm_set_title`, `gfx_set_title` and the `.sxe` name go
  whole through a new `long_title` tail of the surface header; owned windows keep 63. A caption narrower than its
  title ends in "...". Device Manager's sheets are always "`<device>` Properties".

- **`build.sh run` and `debug` can request `sudo` to open an SxFS device.** QEMU still runs with the invoking user's
  permissions.

- **Port installers can request `sudo` for a physical SxFS volume.** Builds, validation and recovery files stay under
  the invoking user's permissions.

- **`prepare-disk` can request `sudo` for a physical disk.** The build and staging run with the user's permissions.

- **Web Viewer** is built only when the OpenSSL port is (`ports/openssl/build.sh` before `./build.sh build`).
- **Web Viewer loads through `sxnhttp` (N4 complete):** HTTP/1.1 with chunked bodies; contradictory framing and
  truncated bodies fail with their message and keep the page; `text/plain` shows as text; a non-200 status is
  noted in the status bar; redirects to `file://` are refused and `https://` says it is not available yet.
- **Web Viewer's window, after Internet Explorer 5:** a toolbar of icons (Back, Forward, Stop, Refresh, Home, Open,
  Source), an `Address` band, a progress pane in the status bar, a context menu on the page, the page's title in the
  window title, and a hovered link's address in the status bar. Stop moves from the Go button to the toolbar and
  View > Stop ([WEBVIEWER.md](docs/desktop/WEBVIEWER.md#the-window)).
- **A full TCP socket table reclaims the `TIME_WAIT` slot closest to expiry** instead of returning `ENOMEM` after
  rapid closes; closings that still owe data or a FIN are kept. `tcp-smoke` checks 40 active closes in a row
  ([NETWORKING.md](docs/net/NETWORKING.md#closing-keeps-protocol-state-alive)).
- **A thread the timer tick wakes runs at once**: on a free idle core (`smp::kick_free_core`), or in place of the
  BSP's current thread, instead of after the rest of its 20 ms slice; `audio-preferences` no longer underruns while
  an AP waits for the disk ([SMP_ROADMAP.md](docs/roadmaps/open/SMP_ROADMAP.md), slice 5.2).
- **Files are read in pieces of up to 64 KiB** instead of 256 bytes, so SxFS asks the disk for whole runs rather
  than one sector at a time: a 4 MB `cp` takes under a second under KVM and holds the kernel lock 13-29% of its window
  instead of 43-53% ([SMP_ROADMAP.md](docs/roadmaps/open/SMP_ROADMAP.md), slice 5.2).
- **Disk writeback waits outside the kernel lock** on an SMP machine (slice 5.2, first cut): one application
  processor's idle at a time sends a writeback command with the lock released, so the other cores no longer stop for
  the disk; the BSP leaves the queue to the APs ([BLOCK_WRITEBACK.md](docs/kernel/BLOCK_WRITEBACK.md#draining)).
- **Turning AMD-Vi on queues its invalidations** instead of handing each to the unit: for an IVHD that covers every
  bus, 336 ms drop to 35 ms under QEMU (the Ryzen 5 1600 spent 330 ms there)
  ([IOMMU.md](docs/kernel/IOMMU.md#translation-one-identity-domain-m2)).
- **Display Properties' Browse... opens the common Open dialog** instead of
  asking for a typed path ([DISPLAY_PROPERTIES.md](docs/desktop/DISPLAY_PROPERTIES.md)).

- **Display Properties is a SUN program**, `/system/display.sxe` from `system/display`; the POSIX `/bin/display` is
  retired from the image. It looks and behaves as before. SUN gains `sxnwm_set_display_mode` for its resolution trial
  ([DISPLAY_PROPERTIES.md](docs/desktop/DISPLAY_PROPERTIES.md)).
- **The shipped files left the root of the disk.** Wallpapers are in `/system/wallpapers`, `mimeicon.ini` in `/system`,
  the Web Viewer pages and `system.mp3` in `/system/samples` and Paint's fixture in `/system/selftests`; a saved
  `/wallpapers/...` choice still loads, and the build retires the old copies. `/README` now describes the system and
  its folders ([DISK_LAYOUT.md](docs/fs/DISK_LAYOUT.md)).
- **The Start menu stays open behind a pinned shortcut's context menu,** as Windows XP's does: Esc or a press on the
  menu closes only the context menu, and Unpin from Start Menu leaves the menu open without the row
  ([START_MENU.md](docs/desktop/START_MENU.md#shortcuts)).
- **The kernel log is in English.** The `smp:`, `uacpi:`, `timer:`, `ioapic:`, `ac97:`, `fb_gpu:`, `cpu:`, `acpi:` and
  `process:` lines that were still in Spanish are translated, as are `polltest` and `gputest`. Renamed fields:
  `ac97-stats` `periods`/`drops`/`max_gap`, `timer-stats` `lost`, `fb_gpu: ... double buffer yes|no`.
- **The SDK is 2.0 (`SAVANXP_SDK_VERSION_MAJOR 2`, `MINOR 0`), recording breaks already shipped since 1.2.**
  The breaks: the `savanxp_` prefix on raw calls, no `/dev/fb0`, and `gfx_open` needing the compositor. Its README
  and REFERENCE are now in English, with a versioning policy. See [SDK_V2_ROADMAP.md](docs/roadmaps/done/SDK_V2_ROADMAP.md).

- **`tools/build-user.sh` no longer puts `subsystems/posix/userland` or `build/generated` on an external program's include path.**
  Neither was ever part of the SDK, so the SDK stays 2.0; a program that included an in-tree userland header stops
  building. Every `sdk/` example still builds to the same binary. Phase 1 of [SDK_V2_ROADMAP.md](docs/roadmaps/done/SDK_V2_ROADMAP.md).

- **The SDK is a sysroot, `build/sdk/2.0`, that `./build.sh build` assembles with the new `tools/make-sdk.sh`.**
  It holds the public headers (allowlist: `public-headers.txt`), the runtime prebuilt with and without SSE, and its own
  `share/sdk/build-user.sh`, so a copy builds programs away from the repository. `tools/build-user.sh` builds against it,
  to the same binaries as before. `--heap-mib` links a strong bootstrap arena instead of recompiling `posix.c`. New host
  test `sdk-sysroot-test`. Phase 2 of [SDK_V2_ROADMAP.md](docs/roadmaps/done/SDK_V2_ROADMAP.md); layout in the
  [SDK README](subsystems/posix/sdk/v1/README.md#the-sysroot).

- **The tick no longer reads the network card or the PS/2 controller every millisecond when their interrupts work.**
  It checks them every 64 ticks and goes back to polling, with a `nic:` or `ps2:` log line, if those interrupts never
  arrive. Under KVM the tick holds the kernel lock about a third as long. See [NETWORKING.md](docs/net/NETWORKING.md#receive-interrupts-and-the-tick).

- **Serial output is queued and fed to the UART from the tick, so a kernel print no longer waits for the port.**
  The stats reports stop being the longest kernel lock holds of an idle desktop. A panic, a fatal exception and
  power-off still write synchronously. Slice 5.1 of [SMP_ROADMAP.md](docs/roadmaps/open/SMP_ROADMAP.md); see [SYSTEM_LOG.md](docs/kernel/SYSTEM_LOG.md#the-serial-port).

- **The boot framebuffer is checked once before anything draws; the boot log gains two `framebuffer:` lines.**
  A malformed descriptor now leaves serial output only instead of being drawn into; a BGR one boots with a warning.
  DISPI mode changes pass the same check. FB1b of [FRAMEBUFFER_ROADMAP.md](docs/roadmaps/open/FRAMEBUFFER_ROADMAP.md).

### Fixed

- **Device Manager no longer lists QEMU's virtio sound card as a failed `ac97`.**
  The AC'97 driver matched it by class code and took it over; it now also needs
  AC'97's two I/O BARs.

- **`shoot.sh --scenario startmenu` passes on a gradient wallpaper.** It no longer
  mistakes a Programs cascade that fits the screen for one with scroll arrows.

- **The POSIX loader applies nonzero `R_X86_64_64` addends (SDK 2.0).**
  Shared libraries such as the Xash3D HLSDK client can use C++ virtual tables.

- **`build.sh smoke --smoke-command` no longer aborts on an unset variable, and a command named `false`
  or `off` is staged.** CMake read such a command as an empty one and staged no smoke specification.

- **GitHub Actions installs `gperf` for the NetSurf build.**
- **`malloc` walked every block on each call:** the C runtime's allocator now keeps a list of free blocks, so a heap
  of many small allocations stays fast; styling a 4000-element DOM in the guest went from 100 s to under a second
  ([NATIVE_HEAP.md](docs/userland/NATIVE_HEAP.md#what-there-is)).
- **SXGUI:** a sunken label (a status bar pane) clips its text to its border instead of drawing into the next pane.
- **SDK:** `gettimeofday` and `clock_gettime(CLOCK_REALTIME)` return the date, not the time since boot.
- **A SUN program's `.sxmeta` says `SUBSYSTEM` native without a `.sxres` saying so** (`sessiond.sxe` and
  `build-user.sh --native` builds carried `posix`). A `.sxres` `subsystem=` that contradicts the build now fails it.
  New host test `sxe-resources-test`.
- **A forked child no longer loses its private memory** (heap arenas, `MAP_PRIVATE`): the child's copy of a private
  section was freed at `fork` while still mapped, so its pages could be handed to anything else
  ([KERNEL_SECURITY.md](docs/kernel/KERNEL_SECURITY.md#found-while-porting-tinycc-2026-11-21), F1).
- **A new thread's kernel stack starts cleared** instead of holding its frames' previous contents. Under QEMU's TCG a
  stack on a frame that once held code slowed the whole guest about a hundredfold, enough for `smoke smoke` to time
  out ([KERNEL_SECURITY.md](docs/kernel/KERNEL_SECURITY.md), F2).
- **A file a POSIX program creates with `fopen` gets 0666 less the umask.** It got a random mode, often 000, so Doom's
  config and saves, `mines.ini` and Files' settings could not be read back. Files already damaged keep their mode.
- **`help` lists every installed command.** `cp`, `date`, `volume`, `ntpsync`, the desktop programs and the `/system`
  tools were missing, and test apps showed in builds without them. `shell-help-test` fails when a registered program
  is left out ([CONSOLE.md](docs/desktop/CONSOLE.md#help)).
- `tools/verify_doom_persistence.sh` (CI) copies every shared library in the SDK's `libraries.txt` into its isolated tree, not just
  `libsxmidi`, so `make-sdk.sh` no longer stops with "libmath.so.0.4 is not built yet".
- POSIX `mkdir` of a directory that already exists fails with `EEXIST` instead of succeeding.

- **A program can include both `<sys/time.h>` and `<sys/select.h>`.** Each defined `struct timeval`, so including the two
  failed to compile; `<sys/time.h>` now takes it from `<sys/select.h>`, as POSIX does. `sdk-sysroot-test` now also compiles
  every SDK header together.

- **A fresh clone builds again: `tools/fat_image.c`, the source of `build/tools/fat-image`, is in the repository.**
  `.gitignore`'s `tools/*` had kept it out since the commit that added it, and every build compiles it.

- **FFmpeg's license notice says where its source is: the `ffmpeg-7.1.1.tar.xz` asset of the release.** `SOURCE.md`
  described a static binary; it now covers the shared library and how to replace it. See
  [RELEASING.md](docs/build/RELEASING.md#ffmpegs-source).

- **A kernel panic is visible on screen even when the desktop was showing its second DISPI buffer or another mode.**
  New `panic-display-front`/`-back`/`-single`/`-released` scenarios (`panictest --display`) check the screenshot.
  FB4 of [FRAMEBUFFER_ROADMAP.md](docs/roadmaps/open/FRAMEBUFFER_ROADMAP.md).

- **Closing Notepad with unsaved changes (X, Alt+F4) asks "save changes?" instead of discarding them.**

- **Power-off and reboot sync and mark every volume clean first, and the ACPI power button no longer drops queued writes.**
  Both go through `power::shutdown()`/`power::reboot()`; failures are logged (`power:`) instead of ignored.
  See [POWER.md](docs/kernel/POWER.md).

## Previous versions

- [0.4.3](docs/changelog/v0.4.3.md) — 2026-11-17
- [0.4.2](docs/changelog/v0.4.2.md) — 2026-11-06
- [0.4.1](docs/changelog/v0.4.1.md) — 2026-10-23
- [0.4.0](docs/changelog/v0.4.0.md) — 2026-10-08
- [0.3.4](docs/changelog/v0.3.4.md) — 2026-08-28
- [0.3.3](docs/changelog/v0.3.3.md) — 2026-07-09
- [0.3.2](docs/changelog/v0.3.2.md) — 2026-07-03
- [0.3.1](docs/changelog/v0.3.1.md) — 2026-07-02
- [0.3.0](docs/changelog/v0.3.0.md) — 2026-06-18
- [0.2.2](docs/changelog/v0.2.2.md) — 2026-04-01
- [0.2.1](docs/changelog/v0.2.1.md) — 2026-03-30
- [0.2.0](docs/changelog/v0.2.0.md) — 2026-03-22
- [0.1.4](docs/changelog/v0.1.4.md) — 2026-03-19
- [0.1.3](docs/changelog/v0.1.3.md) — 2026-03-17
- [0.1.2](docs/changelog/v0.1.2.md) — 2026-03-14
- [0.1.1](docs/changelog/v0.1.1.md) — 2026-03-10
- [0.1.0](docs/changelog/v0.1.0.md) — 2026-03-08
