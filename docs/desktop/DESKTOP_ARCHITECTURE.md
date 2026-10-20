# Desktop architecture: compositord, windowd, desktop

> **Status: design accepted (2026-10-19). Phases 1 and 2 are built** (the
> `desktop` process, window roles, hotkeys, the Task List removed); phase 3 is
> under way (`compositord` and the session manager `sessiond` are native,
> clients connect to `windowd`'s port and whoever launches a program spawns
> it); phase 4 is not. It is the target for the graphics stack and the order for
> getting there.
> Where it disagrees with the "Target layering" of
> [WM_SUBSYSTEM.md](./WM_SUBSYSTEM.md), this document wins; WM_SUBSYSTEM.md
> stays as the record of how `windowd` was extracted from `desktop.c`.

## Why

The WM extraction left the right processes but not the right boundaries.
Several releases later, an audit of `compositord` and `windowd` found (the
shell knowledge listed here is what phase 1 removed):

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
| SMSS → Winlogon → Userinit | **`sessiond`** (the session manager) | Starts the session in order, grants the shell role, restarts the desktop if it dies |
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

If `desktop` dies, `windowd` shows a solid colour background and
[`sessiond`](#the-session-manager-sessiond) restarts the desktop (NT's
`AutoRestartShell`), giving up after three exits within ten seconds of
starting, so a crash at startup cannot become a spin loop.

Which windows the desktop has, and what each is:

| Id | Window | Role |
|---|---|---|
| 0 | Background (wallpaper) | `background`, granted at launch |
| 1 | Taskbar | `panel`, bottom edge, 28 px |
| 2 | Start menu | `popup`, dismissable |
| 3 | Volume, clock or keyboard-layout popup | `popup`, dismissable; the three share the tray corner, so one id |

The modules are `subsystems/posix/userland/desktop/desktop_*.c`, one per
window kind, behind a single loop in `desktop.c`.

### progman

Unchanged: an ordinary window that lists the installed programs. It launches
programs itself: `gfx_desktop_launch*` is a `spawn` in the caller, and the
program connects to `windowd` on its own.

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
failed to start. Afterwards Alt+Tab covers that, and `sessiond` restarts the
desktop.

## The session manager: sessiond

> **Status: built** (`subsystems/posix/userland/sessiond.c`, native).
> `session-smoke` kills the desktop and then `windowd` and checks the desktop
> and then the whole session come back, the desktop each time with the shell
> role.

NT splits starting a session in two: SMSS starts the subsystem servers (CSRSS,
where USER lived in 3.x) and Winlogon, and Winlogon runs Userinit, which starts
the shell and restarts it when it dies. SavanXP has no logon, so one process
does both jobs: **`sessiond`**. The name follows `windowd`, `compositord` and
`audiod`; it is a supervisor, not a server, and serves no requests.

### What it does

1. `init` starts it, where it used to start `windowd`, and restarts it if it
   dies. `init` keeps what is not part of the graphical session: `audiod`, and
   the fallback to `/bin/sh` when the session will not come up.
2. It starts `windowd`, then `desktop` and `progman`. It is their parent, so a
   blocking `waitpid(-1)` tells it when any of them ends; it needs no
   multi-object wait and no polling.
3. It restarts `desktop` when it dies, with the policy `windowd` has today:
   given up after three exits within ten seconds of starting. `progman` is an
   ordinary application and is not restarted: closing it is a user's choice.
4. When `windowd` dies, the session is over: it ends `desktop` and `progman`
   and starts the session again, with the same quick-exit limit; past it,
   `sessiond` exits with an error and `init` falls back to the text shell, as
   it does for `windowd` today.
5. Later: ending the session and shutting down (the Shut Down of Task Manager
   and the Start menu would ask `sessiond`, not kill processes themselves).

`compositord` stays `windowd`'s child for now. They are joined by inherited
descriptors (`compositor_protocol.h`) and `windowd` already restarts it when it
dies. Phase 4 rewrites that protocol anyway -- clients submit to `compositord`
directly, so it needs a port of its own -- and that is when `sessiond` starts
it, before `windowd`.

### Who is the shell: decided by parentage

The shell role (window roles, hotkeys, the window list) must not go to
whoever asks for it on the public port. It goes to the process **the session
manager started** that asks for it:

