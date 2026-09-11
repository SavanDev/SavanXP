# Changelog

Record of user-visible changes per version for SavanXP.

Cut-off notes:

- `v0.1.0` was reconstructed retroactively from the history up to `d822857`.
- `v0.1.1` covers the changes after `v0.1.0`, including work already merged into
  the tree but not yet tagged in git.

## [Unreleased]

### Added

- **The compositor measures itself.** `windowd` reports compose, sync and
  present times — present split into `ipc`/`svc`/`gpu`/`tl`, and `blk_us` for
  the total time blocked on `compositord` — plus damage area, over the new
  write-only `/dev/serial`. New `monotonic_ns()`; compositor protocol v2.
  [How to read it](docs/GRAPHICS_PERF.md).

- **The application processors boot and park.** `smp::` starts the cores the
  bootloader reports, proves the ICR delivers with a ping IPI, and says so at
  boot (`smp: N cores reportados, M en linea`). Nothing schedules on them yet;
  `build.ps1 -Smp <n>` exercises that path. Phase 0 of `docs/SMP_ROADMAP.md`.

- **Add or Remove Programs (`/bin/appwiz`), in the System group.** It lists what
  was installed outside the system image — in `/disk/bin` and not in `/bin` —
  and deletes the binary, optionally with the data directory the program
  declares as `data_dir=` in its `.sxres`. New `build.ps1 appwiz-smoke`.
  [Why that is the rule](docs/SXE_FORMAT.md#uninstalling-the-other-half-of-the-same-idea).

- **Program Manager lists every installed program on its own.** A binary whose
  `.sxres` declares `category=` shows up under that group with nothing to
  register: installing is copying it to `/disk/bin`, uninstalling is deleting
  it. Without the key it stays launchable, just unlisted. `F5` (File >
  Actualizar) rebuilds the catalog, and launching an entry that is gone says so
  instead of nothing. [How it works](docs/SXE_FORMAT.md#all-programs-the-catalog-discovers-itself).

- **`virtio-net` driver for the NIC.** With `-Virtio`, `build.ps1` builds
  `virtio-net-pci` instead of `rtl8139`, and `nic::` prefers
  `virtio_net::driver()`. Polling-only, no IRQ of its own.

- **`-Accel kvm`.** `run`/`debug` accelerate against `/dev/kvm` with `-cpu
  host` on Linux. The smokes stay on TCG on purpose, to remain deterministic.

- **Audio capture (`virtio-sound` RX) and duplex `/dev/audio0`.** `read()` has
  its own session owner, independent of `write()`, so recording and playback can
  come from different processes. New `audiotest --record` and `virtio-record`.

- **Keyboard over `virtio-input`.** `-Virtio` adds `virtio-keyboard-pci`; its
  `EV_KEY` events become scancode set 1 (`ps2::inject_scancode`) and reuse the
  existing layout logic. `ps2::` disables its own keyboard while it is active.

- **`virtio-blk` driver for the persistent disk.** With `-Virtio` the machine
  gets `virtio-blk-pci` instead of IDE, appearing in `block:` as `vblk0`. It
  flushes after every write, or a "successful" write could stay in QEMU's cache.

- **The file list shows an icon per type.** The catalog is standalone `.sxicon`
  blobs in `/disk/icons`, mapped by extension in `diskfs/mimeicon.ini`. **This
  is extension to icon, not type detection: `MIME_OPEN` is still unresolved.**

- **Video player on FFmpeg: MJPEG decoding, YUV->RGB through swscale, on
  screen.** `build.ps1 ffmpeg-smoke` runs `wavinfo`, the player in `--selftest`
  and the player in `--hold`, which presents over `/dev/gpu0`.

- **FFmpeg runs inside SavanXP.** libavutil, libavcodec, libavformat and
  libswresample compile against this libc and really decode. Minimal codec set
  (WAV + PCM s16le), no asm and no threads; built separately with GNU make, see
  `sdk/ffmpeg/README.md`.

- **`static_assert` in `<assert.h>`, the `PRI*`/`SCN*` in `<inttypes.h>`, and
  `fpclassify`.** Plus the missing errnos (`EDOM`, `EILSEQ`, `EPERM`, `ESPIPE`),
  `mkdir`, `F_SETFD`/`FD_CLOEXEC` and `imaxabs`/`strtoimax`/`strtoumax`.

- **`build.ps1 smoke` runs `imagetest`.** It inspects itself to verify what a
  streaming loader can break: `.rodata` byte by byte, `.bss` zeroed and `.data`
  with its initial values, from `/bin` and from `/disk/bin`.

- **`build.ps1 smoke` runs `stacktest`.** Deep recursion in the process and in a
  child, an overflow that must die with a `#PF` on the guard page, and a
  40-argument `argv`.

- **The libc gains what a port takes for granted.** Strings, integers, aligned
  memory, stdio (`fgetc`, `ungetc`, `fdopen`, `setvbuf`, `fseeko`, `perror`),
  `sscanf` with scansets, and a full calendar. Everything is UTC.

- **Real `%f`, `%e` and `%g`, and `strtod`.** `%f` was a stub that consumed the
  argument and wrote `0`. Now with precision, width, rounding carry and the odd
  cases (`nan`, `inf`, signed zero). All behind `__SSE2__`, like libm.

- **libm: the missing ones.** `asin`, `acos`, `hypot`, `cbrt`, `rint`, `lrint`,
  `lround`, `frexp`, `modf`, `scalbn`, `nextafter`, `fdim`, `fma`, `log1p`,
  `expm1`, the hyperbolics, `remainder` and their `float` variants.

- **`build.ps1 smoke` runs `libctest`.** Exercises the libc surface a port
  consumes: standard names as struct fields, `qsort`/`bsearch`, streams,
  directories and the POSIX error convention (`-1` plus `errno`).

- **ES/EN keyboard layout selector, in the taskbar.** US QWERTY joins the baked
  ES map, chosen at runtime through `/dev/input0`
  (`INPUT_IOC_SET_LAYOUT`/`GET_LAYOUT`), saved in `/disk/keyboard.cfg` and
  applied at boot, before windowd. UI in `kbdlayoutpopup.c`.

- **`build.ps1 kbd-smoke`: the keyboard driver is tested on its own.** It closes
  the real PS/2 -> IRQ -> `kernel/ps2.cpp` -> `/dev/input0` path, which the
  harnesses that inject already-formed events do not cover.

- **SXE stamping is no longer opt-in: everything built for the system comes out
  in that format.** Coverage over the complete image is 68/68 installed
  binaries, in-tree and external (Doom, busybox) alike. Every program gets a
  `.sxmeta` with or without a `.sxres` — name, version, subsystem and the short
  commit as `BUILD_ID`; icon, accent, description and mimes are never invented
  and stay `.sxres` enrichment.

- **`icon_file=` in the manifest: the icon travels in the binary, not in
  `assets/`.** It takes a PNG relative to the `.sxres` and derives the two sizes
  the runtime needs. Mutually exclusive with `icon=`.

- **Floating point in userland, with `-Sse` and a libm of our own.** Code using
  `float`/`double` compiled but did NOT link: under `-mno-sse` clang resolves
  each operation through compiler-rt soft-float helpers this system does not
  have. The switch is opt-in, and `math.h` declares the library under `#if
  defined(__SSE2__)` so the error appears at COMPILE time naming the function.
  New `build.ps1 float-smoke`.

- **Tab control in sxgui (`sxgui_tabs`).** The active tab is drawn taller and
  covers the page's top border, so both parts read as one sheet. Program
  Manager's group selector now uses it.

- **Taskbar at the bottom of the screen, listing open windows.** It is a WM
  CLIENT (`/bin/taskbar`), not windowd chrome — the explorer.exe model. No start
  menu and no notification area. Buttons keep a STABLE order by slot, not by
  z-order, or the next click would land on something else. What a client cannot
  know arrives over the new channels in `savanxp/wm_shell_protocol.h`.

- **`tools/shoot.ps1`: visual verification of the session, headless.** It boots
  without a window, sends keys over QMP (holding modifiers, which the monitor's
  `sendkey` cannot) and captures PNGs. The `taskbar` scenario verifies pixels
  and is wired as `build.ps1 taskbar-smoke`. Not part of the build.

- **Alt+Tab to switch windows.** While Alt is held each Tab moves the selection
  and releasing it confirms; Alt+Shift+Tab goes backwards. The switcher shown is
  the Task List: no new UI.

- **Text selection in sxgui, with Cut, Copy and Paste.** Shift with the arrows,
  Home, End and the page keys extends the selection; click anchors, drag
  stretches. Ctrl+C, Ctrl+X, Ctrl+V and Ctrl+A; other Ctrl+letter combinations
  are consumed instead of typed, except with AltGr. Also exposed as
  `sxgui_textedit_copy`, `_cut`, `_paste`, `_select_all` and `_has_selection`;
  Notepad gains its Edit menu.

- **System clipboard, in `/dev/clipboard`.** The content is a value, not a
  stream: a `write` replaces everything and a `read` returns from the start.
  Over `SAVANXP_CLIPBOARD_CAPACITY` (8 KiB) it fails with `ENOSPC`. SDK wrappers
  `clipboard_set_text`, `_get_text`, `_get_info` and `_clear`.

### Changed

- **The compositor accumulates damage as an exact region.** `windowd` stopped
  merging dirty rectangles by bounding box: dragging a window now repaints and
  presents the ring that changed, not the box around the old and new frames.

- **Launcher icon captions wrap to two lines.** A name that does not fit on one
  line breaks at a space and is centred over two, and only what still does not
  fit is cut with an ellipsis — before, a long caption was clipped at *both*
  ends and read as broken rather than as truncated. The cell now derives its
  height from the active font instead of a baked 76 pixels.

- **The boot screen is now a splash with the project logo.** Black background,
  `assets/brand/logo.png` baked in by `tools/gen_boot_logo.py`, the system name
  below it, a sliding block bar, and the step names now in English.

- **All repository documentation is in English, and the README leads with a
  quickstart.** The Linux requirements move to `docs/BUILD_LINUX.md`,
  `docs/README.md` indexes the design docs, and `CLAUDE.md` points at
  `AGENTS.md`.

- **The filesystem is now called `SxFS`, and the `2` leaves the name.** The
  constants and the namespace never carried the number (`SVFS_*`, `svfs::`), so
  the name said one version and the code another. The on-disk magic goes from
  `SVFS2` to `SXFS` (the journal's from `SVJNL2` to `SXJNL`) and `SXFS_VERSION`
  returns to `1`. **Every earlier image stops mounting**: `build/disk.img`
  regenerates itself, but a persistent VirtualBox/QEMU disk must be reformatted.

- **`assert()` reports when it fails.** It was `((void)(expression))`, so a
  failing assert went unnoticed. It now prints and terminates the process; with
  `NDEBUG` it still disappears.

- **`struct stat` has the fields third-party code names.** `st_uid`, `st_gid`,
  `st_nlink`, `st_blksize`, `st_blocks`, `st_atime`, `st_mtime`, `st_ctime`. The
  kernel only reports type and size, so the rest stay zero.

- **`exec` no longer copies the whole image into physically contiguous pages.**
  `elf::load_user_image` takes an `elf::ImageReader` and reads the file directly
  onto the process's pages: half the memory during exec, and no contiguous
  reservation that fails on a fragmented heap.

- **The user stack goes from a fixed 128 KiB to 1 MiB on demand, with a guard
  page.** Only 8 pages are mapped at startup and the rest appear when touched
  (`process::grow_user_stack`), so a shallow process uses LESS memory than
  before. An overflow now dies with a readable fault.

- **`spawn`/`exec` accept 128 arguments and 8 KiB total, instead of 15
  arguments of 63 characters.** The buffer now comes from the kernel heap
  instead of 4 pages of kernel stack, and overflow fails with `E2BIG` (new)
  instead of truncating silently.

- **Streams read in batches and `malloc` aligns to 16.** `fgets` was issuing one
  `read()` per character; file streams now fill a 512-byte buffer. 16 is
  x86-64's `max_align_t`: with 8, a `movaps` in an `-Sse` app could fault.

- **The libc stops renaming with `#define`: the standard names are now real
  symbols.** The SDK headers defined `#define read sx_read` and ~140 more, so
  the preprocessor rewrote any identifier with those names — a `.close` struct
  field included — and `malloc` or `stat` were not symbols a third-party object
  could link. The raw syscall layer is prefixed `savanxp_*`; `math.h` and
  `setjmp.h` keep their macros on purpose.

- **One userland runtime and one `printf`.** `runtime/posix.c` is linked into
  every program and `runtime/libc.c` is reduced to the raw syscall layer plus
  gfx. Which formatter ran used to depend on the headers each `.c` included, and
  libc.c's did not even understand `%ld`/`%lu`. Raw `puts` becomes `puts_out`
  and `putchar(fd, c)` becomes `putchar_fd`.

- **`icon=` in `progman.ini` points at a program, not at an art catalog.** It
  stores a PATH and the icon is read from the binary it points at. The old names
  still work as aliases, and an explicit `icon=` beats the binary.

- **The sxgui chrome moves to the Win95 two-pixel bevel.** A period 3D border
  takes four tones — `SXGUI_COLOR_BEVEL` (223,223,223) was missing — and sunken
  is now the exact reverse of raised. Also dotted focus rectangle, 50% scrollbar
  trough, redrawn checkbox tick, concentric radio button, engraved disabled
  text.

- **The sxgui apps share a single layout grid.** The metrics live in
  `savanxp/sxgui.h` (`SXGUI_MARGIN`, `SXGUI_GAP`, `SXGUI_BUTTON_WIDTH`, ...) and
  are used by notepad, files, progman, aboutapp and widgetsdemo.

- **`tools/shoot.ps1` gains the `files` scenario.** The explorer is the window
  with the most distinct controls at once, so it is where a stray toolkit margin
  shows up.

- **The keyboard event carries the modifiers.** `savanxp_input_event` gains
  `modifiers` (`SAVANXP_KEY_MOD_*`); the kernel already computed it and threw it
  away, so the WM tracked Ctrl by hand and got stuck if a KEY_UP was lost. The
  event grows from 12 to 16 bytes: **external apps built against the previous
  SDK need a rebuild** — Doom reads this struct.

### Removed

- **The icon set baked into `desktop_icons.h` is down to one.** Only
  `DESKTOP_ICON_DESKTOP` survives, as the safety net for a binary that cannot be
  read at all. The PNGs in `assets/desktop/icons/` stay as the catalog each
  `.sxres` references with `icon=<name>`.

- **The start menu strip art is deleted.** The menu was retired with the rest of
  the Win95 chrome, but the chain that drew it was still whole, down to a symbol
  nobody used.

- **The eight hand-written coreutils in `subsystems/posix/userland` are
  deleted.** `cat.c`, `echo.c`, `ls.c`, `mv.c`, `rm.c`, `sleep.c`, `true.c` and
  `false.c` were built by nothing: `/bin` gets them from the busybox multicall.

### Fixed

- **The taskbar drew the generic icon for every window.** The shell window list
  only carried a baked-in icon id, and that set is now just the generic one, so
  each button gets the window's own 16x16 `.sxicon` instead.

- **The ELF loader failed on two `PT_LOAD`s sharing a page.**
  `map_segment_pages` treated an already-mapped page as an error and the load
  died reporting "out of memory". The shared page now keeps the union of the
  permissions.

- **`argc` could exceed the arguments that existed.** The kernel copied at most
  15 arguments but handed the process the original `argc`, so a longer `argv`
  made the program read pointers that were never written.

- **`time()` returned the uptime, not the Unix epoch.** It computed
  `uptime_ms() / 1000` with the RTC right there, so any derived date came out as
  1970.

- **`strtoul` and friends wrapped silently on overflow.** A number larger than
  `ULONG_MAX` returned garbage; it now saturates and sets `ERANGE`.

- **`waitpid()` was infinite recursion.** The variadic macro in `<sys/wait.h>`
  expanded back into itself. Nothing in the tree reached it, but any port
  calling POSIX `waitpid` ate the 128 KiB stack with no net.

- **A read-only SxFS volume became writable again once published.**
  `sxfs::attach()` set the status to `mounted` without looking at how the mount
  had ended, so a volume whose journal could not be recovered accepted writes.
  New `build.ps1 sxfs-smoke` mounts a broken volume from a host test.

- **Two windowd bugs uncovered by the keyboard selector popup.** The click-down
  landed on the overlay window path, and the popup never entered the
  composed/retire signalling lists, so its second `gfx_present()` hung forever.

- **The SXE resource generator blurred an icon when enlarging it.**
  `collect_icons_from_file()` only detected the integer multiple when shrinking,
  so a 16x16 original derived its 32x32 with LANCZOS instead of NEAREST.

- **The Type column in files was clipped when the scrollbar appeared.** The
  width split subtracted a fixed 6 pixels instead of the two bevels plus the
  bar's 16. The bar is now always reserved, so columns no longer jump width.

- **Row text did not line up with its column label.** The header cell's bevel
  eats two pixels the row was not subtracting.

- **In widgetsdemo the free-standing scrollbar overlapped the second column.**
  The column origin was computed from the list width without counting the bar
  between the two.

- **Pressing Ctrl dropped the selection, so Ctrl+C copied nothing.** A modifier
  key produces its own event with `ascii` at zero, and the text widgets treat
  "any other key" as a reason to drop the selection. Modifiers no longer reach
  the widgets.

## [0.3.4] - 2026-08-28

### Added

- **The userland malloc no longer lives in a fixed BSS arena: it grows by asking
  the kernel for sections.** The single BSS arena was resident physical RAM per
  process from exec onwards even if the app never touched a byte. A 256 KiB
  bootstrap stays in the BSS and the rest comes from `section_create` +
  `map_view`. External apps stop forcing `-DSX_HEAP_SIZE`. **A normal `malloc`
  can now return addresses above 4 GiB.**

- **The whole VRAM aperture is mapped write-combining.** The firmware maps only
  the visible mode (4000 KiB of 16 MiB), and that was the ceiling for everything.
  `fb_gpu` now asks dispi how much VRAM there is and maps the complete aperture,
  choosing the WC index from `IA32_PAT` instead of assuming the layout. New in
  `vm::`: `kPagePat`, `map_kernel_device_memory`, `kernel_page_cache_flags`,
  `write_combining_page_flags`.

- **Double buffering by panning on the flat framebuffer (no tearing).** With a
  virtual height twice the visible one, dispi's `Y_OFFSET` picks which frame is
  shown; the compositor always composes onto the one not on screen. Every present
  flips, partial damage included, by reapplying the previous frame's damage on
  top of the current one. `boot::FramebufferInfo` gains `mapped_bytes`, which
  decides whether both buffers fit.

- **The flat framebuffer backend can change modes (Bochs VBE).** It detects the
  dispi interface (ports 0x1CE/0x1CF, implemented by QEMU's standard VGA and
  VBoxVGA) and advertises `MUTABLE_MODE_SETTING`. The ceiling is the native mode;
  changing modes needs the graphics session and no live imported surfaces, and
  releasing the session returns to native. New boot log: `fb_gpu: <W>x<H> native,
  mode-setting ...`.

- **The SXE format: executables carry their identity inside.** A `<name>.sxres`
  manifest is stamped into non-alloc ELF sections (`.sxmeta`/`.sxicon`), so the
  binary declares its own title, version, icons, accent and the extensions it
  opens. Program Manager, `windowd` and Files read from there instead of
  per-path tables that had to be recompiled. Format in `docs/SXE_FORMAT.md`; new
  `build.ps1 sxe-smoke` and `filesapp-smoke`.

- **Default button in dialogs** (`default_button` in `struct sxgui_dialog`).
  Enter triggers it and it is drawn with the period double border. Declared as
  Save in the notepad, OK in the About boxes and **No** in the shutdown one.

- **The notepad warns before losing unsaved changes.** Exit, New and Open ask
  Save / Discard / Cancel and resume the action after saving. Closing via the
  window's X still kills the process: the WM sends SIGKILL instead of requesting
  a close.

- **Notepad (`/bin/notepad`).** A Win95-shaped text editor: File/Edit/Search/Help
  menu, full text area and a status bar with the open file and a modified
  marker. New, Open..., Save, Save As...; F2 saves, F3 opens. 32 KB per document.
  It is the OS's first app that writes files.

- **Multiline editor in sxgui (`sxgui_textedit`).** Caret, insertion and
  deletion, Enter splitting the line, arrows preserving the column,
  Home/End/PageUp/PageDown, click positioning and vertical scrolling. No word
  wrap: long lines follow the caret horizontally.

- **A launch can carry an argument.** `savanxp_desktop_launch_request` gains an
  `argument` field the WM passes as `argv[1]`, exposed as
  `gfx_desktop_launch_arg()`. It is what enables "open this file".

- **Dialogs can start with focus on a widget** (`initial_focus` in `struct
  sxgui_dialog`). Without it a text input dialog opened with no focus.

- **Listbox with columns in sxgui (details view).** Setting `columns` draws a
  fixed header and splits each item by TAB, with optional right alignment
  (`SXGUI_COLUMN_RIGHT`) and per-cell clipping. Opt-in.

- **Size hint: every window starts at the size of its content.** A new
  client->WM channel (`SAVANXP_WM_FD_SIZE_HINT`, fd 11) the WM applies once at
  startup, clamped to the surface capacity. SDK: `gfx_request_content_size()` /
  `gfx_wait_content_size()`; in sxgui, `sxgui_app_autosize()` or
  `sxgui_app_set_content_size()`.

- **`svfs-cli rm`: something can be taken out of an SVFS2 image.** Files and
  empty directories only. Previously the sync was purely additive and removing a
  binary meant recreating the whole image.

- **Resizing windows by their edges.** Edges and corners drag and the cursor
  anticipates them (`RESIZE_H`/`RESIZE_V`), with the opposite edge anchored. Not
  for frameless, maximized or fullscreen windows.

- **Multi-shape Win9x-style cursors.** Eight shapes (`arrow`, `wait`, `text`,
  `move`, `resize-h`, `resize-v`, `unavailable`, `link`). Apps request theirs
  over `savanxp_desktop_cursor_hint` and the WM resolves by priority.

- **Factory wallpaper in `/disk/wallpaper.bmp`.** Committed in `diskfs/` and
  generated by `tools/GenerateDefaultWallpaper.py` with a fixed seed. It is
  changed from Program Manager's Options, which rewrites `/disk/desktop.cfg`.

- **`build.ps1 net-smoke`: automated NIC coverage.** PCI presence, a non-zero
  MAC, address and gateway, and ARP + ICMP against slirp's gateway requiring the
  tx/rx counters to advance. The network had no harness before.

- **`build.ps1 build -NoTestApps`: an image without the diagnostic apps.** Test
  binaries stay out of the rootfs (59 -> 32) and the launcher is built without
  their entries. The automation commands always include them.

### Changed

- **F11 lowers the scanout resolution instead of scaling in software.** The shell
  used to stretch a fullscreen app's 640x400 buffer every frame, at two integer
  divisions per output pixel. It now asks the compositor for the client surface's
  mode (new `SAVANXP_COMPOSITOR_MSG_SET_MODE`) and the pixels go 1:1. The mode
  belongs to the scanout, so it is restored on leaving fullscreen, on the app
  dying and on the compositor dying. Without mode setting, scaled as before.

- **The kernel's cached geometry is restored on every mode change.**
  `GPU_IOC_SET_MODE` now calls `ui::sync_framebuffer_geometry()`, which
  reprograms the absolute pointer's extent. With the old extent the cursor
  pointed elsewhere after a mode change.

- **`memcpy`/`memset` stop moving memory one byte at a time.** The tree's three
  implementations were C loops of one byte per iteration, and the tree is built
  without `-O`. They now use `rep movsq` / `rep stosq` plus the byte tail; 8
  bytes is the maximum with `-mgeneral-regs-only`. `windowd-smoke` drops from
  ~37 s to ~30 s end to end.

- **`cld` on every kernel entry and at the start of every process.** DF is part
  of the process's RFLAGS, so a program could make the kernel's string
  instructions walk backwards. It is the precondition for the memory routines
  above, and for turning on `-O2` later.

- **Full-rectangle blits are copied in a single pass.** `fb_gpu::blit_rect` and
  `sx_painter_blit_bitmap` issued one `memcpy` per row even when the rows were
  already contiguous in source and destination. Small dirty rects still go row by
  row.

- **Files opens files in the notepad.** Activating something that is not a
  program launches `/bin/notepad` with the path instead of refusing.

- **Files takes the shape of the Win95-era file explorer.** Address bar with "Up
  One Level", a details list with a Name/Size/Type header, and a two-pane status
  bar with the object count and total. The menu becomes File/View/Help.

- **Files no longer shows file contents.** The preview pane is gone: reading a
  file is an editor's job. Opening something unlaunchable now says so in the
  status bar instead of dumping the first bytes.

- **Display, audio, block and NIC pick drivers through a registry, not branches
  in `kernel_main`.** Each driver self-describes with `register_driver` and the
  HAL binds by priority: `bind_best` for display, audio and NIC; `probe_all` for
  block, whose devices coexist. API: `block::initialize` -> `block::probe_all`,
  `block::register_ramdisk` -> `ramdisk::attach_image`. New boot logs: `display:`,
  `audio:`, `nic:`, `block: N device(s)`.

- **Desktop assets: from System.Drawing (GDI+) to Pillow.** The three generators
  move to Python + Pillow, with the same generated C header and no ABI change.
  New build requirement on every platform: `python3` + Pillow.

- **Build migration to Linux: host paths and tools.** The separate builds use `/`
  instead of a literal `\`, and `Build-Iso` compiles the `limine` deployer with
  `make` when there is no prebuilt binary. New requirement on Linux/macOS: `make`
  + `cc`.

- **Program Manager no longer lists programs that are not installed.** New
  `progman_registry_prune_missing(exists)` discards items whose path cannot be
  opened and the groups left empty. It can leave the registry empty, which is the
  honest answer.

- **The build no longer regenerates the desktop art if nothing changed.** With a
  Pillow version different from the one that produced the committed PNGs, every
  build left spurious binary diffs in `assets/desktop/`. The generators now
  compare mtimes.

- **QEMU is assembled with "base" hardware by default.** Standard VGA, PS/2 mouse
  and AC'97 audio, so the fallback backends get exercised without leaving QEMU.
  `-Virtio` goes back to the paravirtualized path and applies to every command
  that launches QEMU.

- **The window manager was separated from the shell (NT 3.5 model).** The old
  `desktop` process managed windows and drew the Win95 chrome at once. The WM is
  now `windowd` and the shell is client processes: `shellui` for the background
  and `progman` for the launcher, with `/disk/progman.ini` editable without
  recompiling. Taskbar, start menu and desktop icons give way to the **Task
  List** (Ctrl+Esc). Contract in `savanxp/wm_protocol.h`.

- **SVFS2 has a single implementation, shared kernel<->host.** The on-disk format
  lives in `include/svfs/svfs_format.h` and is compiled by both the kernel and
  the host tool. `libsvfs/` adds the portable core and `svfs-cli`, which does
  **all** writes to `build/disk.img`; the legacy PowerShell writer was deleted.
  That double implementation was the historical source of the desync bugs.

### Removed

- **The Haxe variants of About and Files (`aboutapp-hx`, `filesapp-hx`) were
  retired.** They were AOT-chain validation demos, not official apps. They go
  with their harnesses, the `native-about`/`native-files` targets and the
  **Native** launcher group, plus the `haxe-toolkit/` widgets only they used. The
  `sx_sysinfo.c`/`sx_fs.c` runtimes stay: they are declared native ABI surface.

- **Deleted `subsystems/posix/userland/busybox.c`.** The hand-written multicall
  was built by nothing any more: the installed applets come from
  `vendor/busybox-port`.

### Fixed

- **`realloc` could hang while growing a block.** The in-place growth loop called
  `sx_merge_with_next` on an occupied block, and that helper does nothing unless
  the block is free, so the loop never advanced. `sx_absorb_next` now reports
  whether it absorbed anything.

- **The AC'97 driver truncated the userland buffer address to 32 bits.**
  `copy_period` took it as a `uint32_t`. It was latent while every audio buffer
  came from the ELF image or the BSS, but a section view is mapped at 64 GiB, and
  there `audio_write` returns EINVAL — a client that mutes on the first `write`
  error (Doom) stays mute for the session. `audiotest` now allocates its buffer
  with `section_create` so the 64-bit path is exercised.

- **A smoke left the automation spec stuck forever.** The build wrote `SMOKE`
  into the rootfs but never deleted it, so `init` started that runner instead of
  the desktop on every later build, until the next `clean`.

- **Opening an app briefly showed the empty window at the generic size.** The WM
  composed the window from the fork, without waiting for the first frame. While
  the client starts, the feedback is now the WAIT cursor.

- **Resize: the window ended up half black.** `resize_overlay_client_surface`
  cleared the surface **after** publishing the new dimensions, erasing the frame
  the client had just copied. It surfaced with the size hint, but the bug was in
  edge resizing all along.

- **The power button did not shut the machine down: the SCI was routed twice.**
  `acpi::start_sci()` and the uACPI glue routed the same GSI, overwriting each
  other. An interrupt line now admits several owners, which also covers two PCI
  links resolving to the same GSI. New `acpi::enable_power_button()`, called
  again after `uacpi_initialize` turns off every fixed event.

- **VirtualBox with I/O APIC enabled did not finish booting: xAPIC MMIO support
  in the local APIC.** `initialize_local_apic` only spoke x2APIC over MSR, which
  VBox never exposes, so with the I/O APIC active the firmware masked LINT0, the
  PIT fallback died and the boot froze on the splash. It now maps the MMIO window
  from `IA32_APIC_BASE` and restores LINT0 as ExtINT. New log: `cpu: local APIC
  in xAPIC|x2APIC mode (id N)`.

- **Key Test** overwrote the second help line with the first event, and **Mouse
  Test** clipped the last line of its panel.

- **The build did not compile on native Linux.** `svfs-cli` needs
  `_POSIX_C_SOURCE` for `fseeko`/`off_t` under `clang -std=c11`, and
  `New-SvfsManifest` built relative paths assuming a `\` separator, leaving a
  leading `/` that `svfs-cli apply` rejected. Neither affects Windows.

- **`exec`/`spawn` reported ENOENT for any load failure.** A perfectly installed
  binary failed with `no such file or directory` when the real problem was
  memory. Each step now reports its own reason: `ENOMEM`, `ENOEXEC`, `EIO`,
  `EACCES`, and the kernel logs the failing step and the free page count.

- **The WM hung entirely because of a client that did not drain its input.**
  Opening a second instance of a program froze the whole session: the WM writes
  input non-blocking, but the kernel ignored `O_NONBLOCK` on **partial** pipe
  writes.

- **The cursor froze with the Task List open.** Its handler consumed the pointer
  event and skipped the cursor repaint.

- **`apply` on `build/disk.img` failed with "no contiguous space" without being
  fragmented.** The buffer where `ensure_capacity` preserves the inode was on the
  stack and capped at 20400 bytes. The image also self-compacts now and apply
  retries.

- **`ps` printed the literal `%-13s` in the STATE column.** The busybox applet's
  `printf` did not support the `-` left-justification flag.

## [0.3.3] - 2026-07-09

### Added

- **Audio on VirtualBox: AC'97 driver + audio HAL with backends.** Audio only
  worked with `virtio-sound-pci`, so everything was mute on VBox, Doom included.
  The subsystem moves to a HAL mirroring the display one, with a
  backend-agnostic `/dev/audio0` and two backends: `virtio_sound` and the new
  `ac97` (`kernel/ac97.cpp`), driving the Intel ICH over bus-master DMA with
  pure CIV polling to dodge the INTx VBox never delivers.

- **LiveCD: a self-contained `/disk` in the ISO through a writable ramdisk.**
  `disk.img` travels as a second Limine module and the kernel exposes it as an
  in-memory block device, so the ISO boots with `/disk` mounted. It is
  writable-ephemeral: changes are lost on reboot. A persistent IDE disk keeps
  priority.

- **Doom with Freedoom (a free IWAD) on the LiveCD.**
  `sdk/doomgeneric/build.ps1` bakes `freedoom1.wad` into `/disk/games/doom/`, so
  the ISO ships a playable Doom with no proprietary content. The engine still
  detects `doom1.wad`/`doom.wad` if supplied.

- **IOAPIC/MADT layer and IRQ routing by GSI.** It parses the MADT, programs the
  redirection entries and routes GSIs to IDT vectors through the Local APIC
  (`ioapic::route_gsi` / `route_legacy_irq`), with vectors 50-63 reserved. PS/2
  migrates to it, falling back to the legacy PIC without a MADT. Prerequisite for
  the ACPI SCI and for INTx through `_PRT`.

- **ACPI: SCI routed through the IOAPIC + power button.** `acpi::start_sci()`
  enables ACPI mode, masks every GPE (no AML interpreter, to avoid storms on the
  level-triggered SCI), enables PWRBTN and routes the SCI. The handler triggers
  `acpi::shutdown()` (S5), with no graceful userland teardown.

- **uACPI vendored and integrated: a real AML interpreter.** A baked copy of
  v6.0.0 under `vendor/uacpi/`, with glue in `kernel/uacpi_glue.cpp` over
  heap/vmm/pci/timer/ioapic and time from the TSC, because interrupts are off
  during bringup. It runs alongside the hand-rolled ACPI;
  `uacpi_namespace_initialize()` is deferred to the events stage.

- **INTx routing through uACPI's `_PRT`, closed end to end.**
  `route_pci_intx()` resolves the link device to its real GSI by evaluating
  `_CRS` and programs the IOAPIC with the firmware's polarity and trigger.
  `rtl8139` moves to interrupt-driven over this path. Legacy INTx now arrives on
  q35+APIC, the bottleneck that had forced MSI-X for virtio-gpu.

- **Native subsystem — Phase 2: native ABI v1 + a real runtime.**
  `savanxp_native_abi.h` is the single source: a partitioned syscall space
  (`< 0x1000` delegated to posix, `>= 0x1000` its own), a mandatory version
  handshake (exit 132 on mismatch) and the first two native syscalls
  (`SXN_SYS_INFO`, `SXN_SYS_LOG`). The runtime gains a heap, `operator
  new/delete` and a mini freestanding `<memory>`, so Haxe classes run.

- **Native subsystem — `_std` override: real Haxe String and Array.**
  reflaxe.CPP's `_std` is exposed as build-generated `*.cross.hx` overrides,
  Haxe's official per-platform mechanism. Two codegen fixes without patching the
  pinned libs: a `Math.hx` shadow and the `UniqueLocalNames` preprocessor,
  because flattened sibling blocks collided two for-in counters in one C++ scope.

- **Native subsystem — gfx ABI + a GUI hello in Haxe.** A graphics syscall block
  (`0x1010`: GFX_INFO / ACQUIRE / RELEASE / PRESENT). The display is first-class
  ABI, with no `/dev/gpu0` and no ioctls, sharing `display::`'s internals and the
  per-pid session. Haxe's Float does not work in freestanding yet.

- **Native subsystem — compositor client protocol (windowed apps).** An
  `sxn_gui_*` layer speaking the v3 surface contract over fds 3..9, on baseline
  syscalls only. First native windowed app: `nativegui`, verified headless with
  `test/guihost.c` — also the first test of a subsystem switch through exec.

- **A generic waitable header in the Object Manager (`object::Header`).** The
  signalling state duplicated per type moves into the common base, so a new
  waitable type no longer forces touching the wait dispatcher.

- **A real semaphore (`SAVANXP_SYS_SEMAPHORE_CREATE`/`_RELEASE`).** The first use
  of the generic header: it saturates at `max_count` and rejects a release that
  would exceed it, with SDK wrappers and `semaphoretest` in the `smoke` suite.

- **`build.ps1 run`/`debug`: `-Accel whpx` support.** It forces `-cpu qemu64`
  because `-cpu max`/`host` under whpx crash OVMF with a `#GP` in `PlatformPei`.
  The automated targets stay on TCG on purpose.

- **`virtio-sound`: the end of silent muting.** If the device responds but no
  output stream offers the ABI's fixed format, the failure is logged instead of
  leaving `/dev/audio0` unregistered without a trace.

### Changed

- **Kernel+userland compilation through Ninja.** It replaces `build.ps1`'s
  sequential, non-incremental phase (~250-300 sources) with parallel builds and
  header dependency tracking through `-MMD`, pinned in the toolchain. The link,
  the image, the ISO and QEMU are unchanged.

- `savanxp_mode_bits` (SDK) loses the enum's fixed underlying type, which
  triggered `-Wfixed-enum-extension` in every TU including `syscall.h`. No
  functional change.

### Fixed

- **`virtio-gpu`: `SET_SCANOUT` hung forever under WHPX.** The boot got stuck at
  "Preparing display": the second wait tier fell to `HLT`, and during early boot
  the kernel runs with `IF=0`, so it only wakes on an NMI. The wait now uses
  `timer::monotonic_ns()` with a bounded busy spin.

- **virtio-sound: async multi-buffer TX playback.** The TX path sent one period
  and spun waiting for the device with interrupts disabled. The queue now uses a
  ring of `kTxSlots` periods and drops instead of blocking when it is full.

- **AC'97: non-blocking playback, a silence cushion and no IOC.** Three fixes for
  choppy audio on VirtualBox: `submit_period` stops spinning with IRQs off (it
  froze the guest clock Doom uses to pace audio *and* its logic); the BDL entries
  lose the interrupt-on-completion bit nobody serviced; and periods of silence
  are preloaded to absorb producer jitter. New `ac97-count` and `ac97-stream`.

- **"Robotic"/stuttering audio in Doom (underfeeding the device).**
  `DG_Sound_Update` advanced every channel's position but wrote only the total
  rounded down to the period, feeding the device at ~0.75x and running effects
  ~1.34x fast. It was in the glue's common layer, so it sounded the same on both
  drivers.

- **Visual cursor residue over compositor elements.** `sx_painter_draw_frame`
  drew the frame of the rect already intersected with the clip, so a fragmented
  repaint gave each fragment its own border. It now traces the original rect as
  four clipped strips. New headless regression `build.ps1 cursor-repro`.

- **Polish for `fb_gpu` and `virtio-gpu`.** `present_region` read the origin from
  row 0 instead of the surface offset and `GET_STATS` returned zeros;
  `refresh_scanouts` no longer clobbers a fullscreen flip; the cursor queue's
  `used` header is read volatile; `notify_off_multiplier == 0` no longer skips
  the notify; and `REFRESH_SCANOUTS` requires the graphics session.

- **`build.ps1` silently forked to PowerShell 5.1 to generate assets.** `&
  powershell` always resolves to Windows PowerShell, so running everything with
  `pwsh` 7 gave a false sense of portability. The scripts are now invoked
  in-process.

- **`Join-Path` paths with an embedded `\`, incompatible outside Windows.** On
  Windows it worked by accident; on Linux `\` stays a literal character. ~18
  occurrences normalized to `/`.

## [0.3.2] - 2026-07-03

### Added

- **Complete sxgui: a Win9x-style widget toolkit.** From 5 basic controls to a
  full toolkit, keeping the allocation-free retained-mode model: focus traversal,
  a textfield with a real caret, scrollbar and scrolling listbox, action reasons
  (`CLICK`/`CHANGE`/`ACTIVATE`), radio groups, a combobox drawn inside its own
  backbuffer, a menu bar with `on_command(id)`, modal dialogs as a state machine
  in the same main loop, groupbox, progress bar and textview.

- **The `sxgui_app` app frame.** It encapsulates the gfx session and the main
  loop every widget app repeated (input polling, RESIZED, gated repaint, present,
  16 ms throttle), with optional `on_key`/`on_paint`/`on_resize` hooks.

- `widgetsdemo` grows into a reference gallery of the whole toolkit.

### Changed

- **`aboutapp` and `filesapp` ported to sxgui.** aboutapp becomes declarative;
  filesapp keeps its filesystem logic but delegates the list, preview, menu bar
  and status bar. Both lose their static 8 MiB backbuffer and manual event loop.

- **The SDK's malloc arena drops from 48 MiB to 8 MiB by default.** The static
  heap lives in the BSS and the kernel maps the whole BSS at exec, so each app
  cost ~50 MiB resident and three of them exhausted physical memory. External
  builds keep 48 MiB through `-DSX_HEAP_SIZE` for heavy apps like Doom.

### Fixed

- **Physical memory leak on fork from section view pages.**
  `vm::clone_address_space` copied every present user page and only then
  discarded the section view ones, without freeing the copy. Since the desktop
  maps every client surface's view, each launch lost ~4 MiB per view and after a
  few of them no `exec` worked until reboot.

- `desktop_client.path` stored the pointer received at launch, which for
  client-requested launches pointed at the request's stack buffer: the window
  title and the logs read dangling memory. The client now keeps its own copy.

## [0.3.1] - 2026-07-02

### Added

- **Separate graphics compositor (`/bin/compositord`).** Direct `/dev/gpu0`
  access, display surface import, batched presents, the present timeline and the
  hardware cursor moved into a userland daemon. `desktop` starts it with pipes
  and an inherited framebuffer section and speaks a versioned binary protocol
  (`savanxp/compositor_protocol.h`). The shell keeps window policy, chrome and
  input routing but no longer issues GPU ioctls.

- **GPU HAL: a swappable display backend.** `namespace display` goes from a fixed
  passthrough to `virtio_gpu` to a real `display::Backend` vtable, with
  `/dev/gpu0` registered by a backend-agnostic dispatcher
  (`kernel/gpu_device.cpp`). New `kernel/fb_gpu.cpp` backend: software
  composition straight onto Limine's linear framebuffer, for when there is no
  virtio-gpu. `kernel_main` autodetects.

- **Composited fullscreen for apps (F11 key).** A fullscreen-capable app (Doom,
  Gfx Demo) goes chromeless: the shell scales its 640x400 buffer and presents
  through `compositord`, with no mode change and no client scanout flip. It works
  over virtio-gpu and over the flat framebuffer alike.

- **Native subsystem (Haxe) — Phase 0 kickoff.** The chain is proven end to end
  at compile/link level: `Main.hx` -> `reflaxe.CPP` -> C++17 -> freestanding
  clang++ -> a native ELF. New `subsystems/native/` with a seed SDK and a
  separate `build.ps1` that pins reflaxe under `toolchain/haxe-libs/`.

- **Native subsystem — Phase 1: real native processes.** A native binary is
  marked with `e_ident[EI_OSABI]=0x53` and the loader assigns
  `subsystem::Id::native` by the binary's ABI, not by inheritance from the
  parent. Its syscalls enter through `dispatch_native_syscall`, which delegates
  the baseline to posix and stands as the point of divergence.

- Real typefaces baked offline into C tables with `tools/font/genfont.py`: **GNU
  UniFont 8x16** for the kernel console and the terminal, and proportional
  antialiased **Noto Sans** for the desktop chrome. The OS still does not parse
  TrueType at runtime.

- A monospace text path in `sxgfx` (`gfx_blit_text_mono`,
  `gfx_cell_width/height`) and per-pixel alpha blending (`gfx_pixel_blend`) for
  Noto's antialiased text.

- Real interrupts through **MSI-X** in `virtio-gpu`, with an ISR/DPC pattern
  instead of pure polling. The kernel has no IOAPIC and q35 in APIC mode does not
  deliver legacy INTx, so MSI-X is the only path. Includes
  `pci::find_capability`, `virtio_pci::enable_msix` and IDT vector 49.

- `poll()` reports readiness for kernel waitable objects (events, timers), so the
  compositor can wait for its clients' submit events in the same poll set.

- An on-screen FPS and present latency overlay in Doom's backend, and a dump of
  the driver's per-stage stats in `gputest --soak`.

- A `PIT` timer backend (8254, IRQ0) as a fallback when the local APIC does not
  support x2APIC or calibration fails; previously the scheduler never started
  there. `savanxp_system_info` exposes the active backend
  (`SAVANXP_TIMER_LOCAL_APIC` / `_PIT` / `_NONE`).

### Changed

- The desktop compositor composes by **regions with occlusion culling**: it
  builds the layer list in z-order and paints each one once over its visible
  region, eliminating overdraw under opaque windows. New
  `sx_rect_set_subtract_rect`; `sx_rect_set` capacity goes from 32 to 64.

- The kernel console moves from the hand-authored 5x7 bitmap font to UniFont
  8x16, with ASCII + Latin-1 + box drawing through a sparse table.

- `gfx_blit_text` rasterizes Noto Sans from an 8-bit coverage atlas; `shellapp`
  uses the UniFont monospace path. The desktop chrome metrics were readjusted for
  the larger line height.

- UniFont is baked from `unifont.hex` (crisp on-grid bitmaps), not from the TTF
  outline, which rasterized off-grid with artifacts.

- The kernel timer goes from 200 Hz to **1000 Hz**, with the scheduler quantum
  rescaled to keep ~20 ms of wall clock. Signalling an event now yields to the
  woken thread on the syscall return instead of waiting for the next tick.

- The compositor wakes on a client's frame submit instead of exhausting the
  16 ms timeout, which remains as a backstop.

- The `virtio-gpu` driver becomes interrupt-driven: the present waiters' spin is
  cut from 50000 to 2000 iterations before halting, and the backstop timeouts are
  expressed in wall-clock milliseconds to survive the tick rate change.

### Removed

- The hand-generated 8x8 font system (`tools/font/genfont.ps1`, `font8x8.txt`,
  `gfx_font8x8.inc`), replaced by the `genfont.py` toolchain.

### Fixed

- `release_surface_allocation` (`virtio-gpu`) freed the primary surface's backing
  without a RESOURCE_UNREF of the host resource, so the first real runtime mode
  change failed with `RESOURCE_CREATE_2D` -> INVALID_RESOURCE_ID.

- `decode_bar_size` computed `~mask + 1` in 64 bits over a mask holding only the
  low 32 bits, returning garbage sizes for any 64-bit memory BAR under 4 GiB and
  preventing them from being mapped — the MSI-X table among them.

- **Inode corruption from rounding in the SVFS2 bitmap.** The host-side installer
  computed the byte index with `[int]($Bit / 8)`, which in PowerShell rounds
  instead of truncating, desyncing from the kernel's floor indexing and
  reassigning live inodes (symptom: "expected inode N but read 0"). Now `-shr 3`.

- An orderly QEMU shutdown in `Run-AutomationQemu`: `quit` over the HMP monitor
  instead of `Stop-Process -Force`, so QEMU flushes its block backends and closes
  the disk file cleanly, with a fallback to the forced kill.

- The OS hung indefinitely after "Starting welcome" on hypervisors without
  x2APIC: `initialize_local_apic` failed silently and the scheduler never
  started. Resolved by the `PIT` fallback above.

- **Erratic cursor: PS/2 packet framing corrupted in streaming.**
  `process_mouse_byte` discarded every `0xFA`/`0xFE` assuming ACK/RESEND, but in
  streaming those are also legitimate deltas (`-6` and `-2`), and losing the byte
  desynced the 3-byte framing. The bug was directional — only left/down. A
  defensive clamp of +-150 per axis clips instead of discarding the packet.

- `reserve_kernel_mmio_window` reserved a PML4 entry without installing the
  corresponding PDPT, so any later `map_kernel_mmio` over that window failed or
  corrupted memory.

- `fb_gpu`'s present timeline always returned `submitted_sequence = 0,
  retired_sequence = 0`, so `wait_present` depended on a lucky coincidence
  instead of a real counter.

- **Experimental VirtualBox compatibility (`VBoxVGA` backend).** The system now
  boots stably to a graphical session and the PS/2 mouse responds in all four
  directions. Open and undiagnosed: the binaries in `/disk` still do not start
  under VirtualBox even with the SVFS2 volume attached.

## [0.3.0] - 2026-06-18

### Added

- An automated headless graphics smoke for the compositor: `desktop --selftest`
  starts the compositor, imports the scanout, launches a real client and
  deterministically validates multi-window composition and the advance of the
  GPU's present timeline, also exercising maximize/restore/minimize/move.
  Exposed as `.uild.ps1 desktop-smoke` (token `DESKTOP SMOKE PASS/FAIL`),
  closing a graphics path that used to be tested only by hand.
- New desktop client apps: `filesapp` (browsing `/disk` with a preview) and
  `aboutapp` (a system summary), wired into the Start menu.
- Explicit policy and traceability for adopting components inspired by
  SerenityOS, with new documentation in `docs/THIRD_PARTY_ADOPTION.md` and
  `docs/THIRD_PARTY_PROVENANCE.md`.
- A reusable 2D graphics layer `sxgfx` in the POSIX SDK v1, with `sx_bitmap`,
  `sx_painter`, alpha blending, clipping and `sx_rect_set` for handling multiple
  damage rects from userland.
- A `display` facade inspired by `DisplayConnector` over the `virtio-gpu`
  backend, with connector properties, surface import/release, present batching,
  a timeline and waitable events exportable to userland.
- The `SAVANXP_GPU_CLIENT_SURFACE_VERSION_3` graphics contract for client apps,
  based on `section_create/map_view` plus dirty rect batches and
  `submit/retire/shutdown` events instead of the legacy present pipe.
- A new, wider public ABI on `/dev/gpu0` for explicit present tracking, batching
  and connector capabilities, including the timeline, waits/events and
  property/scanout queries without owning the display.
- A real multi-window desktop compositor, with simultaneous overlays, simple
  z-order, an active window, dragging from the title bar and a close button in
  the top right corner.
- A bitmap asset pipeline of our own for the desktop, with embedded PNG icons
  and art generated inside the repository for the taskbar, Start menu, title bars
  and side strip, removing the dependency on SerenityOS assets.
- Wider `virtio-gpu` statistics with end-to-end per-frame latency, including
  accumulated samples and the worst case observed.

### Changed

- The system reports itself as `v0.3.0` in the kernel, the shell, `uname`,
  `sysinfo`, `aboutapp` and the other components consuming the shared version.
- `virtio-gpu`'s background progress stops depending on the input subsystem and
  is pumped from a kernel device service invoked on the timer and on blocking
  waits.
- The `desktop` leaves behind the single fullscreen client model and moves to a
  surface compositor with multi-rectangle invalidation, composition by clipping
  and batched presents to the primary scanout.
- The `desktop`'s main loop is decoupled into layout/render/menu/session, keeping
  a single binary but better separating the responsibilities of the compositor,
  the background shell and the overlay windows.
- `shellapp`, `doomgeneric` and the other client apps migrate to the async
  version 3 surface channel; the terminal forces a full redraw when scrolling
  moves the history, to avoid visual artifacts in the window.
- Compositor-GPU synchronization moves to an explicit timeline and to retiring
  the previous frame before recycling the visible backbuffer, reducing logical
  tearing and improving the desktop's pacing.
- The Start menu and the taskbar get several passes of visual and behavioral
  polish: a cleaner layout, a bitmap sidebar, text that fits better, stable
  hover and better feedback for the active client.
- Overlay windows can now be moved inside the desktop's work area and closed
  straight from their title bar with a classic shell-style cross.
- The desktop's embedded art is now generated from the repository's own assets,
  replacing the temporary references used during the SerenityOS-inspired
  prototyping.
- `gputest --smoke` now also validates the present timeline and the driver's
  explicit pacing, not just internal stats/stages.
- The persistent `build/disk.img` flow is re-validated after every large batch of
  changes, with `doomgeneric` as the real non-regression test.
- `virtio-gpu` reorganizes its internal state around an `Adapter` with separate
  substates for transport, display, cursor, presents and runtime, leaving a
  better base for fine-grained locking and more predictable recovery.
- `virtio-gpu`'s background work is split into explicit phases of queue draining,
  pipeline advance and config event processing, with short atomic serialization
  for virtqueue submit/drain.
- The present timeline now recognizes `present_cookie` as a real correlation
  even with coalescing, range retirement and damage batching.
- The partial presentation path can now update the active front buffer without a
  full clone when the resource is idle, and imported batches keep real rects for
  `TRANSFER_TO_HOST_2D` and `RESOURCE_FLUSH` before falling back to the bounding
  rect only if the internal capacity runs out.
- The `desktop` starts using the waitable present event exported by `/dev/gpu0`
  as a readiness hint to reduce unnecessary polling of the timeline, keeping
  `WAIT_PRESENT` as the strong synchronization.
- `virtio-gpu`'s display/scanout event handling is hardened: a failed refresh now
  triggers deliberate degradation and recovery instead of staying a silent
  failure of the hotplug/config path.
- `gputest --smoke` raises driver coverage by validating waitable present and
  scanout handles, together with a light soak of partial presents and repeated
  refreshes to catch pacing and recovery regressions earlier.
- The surface reservation and pending slot timeouts in `virtio-gpu` are now
  treated as symptoms of a real pipeline stall, entering degraded mode and
  attempting recovery just like the other critical waits.
- `virtio-gpu`'s recovery avoids simultaneous reentry and `gputest --smoke`
  hardens event coverage by also checking that the handles go back to unsignalled
  after `event_reset`, with a somewhat more aggressive soak.
- `gputest` adds a dedicated `--soak` mode to exercise the `/dev/gpu0` backend
  for longer with a deterministic mix of full presents, partial presents, event
  waits and scanout refreshes, without slowing down the normal smoke.
- `build.ps1` adds the `gpu-soak` command, reusing `/SMOKE` as the runner
  selector so `init` can launch `gputest --soak` in QEMU and report `SOAK
  PASS/FAIL` tokens suitable for automated validation.
- `virtio-gpu`'s recovery becomes less global in non-critical domains: a failure
  to rearm the cursor now degrades to a software cursor and non-critical imported
  surfaces can be dropped locally during recovery instead of tipping over the
  device's whole rearm.
- `virtio-gpu`'s scanout refreshes become transactional over the connector
  cache: if the host delivers an incomplete event or rearming the primary fails,
  the driver restores the previous state and only escalates to global recovery
  when it cannot even keep the primary scanout.
- `gputest --soak` now accepts an iteration count and also covers imported
  surfaces through `GPU_IOC_IMPORT_SECTION` and
  `GPU_IOC_PRESENT_SURFACE_BATCH`, with `build.ps1 gpu-soak -GpuSoakIterations N`
  to repeat longer runs without touching the normal smoke.
- The `desktop` compositor fixes the pacing of its imported presents by using
  the real timeline to generate `present_cookie`, avoiding retiring frames before
  the GPU is done and reducing visible tearing in heavy apps.
- The SDK's `gfx` runtime stops exposing as a direct backbuffer the same shared
  buffer the compositor reads: each client draws into a private backbuffer and
  the runtime copies to the shared surface only once the previous frame has been
  composed, eliminating the frame backlog without waiting for the GPU's final
  retirement.
- The client surface protocol adds `composed_sequence` to separate desktop
  composition from GPU retirement; the compositor signals progress as soon as it
  copies the frame into the visible backbuffer and keeps `retired_sequence` for
  the present's real retirement.
- The `desktop` limits how many mouse events it drains per frame so that
  dragging windows does not accumulate a large backlog before composing again.
- `poll()` stops treating every device as always readable: `/dev/input0` and
  `/dev/mouse0` expose real queue readiness, reducing the compositor's busy loop
  when there are no pending events.
- The `desktop`'s mouse handling moves to reading blocks per frame and coalescing
  consecutive movements with the same button state, following SerenityOS's
  WindowServer pattern so as not to compose a frame per raw packet.
- `sx_rect_set` fixes the merging of adjacent rectangles so it does not join
  separate areas that merely share an edge coordinate, avoiding artificially huge
  dirty rects when moving windows.
- `doomgeneric` and the compositor now print the concrete present or invalid
  batch error when a client surface fails, making visual regressions easier to
  diagnose.

## [0.2.2] - 2026-04-01

### Added

- A new `subsystems/` tree with `subsystems/posix` as the OS's first explicit
  subsystem, separating `kernel`, `userland` and `sdk` under a single ownership.

### Changed

- The POSIX syscall entry and dispatcher move under `subsystems/posix/kernel`,
  while `kernel/` keeps the scheduler, base processes, VM, VFS, drivers and the
  other generic mechanisms.
- The canonical SDK v1 moves to `subsystems/posix/sdk/v1`; the main build,
  `tools/build-user.ps1` and the VS Code extension now consume that path as the
  POSIX subsystem's public reference.
- The internal userland moves to `subsystems/posix/userland` and builds against
  the SDK's shared canonical runtime, removing internal duplicates and leaving
  top-level `sdk/` as the root for examples and ports.
- The public definition of the visible ABI is unified in
  `subsystems/posix/sdk/v1/include/savanxp/syscall.h`, with no parallel copy in
  `include/shared`.
- The system reports itself as `v0.2.2` in the kernel, the shell, `uname`,
  `sysinfo` and the components consuming the shared version.
- The migration re-validates the persistent image flow:
  `.\sdk\doomgeneric\build.ps1` and `.\build.ps1 build` still keep
  `doomgeneric` and `doom1.wad` in `build/disk.img` without recreating the image
  under normal operation.

## [0.2.1] - 2026-03-30

### Added

- A new public ABI on `/dev/gpu0` for diagnostics and extended 2D control:
  `GPU_IOC_GET_STATS`, `GPU_IOC_GET_SCANOUTS`, `GPU_IOC_REFRESH_SCANOUTS`,
  `GPU_IOC_SET_CURSOR` and `GPU_IOC_MOVE_CURSOR`.
- Wider `virtio-gpu` statistics for presents, stages, waits, timeouts,
  completions, IRQs, recovery and cursor operations.
- Scanout enumeration and basic display info/hotplug refresh in the `virtio-gpu`
  backend, keeping `desktop` single-display by default.
- Initial hardware cursor plane support in `virtio-gpu`, with a transparent
  fallback to the `desktop`'s software cursor when the backend does not expose it
  or fails.
- Additional automated coverage in `gputest --smoke` to validate driver progress
  through `GPU_IOC_GET_STATS` and scanout enumeration.

### Changed

- The system reports itself as `v0.2.1` in the kernel, the shell, `uname`,
  `sysinfo` and the components consuming the shared version.
- The normal graphics model is now definitively desktop-first: the taskbar stays
  visible, client apps render over a stable work area and the `desktop` becomes
  the normal owner of the scanout.
- `shellapp`, `gfxdemo`, `keytest`, `mousetest` and `doomgeneric` are aligned to
  the compositor's client path instead of the legacy direct fullscreen over
  `/dev/gpu0`.
- The taskbar and the Start menu get a pass of visual and behavioral polish to
  fit the new desktop-first contract better.
- The `virtio-gpu` backend stops depending on opportunistic caller reentry to
  make progress: the internal scheduler now coalesces presents per resource,
  reduces redundant `SET_SCANOUT`s and advances work in the background with IRQ
  support when the PCI line is available.
- `virtio-gpu` adds deliberate recovery and a predictable degraded mode in the
  face of device timeouts, trying to restore the primary scanout and the console
  without requiring an immediate OS restart.
- The main build and the associated tooling make it more explicit that
  `build/disk.img` is persistent by default: `SVFS2` consistency is validated,
  recreating the image is avoided except on real corruption, and `doomgeneric`
  together with `doom1.wad` is kept as a practical persistence regression test.
- The QEMU profiles used by `run`, `smoke` and the graphics utilities align
  better with the current stack's real virtual hardware (`virtio-gpu` +
  `virtio-tablet` + `desktop`).
- `doomgeneric` moves permanently to living as a compositor client and is aimed
  at manual validation inside the normal graphics session, instead of depending
  on a host-side smoke of its own.

## [0.2.0] - 2026-03-22

### Added

- A `desktop-first` session with the `desktop` compositor, the `shellapp`
  fullscreen shell, client surfaces shared through a `SectionObject` and
  graphical app launching through `fd 3..6`.
- An extended public ABI on `/dev/gpu0` with `GPU_IOC_SET_MODE`,
  `GPU_IOC_IMPORT_SECTION`, `GPU_IOC_RELEASE_SURFACE`,
  `GPU_IOC_PRESENT_SURFACE_REGION` and `GPU_IOC_WAIT_IDLE`.
- A new public ABI for audio with `SAVANXP_IOCTL_GROUP_AUDIO`,
  `AUDIO_IOC_GET_INFO` and `struct savanxp_audio_info`.
- A playback-only `virtio-sound` driver over `virtio_pci`, exposing
  `/dev/audio0` with the fixed `S16LE stereo 48 kHz` format.
- A new `audiotest` utility and automated coverage in `.\build.ps1 smoke` to
  validate `/dev/audio0`.
- A minimal object manager with generic kernel handles for I/O, events, timers
  and sections.
- New `EVENT_*`, `WAIT_ONE`, `WAIT_MANY`, `TIMER_*`, `SECTION_CREATE`,
  `MAP_VIEW` and `UNMAP_VIEW` syscalls, with updated wrappers in userland and
  SDK v1.
- Initial anonymous `Section/View` support in the kernel, including shared
  memory between processes, `shared` vs `private` inheritance on `fork()` and
  new `eventtest`, `timertest`, `sectiontest` and `mmaptest` tests.
- A new POSIX layer for anonymous `mmap` / `munmap` in
  `subsystems/posix/sdk/v1`, plus the standard `sys/mman.h` header.

### Changed

- The system reports itself as `v0.2.0` in the kernel, the shell, `uname`,
  `sysinfo` and the components consuming the shared version.
- The normal boot now supervises `desktop` from `init`; `/SMOKE` still avoids
  the desktop and keeps the automated headless smoke.
- `gfx_open` and the graphics runtime become compositor-first, with a direct
  fallback over `/dev/gpu0`; the legacy `/dev/fb0` node is no longer exposed in
  the current system.
- The main build now installs the ported BusyBox multicall in `/bin` and
  `/disk/bin` for `ls`, `cat`, `echo`, `mkdir`, `rm`, `mv`, `cp` and `ps`.
- `virtio-gpu` moves to presenting over an internal set of three surfaces and the
  legacy `FB_IOC_*` path leaves the current ABI.
- The `run` and `smoke` QEMU profiles add `virtio-sound-pci` with an `audiodev`
  separate from the `pcspeaker` path.
- `sleep_ms()` now runs over kernel waitable timers instead of a separate special
  path, and `fork()` preserves anonymous views as shared or private according to
  the mapping type.
- The Start menu no longer offers `Exit Desktop`, and `shellapp` can be closed
  with `exit` to return to the desktop and reopened later from `Menu -> Shell`.
- The desktop compositor cuts some redundant work in the presentation path and
  better fixes input routing/polling for fullscreen clients.

## [0.1.4] - 2026-03-19

### Added

- New POSIX syscalls and wrappers for `fork`, `kill`, `raise`, `poll`, `select`
  and `fcntl(F_GETFL/F_SETFL)` with `O_NONBLOCK` support.
- An automated runner `.\build.ps1 smoke`, which rebuilds, installs into
  `/disk/bin`, boots QEMU headless and validates `fork`, basic signals, polling
  and real persistence over `SVFS2`.
- A `busybox` multicall userland to start replacing the stopgap utilities,
  including `echo`, `cat`, `ls`, `mkdir`, `rm`, `mv`, `cp`, `true`, `false` and
  `sleep`.

### Changed

- The system reports itself as `v0.1.4` in the kernel, the shell, `uname`,
  `sysinfo` and the components consuming the shared version.
- The system's base timer is now calibrated targeting `200 Hz` instead of
  `100 Hz`, slightly improving perceived mouse response and the practical
  rounding of `sleep_ms()` for graphics and input loops.
- The internal ceilings go up for processes, descriptors, pipes, sockets, VFS
  and `SVFS2`, leaving more headroom for ports and a real userland.
- `SVFS2` can now mount `/disk` in a degraded read-only mode if recovery does not
  leave the volume safe for `RW`, avoiding it going straight offline in the face
  of recoverable failures.
- The main build also installs the internal binaries in `/disk/bin`, so the shell
  and the automated smoke exercise the same persistent copy of the userland.

## [0.1.3] - 2026-03-17

### Added

- A shared `virtio_pci` base for modern `virtio` drivers over PCI/MMIO, reused
  by `virtio-input` and prepared for synchronous polling queues.
- A new 2D `virtio-gpu` driver for QEMU, with MVP support for
  `GET_DISPLAY_INFO`, `RESOURCE_CREATE_2D`, `RESOURCE_ATTACH_BACKING`,
  `SET_SCANOUT`, `TRANSFER_TO_HOST_2D` and `RESOURCE_FLUSH`.
- A new `/dev/gpu0` node with the public ABI `GPU_IOC_GET_INFO`,
  `GPU_IOC_ACQUIRE`, `GPU_IOC_RELEASE`, `GPU_IOC_PRESENT` and
  `GPU_IOC_PRESENT_REGION`.
- A new `gputest` utility to validate the direct presentation path over
  `/dev/gpu0`.
- Calibration of the `local APIC/x2APIC` timer against the `RTC/CMOS` during boot
  so that `uptime_ms` and `sleep_ms` line up better with real time in QEMU.

### Changed

- `/dev/fb0` keeps compatibility with the existing fullscreen apps, but can now
  present over `virtio-gpu` when the backend is available.
- The QEMU profile in `build.ps1 run` now adds `virtio-vga` with
  `xres=1280,yres=800`, and `limine.conf` asks for `1280x800x32` so the boot
  framebuffer and the graphics backend line up during the handoff.
- The console and the fullscreen UI can stay visible over `virtio-gpu`'s primary
  resource, including the return from exclusive graphics sessions and a clean
  redraw of the whole shell with no residue at the margins.
- `virtio-gpu` now tries to keep the boot framebuffer's large mode before falling
  back to the native scanout the device reports, preventing the system from
  returning to `640x480` at the end of boot when the larger mode is accepted.
- `virtio-input` moved to using the active framebuffer's effective geometry to
  normalize the absolute tablet, fixing the mouse desyncing from the host after
  the switch to `virtio-gpu`.
- The kernel heap stopped being linear-only and now recycles freed blocks, does
  `split/coalesce` and can return whole arenas to the physical allocator when
  they end up empty.
- The POSIX runtime of `subsystems/posix/sdk/v1` replaced its arena/bump
  allocator with a fixed recyclable heap, so `malloc`, `free`, `calloc` and
  `realloc` now reuse memory in external apps.
- `sdk/doomgeneric` no longer runs accelerated by an incorrect base clock; game
  time is back on a time backend closer to real, leaving the remaining
  performance tuning on the port's side.

### Known limits

- This `virtio-gpu` MVP visibly improves the fullscreen GUI in QEMU, but pixel
  upload is still synchronous, copied by the CPU, with no `mmap`, page flipping
  or real double buffering.
- Porting apps to `/dev/gpu0` reduces layers and sets up the evolution better,
  but the big improvement in smoothness is left for a later stage with shared
  buffers and less blocking presentation.
- The final performance of large external ports such as `sdk/doomgeneric` still
  depends heavily on the scaling cost and the frame size once the system runs at
  higher resolutions.

## [0.1.2] - 2026-03-14

### Added

- Basic `PS/2` mouse support over the `i8042` controller's auxiliary port, with
  IRQ12, standard 3-byte packets and safe degradation to keyboard-only if the
  mouse does not initialize.
- A new `/dev/mouse0` node with dedicated mouse events for graphical apps,
  without breaking `/dev/input0`'s previous semantics.
- The shared ABI extended with `struct savanxp_mouse_event`, public button flags
  and new `mouse_open` / `mouse_poll_event` helpers in the libc/runtime.
- A new fullscreen graphical shell `desktop`, inspired by the visual language of
  Windows 2000, with a taskbar, Start button, clock and cursor.
- A new `mousetest` utility to validate `/dev/mouse0`, relative movement and
  buttons from userland.

### Changed

- The kernel's fullscreen layer now registers `/dev/mouse0` alongside `/dev/fb0`
  and `/dev/input0`, and clears the keyboard/mouse queues when acquiring or
  releasing the exclusive graphics session.
- Under QEMU, the desktop and `mousetest` now prefer an absolute
  `virtio-tablet-pci` backend when available, while `/dev/mouse0` keeps the delta
  ABI so as not to break already-compiled apps.
- The kernel now reserves an MMIO window of its own for modern PCI drivers and
  uses it to map memory BARs safely during boot.
- In QEMU environments with `virtio-tablet-pci`, the `PS/2` stack stops
  initializing the auxiliary mouse and stays keyboard-only; the `PS/2` mouse is
  kept as a fallback when `virtio-input` is not available.
- `RTC/CMOS` reading was added to the kernel along with an additive public helper
  for querying real time from userland; the desktop's clock no longer depends on
  `uptime` alone.
- The builtin shell now lists `desktop` and `mousetest` in the interactive help.
- The main documentation and the SDK v1 reference reflect the new mouse input,
  the initial desktop and the jump to `v0.1.2`.

### Known limits

- `v0.1.2` exposes only relative movement and basic buttons in the public ABI;
  internally it can use an absolute pointer under QEMU, but there is no wheel, no
  real windows, no compositor and no raw input for games.
- `gfx_poll_event` is still keyboard-only at this stage; the mouse comes in
  through `/dev/mouse0`.

## [0.1.1] - 2026-03-10

### Added

- `SVFS2` as the new version of `/disk`'s persistent filesystem, with a
  primary/secondary `superblock`, a fixed metadata journal, a block bitmap, an
  inode bitmap and an inode table with extents.
- A new `sync` syscall and a userland `sync` command to force an explicit
  checkpoint of the persistent state.
- Minimal networking over `rtl8139` + `QEMU user-net`, with `ARP`, `IPv4`,
  `ICMP` echo request/reply, basic IPv4 UDP sockets and a minimal TCP client.
- An extended public ABI for devices and `ioctl`, with the `/dev/fb0`,
  `/dev/input0`, `/dev/net0` and `/dev/pcspk` nodes.
- An initial fullscreen GUI with `gfx_*` primitives, the internal `gfxdemo` demo
  and the external `sdk/gfxhello` example.
- Minimal sound over the `PC speaker` with a `beep` command.
- The first large external port in `sdk/doomgeneric`, used as a practical
  milestone for external graphical apps over the system's ABI.
- An initial POSIX/libc layer for SDK v1 with public standard headers:
  `unistd.h`, `fcntl.h`, `stdio.h`, `stdlib.h`, `string.h`, `dirent.h`,
  `sys/stat.h`, `sys/socket.h`, `netinet/in.h`, `arpa/inet.h`, `time.h` and
  friends.
- A new `subsystems/posix/sdk/v1/runtime/posix.c` runtime for external apps,
  with basic `stdio`, `DIR*`, a simple arena-style heap, conversions, string
  helpers, time and client sockets.
- New syscalls/base ABI for `getpid`, `stat`, `fstat`, `chdir` and `getcwd`.
- An external smoke test `sdk/posixsmoke`, compiled only against standard
  headers.
- A `keytest` utility to inspect keyboard events over `/dev/input0` in fullscreen
  and validate `key down/up`, `keycode` and `ascii`.
- `FB_IOC_PRESENT_REGION` as an extension of the graphics ABI to present only a
  region of the framebuffer from userland.

### Changed

- The VFS layer now centralizes path normalization and raised the internal path
  capacity to `256` bytes, so `process`, `cwd` and filesystem operations share a
  single canonical resolution.
- The host tooling over `build/disk.img` (`build.ps1`, `tools/build-user.ps1`
  and `tools/UserAppCommon.ps1`) stopped writing `SVFS1` and moved to creating
  and installing directly onto `SVFS2` images.
- `/disk`'s persistent paths stopped depending on path names as their on-disk
  identity and moved to being mounted from stable inodes cached in kernel memory.
- The kernel now resolves relative paths against a per-process `cwd`, so `open`,
  `exec`, `spawn` and filesystem operations share the current directory.
- The `PS/2` keyboard stack was hardened with a more robust controller init,
  decoding decoupled from `TTY`/`UI`, and better support for `AltGr`, locks and
  special keys.
- `sdk/doomgeneric` stopped depending on its private set of standard headers and
  moved to consuming the SDK's public layer, reducing `savanxp_compat.c` to
  port-specific glue.
- The shared runtime's `gfx_*` primitives were optimized to work with contiguous
  spans/rectangles and reduce the per-frame drawing cost.
- `gfxdemo`, `sdk/gfxhello` and `keytest` stopped refreshing the whole screen on
  every iteration and now use dirty regions or on-demand redraws to improve
  fluidity in fullscreen.
- `sdk/doomgeneric`'s backend replaced per-pixel division-based scaling with
  cached row expansion, lowering the per-frame CPU cost during fullscreen
  rendering.
- The main documentation and the SDK reference were updated to reflect the
  available POSIX/libc surface and its practical limits.

### Known limits

- The recent validation of the jump to `SVFS2` covers a full build and host-side
  verification of the `build-user` flow, but does not yet include reboot/replay
  smoke tests inside QEMU.
- If journal or base metadata recovery fails at mount time, the volume goes
  offline; there is no read-only degraded mode yet.
- `free()` does not recycle memory yet; the userland allocator is still
  arena/bump style.
- `DIR->d_type` is filled in by a best-effort `stat()` in userland.
- `setsockopt`/`getsockopt` cover only basic client flags and timeouts.
- Depending on the host and QEMU's keyboard capture, `PrintScreen` may not reach
  the guest as a dedicated key and may require `Alt+PrintScreen` for manual
  testing.
- The recent validation of `v0.1.1` is host-side; the new POSIX smoke has not
  been run inside QEMU in this batch.

## [0.1.0] - 2026-03-08

The experiment's first published version.

### Added

- Kernel bootstrap over `x86_64 + UEFI + Limine`, receiving `bootloader info`,
  `framebuffer`, `memory map`, `HHDM` and `initramfs`.
- A text console over the framebuffer with scrolling and a cursor, plus early
  serial output over `COM1` / `debugcon`.
- GDT/IDT with user segments, `TSS`, basic exceptions and a syscall gate through
  `int 0x80`.
- An early physical allocator, a kernel heap and a minimal VMM for user address
  spaces.
- A `PS/2` keyboard driver, a canonical `TTY` and an initial interactive shell.
- A minimal `VFS` mounting a `cpio newc` `initramfs`, with dynamic in-memory
  files and a persistent `SVFS` volume mounted at `/disk`.
- A static `ELF64` loader for simple `ring 3` processes.
- A preemptive round-robin scheduler with blocking on `wait`, `read` and `sleep`.
- A shell with `pipes`, redirection (`|`, `<`, `>`, `>>`, `2>`, `2>>`, `2>&1`),
  a single/double quote parser and the `exec`, `which` and `mkdir` builtins.
- Refcounted handles with `dup`, `dup2`, `waitpid(-1)` and zombie/reap
  processes.
- Page reclaim on `exit`/`exec`, `VmSpace` destruction and freeing of kernel
  stacks when reaping processes.
- `SVFS` with simple persistent subdirectories under `/disk`, `mkdir`, `rmdir`
  of empty directories, explicit `truncate` and persistent `rename`.
- A minimal SDK v1 in `C`, with `crt0`, `libc`, a linker script, the `savanxp/*`
  headers, host tooling to install external apps into `build/disk.img` and base
  examples (`sdk/hello`, `sdk/errdemo`, `sdk/fsdemo`, `sdk/pathops`,
  `sdk/procpeek`, `sdk/spawnwait`, `sdk/statusdemo`, `sdk/multifile`,
  `sdk/template`).
- An initial userland with `init`, `sh`, `echo`, `uname`, `ls`, `cat`, `sleep`,
  `ticker`, `demo`, `true`, `false`, `ps`, `fdtest`, `waittest`, `pipestress`,
  `spawnloop`, `badptr`, `rm`, `rmdir`, `truncate`, `seektest`, `truncatetest`
  and `errtest`.

### Changed

- Processes, pipes and persistence were consolidated so that `/disk` is
  operational as the main working flow across reboots.
- The syscall surface and the minimal `libc` were widened with the filesystem and
  process operations needed for the shell, pipes and external apps.
- The repository and SDK v1 documentation was frozen to leave a useful public
  base from the first numbered version onwards.
