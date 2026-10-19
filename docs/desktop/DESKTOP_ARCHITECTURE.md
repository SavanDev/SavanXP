# Desktop architecture: compositord, windowd, desktop

> **Status: design accepted (2026-10-19). Nothing in it is built yet.**
> It is the target for the graphics stack and the order for getting there.
> Where it disagrees with the "Target layering" of
> [WM_SUBSYSTEM.md](./WM_SUBSYSTEM.md), this document wins; WM_SUBSYSTEM.md
> stays as the record of how `windowd` was extracted from `desktop.c`.

## Why

The WM extraction left the right processes but not the right boundaries.
Several releases later, an audit of `compositord` and `windowd` found:

- **`compositord` composes nothing.** It owns the GPU, programs the video mode,
  imports the display section, forwards dirty rects to the present syscall,
  reports the present timeline and drives the hardware cursor. The
  composition (layers, occlusion, exact damage regions) lives in
  `windowd_render.c`. `windowd` also allocates the display memory, picks the
  initial mode, spawns `compositord` and respawns it when it dies.
- **`windowd` does five jobs:** window manager, compositor, input server,
  process launcher and session starter, and supervisor of `compositord`.
- **`windowd` knows the shell by name.** It hard-codes seven binary paths. It
  has a dedicated client slot for the taskbar, the background, the Start menu
  and each of three popups (keyboard layout, volume, clock). It also hard-codes
  their pixel sizes (duplicated in the popup sources), where each is anchored
  against the taskbar, that the three popups exclude each other, the toggle
  behaviour and click-outside-to-close. Each of those appears as a near-identical
  branch in pointer routing, reaping, the main loop and the layer builder.
- **Shell vocabulary sits in the kernel ABI.**
  `SAVANXP_DESKTOP_LAUNCH_FLAG_TASKBAR_POPUP`, `_START_MENU`, `_VOLUME_POPUP`
  and `_CLOCK_POPUP` are in `include/abi/savanxp_abi.h`, which the kernel never
  reads. They travel through the generic launch queue every client has, so
  any app can open or close the Start menu, even though a role-gated shell
  channel (`wm_shell_protocol.h`) exists.
- **Other shell knowledge in the WM:** the taskbar height
  (`WINDOWD_TASKBAR_HEIGHT`, tied by a static assert to the wallpaper module),
  the wallpaper itself (the fallback background re-reads `/disk/desktop.cfg`
  on every paint), a per-path table of known apps with their accents, a client
  slot for the terminal (`shell_client`) that the normal session never fills,
  and dead code from the old taskbar clock.

The goal of this document is that each process has one job, that the WM does
not know who the shell is, and that the platform services stop depending on
the POSIX subsystem.

## The model: NT, with a compositor added

The reference is NT 3.x for the layering and NT 4.0 for the shell, since the
taskbar is back. NT 3.x had no compositor; that one layer is a later idea
(DWM), and the reference does not decide it.

| NT | SavanXP | Job |
|---|---|---|
| GDI display driver (and DWM, much later) | **`compositord`** | Video mode, display memory, composition, cursor, present |
| USER inside CSRSS | **`windowd`** | Windows, z-order, focus, input routing, frames, Alt+Tab, global hotkeys |
| Explorer (NT 4) | **`desktop`** | Background and icons, taskbar, Start menu, taskbar popups |
| `progman.exe` | **`progman`** | Launcher; an ordinary application |
| `taskmgr.exe` | **`taskmgr`** | Process list, End Task |
| SMSS → Winlogon → Userinit | **session launcher** | Starts the session, restarts the desktop if it dies |
| `\Windows\ApiPort` (CSRSS LPC port) | **kernel port object** | How a client finds a service |

## Responsibilities

### compositord: composition only

Owns:

- The GPU, the video mode and the display memory (it allocates it; today
  `windowd` does).
- The scene: an ordered list of surfaces with a rectangle, a position in the
  stack, visibility, opacity and an optional scale (for composited
  fullscreen).
- Composition: occlusion, damage tracking, the present timeline.
- The cursor, completely: hardware plane, software fallback, the cursor images.
  `windowd` only says which shape and where.
- Client frame traffic: clients submit frames to `compositord` and are
  retired by it. `windowd` is no longer woken by every frame of every app.