- The hello gets a flag, `SAVANXP_WM_HELLO_SHELL`. The desktop's runtime sets
  it; no other program does.
- `windowd` grants the role when the flag is set and the kernel says the
  client's parent is `windowd`'s own parent (`sessiond`) or `windowd` itself
  (its selftests start a desktop). The client pid comes from the port, which
  the client cannot forge, and the parent from the process table. One shell
  at a time: while a live process holds the role, a second request is refused.
- A program the desktop launches has the desktop as its parent, and an orphan
  is reparented to `init`, so neither can pass.

That needs no control channel between `sessiond` and `windowd`, no fixed
descriptors and no ordering: the desktop can connect before or after anything
`sessiond` would have said. It also removes the pid matching and the slot
reservation for the desktop from `windowd`. In NT the trust comes from who
created you as well, not from what the client claims.

A control channel becomes necessary only for something `sessiond` has to tell
`windowd` -- "the session is ending, close every window". When that exists it
is a connection to the same port with its own hello flag, granted under the
same parentage rule.

### Clients when the WM goes away

When `windowd` dies, every client's event pipe hangs up. The runtime treats
that as a close request (`gfx_should_close` returns 1), so an application ends
the way it ends when its window is closed, instead of drawing into a surface
nobody composes. `sessiond` ends what is left of the session after a grace
period, which needs the process termination call phase 3 already lists.

### Native

`sessiond` is native from the start: it opens no file and only starts and
watches processes. `SXN_SYS_SPAWN` starts binaries of any subsystem (the
loader goes by the binary's stamp) and clones the caller's handle table, which
for `sessiond` is stdio and nothing else. `SXN_SYS_WAITPID` already takes -1.
The one call it lacks is process termination, for ending the session; the
first version can restart the desktop and the session without it, since both
only follow a death.

### Order of work

1. Process termination in the native API (and `kill` for POSIX is already
   there for `windowd`'s close button). **Done:** `SXN_SYS_TERMINATE`.
2. The shell flag in the hello and the parentage rule in `windowd`; `desktop`
   asks for the role. The runtime treats a hung-up event pipe as a close
   request. **Done:** `SAVANXP_WM_HELLO_SHELL`, `gfx_open_shell`; waits for
   the WM look for the hang-up every 250 ms. `windowd --selftest` has the
   desktop launch Task Manager and checks the rule refuses its parentage.
3. `sessiond`, native, built in-tree with `NATIVE`; `init` starts it instead of
   `windowd`. `windowd` stops spawning `desktop` and `progman`, drops the
   desktop restart and its reservation; the selftests keep spawning their own
   apps and desktop. **Done**, except the desktop reservation: `windowd`
   still holds the desktop slot for the desktop its selftest spawns, so that
   one's death before connecting is noticed. `sessiond` waits for `windowd`'s
   port (probing it with a connection that never says hello) before starting
   the shell, and exits with status 3 when the session will not come up,
   which `init` reads as "fall back to the text shell".
4. Smokes: `windowd-smoke` (the selftest's desktop still gets the role), a new
   `session-smoke` that kills the desktop and checks it comes back and that the
   session comes back when `windowd` is killed, `taskbar-smoke`, and
   `shoot.sh --scenario startmenu`. **Done.**

Building it found a kernel bug: a new process copied its parent's subsystem
state and only then took the subsystem of its image, so a POSIX program
started by a native one (`windowd` by `sessiond`) ran with no POSIX state and
its `poll` failed. Spawning now resizes the state for the child's subsystem.

## Window roles

A role is a property of a window, requested when the window is created. It is
not a client slot in `windowd`, and it is not a launch flag.

| Role | Stacking | Behaviour |
|---|---|---|
| `background` | Below every window | Full screen, never activated |
| `panel` | Above normal windows, below a fullscreen app | Anchored to a screen edge; declares a reserved strip (the maximize area excludes it) |
| `popup` | Above panels | At the screen position the client gives, clamped to the display; frameless; never focused; optional *dismiss on outside click* |
| `normal` | The ordinary z-order | Frame, cascade placement, owned dialogs |

Rules:

- Only a client holding the shell privilege may create anything but `normal`
  windows. The privilege is granted when the connection is made, to a client
  that asks for it and was started by the session manager (see
  [Who is the shell](#who-is-the-shell-decided-by-parentage)), never on the
  client's word alone.
- `windowd` keeps no per-role special cases in routing, reaping or stacking.
  One loop over windows, ordered by role and then by z-order.
- Sizes and positions come from the client: the numbers that used to live in
  both `windowd.c` and the popup sources live only in `desktop`. Which popups
  exclude each other is the desktop's business too; `windowd` has no notion of
  a group.
- A press whose topmost window belongs to **another process** (or that lands on
  nothing) asks every dismissable popup to close, through the same shutdown flag
  a dialog's close button uses, **and keeps going** to whatever it landed on.
  A press on another window of the popup's own process does not dismiss it:
  the toggle button on the taskbar, but also the background, are the desktop's
  to interpret (it closes its popups on a press on the background).
- The fields are appended to `savanxp_wm_window_request` (`role`,
  `role_flags`, `x`, `y`); clients built before them still map their surface,
  since the header only grew at its end. The runtime call is
  `gfx_window_open_role`.

## Global hotkeys

A client may register a key combination (`RegisterHotKey`). `windowd` delivers
it to that client even when another window has focus.

- The request is `SAVANXP_WM_SHELL_REGISTER_HOTKEY` on the shell request pipe
  (shell protocol v4): a key and an exact Shift/Ctrl/Alt mask. The KEY_DOWN and
  its KEY_UP go to the desktop's main window, and the focused application sees
  neither.
- `desktop` registers Ctrl+Esc (Start menu) and Ctrl+Shift+Esc (launch
  `taskmgr`). `windowd` does not know what either means.
- Combinations `windowd` keeps for itself cannot be registered: Alt+Tab,
  Alt+F4, F11.
- Delivering a hotkey leaves composited fullscreen first: the shell's windows
  are not drawn over a fullscreen app (and are laid out for the native mode),
  so Ctrl+Esc would otherwise open a menu nobody can see.
- Registering is a shell privilege at first. Whether ordinary applications
  may register hotkeys is an open question, not a default.

## Window enumeration

The window list section of `wm_shell_protocol.h` (seqlock, one shared section,
read-only for the client) stays: it is the right design. Two changes:

- Window ids stay pids for now: an entry is a task, a task is an
  application's main window, and owned windows and the desktop's windows are
  never tasks. They become real window ids the day one process can own more
  than one task.
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

- **`compositord`, `windowd` and `sessiond` become native
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
| Connectable named port (accept, connect, message, handle transfer) | everyone | done (slice 15); the WM protocol v5 connects through it |
| Wait on several objects at once | `windowd`, `compositord` | done (slice 17), streams included |
| Open a device and send it control requests (`gpu0`, `input0`, `mouse0`) | `compositord`, `windowd` | done (slice 14) |
| Close and duplicate a handle | everyone | done (slice 14) |
| Monotonic clock | `compositord`, `windowd` (frame stats) | done (slice 14) |
| Process termination and query | `sessiond` (ending the session), `windowd` (closing a main window ends its process) | termination of a child done (slice 16); query missing |
| POSIX access to the native object calls | POSIX clients | done (slice 15) |
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

### Phase 1: extract `desktop`, generic primitives in `windowd` (done)

No kernel change; every process still runs on POSIX. The new protocol pieces
are built only on kernel objects, so they survive phase 3 unchanged.
`windowd --selftest` asserts the roles end to end against the real desktop
(panel geometry and strip, Start opening a popup, a press elsewhere closing
it, an application refused a role); `taskbar-smoke` asserts the result
pixel by pixel.

1. Window roles, global hotkeys and the reserved strip in the WM protocol;
   `windowd` implements them generically.
2. `desktop` as one process, merging `shellui`, `taskbar`, `startmenu` and the
   three popups as surfaces with roles.
3. Removed from `windowd`: the per-popup slots and branches, the seven
   hard-coded paths except the session's own children, `WINDOWD_TASKBAR_HEIGHT`,
   the wallpaper (the fallback becomes a solid colour), the `shell_client`
   slot, the dead clock code. Removed from `include/abi/`: the four shell
   launch flags.

### Phase 2: remove the Task List (done)

`windowd --selftest` covers the switcher (cycling, confirm on Alt release,
Esc cancelling, a minimized window restored through it) and the hotkeys end to
end against the real desktop; `shoot.sh --scenario startmenu` drives Ctrl+Esc
and Ctrl+Shift+Esc with real keys.

1. The Alt+Tab switcher UI in `windowd` (icon and title box).
2. Ctrl+Esc and Ctrl+Shift+Esc registered by `desktop` (Start menu,
   `taskmgr`).
3. The Task List deleted: its state, layout constants, layer and selftests.

Depends on the hotkeys of phase 1.

### Phase 3: native foundation, launching leaves `windowd`

1. **Kernel:** the port object, multi-object wait, device open and control,
   handle close and duplicate, monotonic clock, process termination, and
   POSIX access to the native object calls. **Done so far:** handle close and
   duplicate, pipe and device read/write, device open and control, the clock
   (`docs/kernel/KERNEL_NATIVE_API.md`, slice 14). The port and POSIX access
   to the native object calls followed (slice 15), then a parent ending its
   child (slice 16) and waiting on several handles, streams included
   (slice 17). Still to do: querying a process (for `windowd`).
2. **Runtime:** the native C runtime, the in-tree native CMake target, SXGFX
   and SXCHROME built for native.
3. **`compositord` native.** The pilot: about 600 lines, no files, only the
   GPU, its IPC and a clock. **Done:** it speaks only SXN (the GPU as a device
   with control codes, its pipes through native read/write, the native clock),
   built in-tree with `NATIVE`. It needed no native C runtime: a static
   program links the usual runtime, whose call-free parts work unchanged.
4. **Presentation from the client** (see above). **Done:** the window runtime
   reads the program's own `.sxe` through `argv[0]` (kept by `crt0`) and
   publishes `savanxp_wm_presentation`; windowd starts every window with its
   file name and adopts what it publishes. The per-path table is gone. Native
   clients have no file access to read their own `.sxe` and so show their
   file name.
5. **Launching moves to the caller.** `desktop` and `progman` spawn
   programs; the child connects to `windowd` through its port. A native
   session manager, `sessiond`, started by `init`, starts `windowd`, `desktop`
   and `progman` (and `compositord` from phase 4), and restarts `desktop`. `windowd` drops `fork`,
   `dup2`, `exec`, the descriptor sweep and reaping. **Done so far:** WM
   protocol v5 (`savanxp/wm_protocol.h`, [WM_SUBSYSTEM.md](./WM_SUBSYSTEM.md#descriptor-budget)):
   a client connects to the port `windowd`, says hello with the launch flags
   its own `.sxe` declares and whether it wants an input ring, and gets its
   handles with the welcome. `gfx_desktop_launch*` is a `spawn` in the caller,
   which reaps its children; `windowd` no longer forks, `dup2`s, sweeps
   descriptors, sniffs `EI_OSABI` or keeps a launch queue, and labels a window
   it did not start by its process name. Then the session manager,
   [`sessiond`](#the-session-manager-sessiond): `windowd` starts nothing of
   the session any more and restarts nothing; `compositord` is still its
   child until phase 4. **Step 5 is done.**
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

- **Session teardown can be slow enough to time out `windowd-smoke`.** After
  the capacity test the selftest kills twelve applications and the desktop and
  shuts `compositord` down. Sampling the guest's RIP over QMP while it stalled
  found two costs. The first was the kernel's text console, which takes the
  screen back when `compositord` releases the GPU and used to repaint the whole
  framebuffer on every scroll (fixed: it now repaints only the glyph pixels that
  changed, and tick loss in that window fell from ~87% to ~10%). The rest is
  not the guest's work: timing each part of the tick handler showed every part
  -- even `object::poll_timers`, a 128-entry loop with no I/O, 4 us at rest --
  stretching 15-70x in the slow windows, which only happens when the vCPU is not
  being run. It is host-side (KVM contention; a VirtualBox VM running on the
  same host was one instance), so `compositord`'s exit can still take minutes
  on a loaded host. It also showed far more often with the pattern wallpaper
  (`/disk/desktop.cfg` = `2`) than with the default one, measured but not
  explained. `windowd_compositor_close` kills a daemon that does not confirm its shutdown, which covers the timeout
  case. Reproduce: `./build.sh smoke windowd-smoke` a few times; a hang after
  `DESKTOP SMOKE capacity` is the symptom.

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
- **Where the session manager ends and `init` begins:** decided --
  `sessiond` owns the graphical session, `init` keeps `audiod` and the text
  fallback (see [The session manager](#the-session-manager-sessiond)).