Never: input, window policy, focus, launching processes, reading files.

### windowd: windows only

Owns:

- Windows: creation, owned windows (dialogs), z-order, focus and activation,
  minimize/maximize/resize/drag, hit testing.
- The frames: `windowd` draws the non-client area (caption, borders, buttons)
  into its own surface per window, as USER painted `WM_NCPAINT`. Frames keep
  using SXCHROME; `windowd` still does not link SXGUI-C.
- Input: it reads the keyboard and pointer, coalesces and routes them. Routing
  needs geometry and focus, which is why input stays here and not in
  `compositord` (in NT, USER's raw input thread).
- The generic primitives listed under [Window roles](#window-roles),
  [Global hotkeys](#global-hotkeys) and
  [Window enumeration](#window-enumeration).
- Alt+Tab, Alt+F4 and F11 (composited fullscreen, which asks `compositord`
  for the mode change).
- Telling `compositord` what the scene is whenever geometry, stacking or
  visibility changes.

Never: a binary path, the size or position of a shell element, the taskbar
height, the wallpaper, a table of known applications, launching processes,
reading files.

### desktop: the desktop only

One process, several surfaces:

- **Background** (role `background`): wallpaper and desktop icons.
- **Taskbar** (role `panel`, bottom edge, reserving its strip).
- **Start menu, volume, clock and keyboard-layout popups** (role `popup`), all
  surfaces of the same process. The ownership mechanism of
  [OWNED_WINDOWS.md](./OWNED_WINDOWS.md) already lets one process hold several
  surfaces over a single event channel.

It replaces six processes (`shellui`, `taskbar`, `startmenu`, `volumepopup`,
`clockpopup`, `kbdlayoutpopup`). The popups open without spawning a process.
The launch flags leave the ABI, and the descriptor budget recovers what those
processes cost.

The wallpaper belongs here, not in a `wallpaperd` of its own. A process that
paints a bitmap which changes once a month would cost a channel and isolate
nothing worth isolating.

If `desktop` dies, `windowd` shows a solid colour background and the session
launcher restarts it (NT's `AutoRestartShell`).

### progman

Unchanged: an ordinary window that lists the installed programs. It launches
programs itself once launching leaves `windowd` (phase 3).

### The Task List is removed

NT 3.x's Task List (`taskman.exe`) was the switcher of a desktop without a
taskbar. With the taskbar back, everything it did has another owner:

| Task List did | Now |
|---|---|
| Switch to a window | Taskbar button, or Alt+Tab |
| Restore a minimized window | Taskbar button |
| End Task | `taskmgr` (Ctrl+Shift+Esc) |
| Ctrl+Esc | Opens the Start menu (NT 4 / Win95 behaviour) |
| Alt+Tab switcher UI | A small switcher drawn by `windowd`: icon and title of the candidate, as NT's Alt+Tab box |

The Task List was also the fallback for switching windows when the taskbar
failed to start. Afterwards Alt+Tab covers that, and the session launcher
restarts the desktop.

## Window roles

A role is a property of a window, requested when the window is created. It is
not a client slot in `windowd`, and it is not a launch flag.

| Role | Stacking | Behaviour |
|---|---|---|
| `background` | Below every window | Full screen, never activated |
| `panel` | Above normal windows, below a fullscreen app | Anchored to a screen edge; declares a reserved strip (the maximize area excludes it) |
| `popup` | Above panels | Anchored to a rectangle the client gives; frameless; optional *dismiss on outside click*; optional *exclusive group* (opening one closes the others in the group) |
| `normal` | The ordinary z-order | Frame, cascade placement, owned dialogs |

Rules:

- Only a client holding the shell privilege may create anything but `normal`
  windows. The privilege is granted when the connection is made (by
  `windowd` when it starts the desktop in phase 1, by the session launcher
  through the port afterwards), never claimed by the client.
- `windowd` keeps no per-role special cases in routing, reaping or stacking.
  One loop over windows, ordered by role and then by z-order.
- Sizes, anchors and groups come from the client. The numbers that today live
  in both `windowd.c` and the popup sources live only in `desktop`.
- A click outside a dismissable popup closes it **and keeps going** to whatever
  it landed on, as today. A click on the panel that owns the popup's toggle
  button is the panel's to interpret.

## Global hotkeys

A client may register a key combination (`RegisterHotKey`). `windowd` delivers
it to that client even when another window has focus.

- `desktop` registers Ctrl+Esc (Start menu) and Ctrl+Shift+Esc (launch
  `taskmgr`). `windowd` does not know what either means.
- Combinations `windowd` keeps for itself cannot be registered: Alt+Tab,
  Alt+F4, F11.
- Registering is a shell privilege at first. Whether ordinary applications
  may register hotkeys is an open question, not a default.

## Window enumeration

The window list section of `wm_shell_protocol.h` (seqlock, one shared section,
read-only for the client) stays: it is the right design. Two changes:

- Window ids become real window ids, not pids. A process can have several
  windows (`desktop` will; owned windows already carry a `window_id`).
- The request set grows from activate/minimize to what the taskbar needs, such
  as close for a button's context menu. Cascade and tile go here if a client
  ever needs them.

## Presentation comes from the client

Today `windowd` opens the `.sxe` of every binary it launches to read the title,
icon, accent and `window_flags`, and falls back to a per-path table. In NT the
title and the style are arguments of `CreateWindow`: the application says
them.

The client runtimes (`gfx_impl.inc`, `sxnwm.h`) read the process's own `.sxe`
and publish title, icon, accent and style in the surface header before the
first frame. `windowd` reads no files. The per-path table is deleted; a binary
without resources gets its basename and the generic icon.

`window_flags=fixed_size` keeps its rule ("holds no matter who launched the
program") because it is read from the program's own image, by the program's
own runtime.

## Native from the start

The kernel has a native NT-style API (see
[KERNEL_NATIVE_API.md](../kernel/KERNEL_NATIVE_API.md)), and POSIX is one
subsystem on top. The platform services move onto it:

- **`compositord`, `windowd` and the session launcher become native
  processes** (`EI_OSABI=0x53`). In NT, CSRSS and SMSS are native
  applications.
- **`desktop`, `progman` and applications are clients**, and may be POSIX or
  native. In NT, Explorer is a Win32 application, not a native one. Being a
  client of the window system is independent of the subsystem a program
  runs in.

### Rules that make that possible

- **Every protocol is defined in terms of kernel objects:** sections, events,
  ports and handles passed over them. Never POSIX constructs: no rendezvous by
  `fork` inheritance, no `poll`-only readiness, no byte-stream framing.
  The structs stay in the neutral `include/abi/` headers, as the v4 WM
  protocol already does.
- **The native object API is the floor every subsystem stands on.** Today a
  POSIX process that issues an `SXN_SYS_*` number gets `ENOSYS`. A POSIX
  client must be able to open a port, wait on events and map sections through
  the native object calls, as a Win32 process calls `ntdll`. That is a
  dispatcher change (POSIX falls through to the native object calls for
  numbers at `SXN_SYS_BASE` and above), and it does not weaken the isolation:
  the kernel stays indifferent to POSIX.
- **No shell vocabulary in `include/abi/`.** Window roles are generic; the
  words "taskbar", "start menu", "volume" or "clock" never appear in an ABI
  header.
- **Platform services do not live in the POSIX tree.** They move out of
  `subsystems/posix/userland/` into a tree of their own (for example
  `system/`), built by an in-tree native CMake target.

### What the native API lacks today

The SXN surface has 16 calls (info, log, exit, spawn, waitpid, events,
sections, views, a single-object wait and a raw framebuffer path). The
services need:

| Need | Used by | Today |
|---|---|---|
| Connectable named port (accept, connect, message, handle transfer) | everyone | missing; a new kernel object |
| Wait on several objects at once | `windowd`, `compositord` | only `SXN_SYS_WAIT_ONE` |
| Open a device and send it control requests (`gpu0`, `input0`, `mouse0`) | `compositord`, `windowd` | missing; `SXN_SYS_GFX_*` writes the framebuffer directly and would bypass `compositord` |
| Close and duplicate a handle | everyone | missing |
| Monotonic clock | `compositord`, `windowd` (frame stats) | missing |
| Process termination and query | session launcher, `windowd` (closing a main window ends its process) | missing (`waitpid` exists) |
| POSIX access to the native object calls | POSIX clients | `ENOSYS` today |
| A native C runtime: `crt0` that ends in `sxn_exit`, a heap over sections, string functions | everyone | header-only `sxn.h`; the stock `crt0` falls through to a POSIX exit |
| Files | `desktop`, `progman` | missing, and not needed by the services |

The graphics libraries are cheap to bring along. SXGFX (`gfx2d.c`) needs only
`malloc`, `free` and `realloc`. SXCHROME needs nothing. The fonts are compiled
in. SXGUI-C opens one file and reads the uptime. Files are the one need
that keeps `desktop` and `progman` on POSIX for now. For the services
that is the point of the design: once launching and `.sxe` reading leave
`windowd`, neither service opens a file.

## Phases

Each phase leaves a working desktop. The smoke and visual scenarios
(`windowd-smoke`, `taskbar-smoke`, `progman-smoke`, `sxgui-smoke`,
`tools/shoot.sh`) are the bar for each step.

### Phase 1: extract `desktop`, generic primitives in `windowd`

No kernel change; every process still runs on POSIX. The new protocol pieces
are built only on kernel objects, so they survive phase 3 unchanged.

1. Window roles, global hotkeys and the reserved strip in the WM protocol;
   `windowd` implements them generically.
2. `desktop` as one process, merging `shellui`, `taskbar`, `startmenu` and the
   three popups as surfaces with roles.
3. Removed from `windowd`: the per-popup slots and branches, the seven
   hard-coded paths except the session's own children, `WINDOWD_TASKBAR_HEIGHT`,
   the wallpaper (the fallback becomes a solid colour), the `shell_client`
   slot, the dead clock code. Removed from `include/abi/`: the four shell
   launch flags.

### Phase 2: remove the Task List

1. The Alt+Tab switcher UI in `windowd` (icon and title box).
2. Ctrl+Esc and Ctrl+Shift+Esc registered by `desktop` (Start menu,
   `taskmgr`).
3. The Task List deleted: its state, layout constants, layer and selftests.

Depends on the hotkeys of phase 1.

### Phase 3: native foundation, launching leaves `windowd`

1. **Kernel:** the port object, multi-object wait, device open and control,
   handle close and duplicate, monotonic clock, process termination, and
   POSIX access to the native object calls.
2. **Runtime:** the native C runtime, the in-tree native CMake target, SXGFX
   and SXCHROME built for native.
3. **`compositord` native.** The pilot: about 600 lines, no files, only the
   GPU, its IPC and a clock.
4. **Presentation from the client** (see above).
5. **Launching moves to the caller.** `desktop` and `progman` spawn
   programs; the child connects to `windowd` through its port. A native
   session launcher, started by `init`, starts `compositord`, `windowd`,
   `desktop` and `progman`, and restarts `desktop`. `windowd` drops `fork`,
   `dup2`, `exec`, the descriptor sweep and reaping.
6. **`windowd` native.**

### Phase 4: composition moves into `compositord`

The scene protocol, client surfaces mapped by `compositord`, submit and retire
against `compositord` directly, the cursor entirely in `compositord`, frames as
`windowd`-owned surfaces. Last because it is the largest change and the one the
work in [GRAPHICS_PERF.md](./GRAPHICS_PERF.md) is most sensitive to: measure
`blk_us`, `ipc_us` and damage before and after.

### Later, not required

`desktop` and `progman` on the native API, once it has files and SXGUI-C
builds for it. Not a goal of this plan: a client in any subsystem is a valid
client.

## Open questions

- **POSIX access to native calls:** fall through in the POSIX dispatcher, or
  explicit POSIX wrappers in the translation layer. Falling through matches NT
  and adds no surface to maintain; it needs a decision on which native calls
  are exposed (objects yes; `SXN_SYS_EXIT` would bypass POSIX process
  teardown).
- **Descriptor budget after ports:** per-client cost in `windowd` and in
  `compositord` has to be re-measured against the 64-handle table
  ([WM_SUBSYSTEM.md](./WM_SUBSYSTEM.md#descriptor-budget)) once frame traffic
  goes to `compositord`.
- **Hotkeys for ordinary applications:** allowed or shell-only.
- **Where the session launcher ends and `init` begins:** whether `init` keeps
  restarting the window system (today it restarts `windowd`) or delegates all
  of it to the session launcher.
