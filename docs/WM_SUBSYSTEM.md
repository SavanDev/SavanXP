# Extracting the window manager into a subsystem (NT 3.5 model)

> **Status: Phase A COMPLETE (A1 + A2 + A3), on master.**
>
> What was built:
>
> - The WM is `windowd.c` + the `windowd_*` modules (`_session`, `_render`,
>   `_layout`, `_compositor_client`, `_appinfo`). Binary `/bin/windowd`.
> - The shell is a set of client processes: `shellui` draws the background,
>   `progman` is the launcher: it lists every installed program that declares
>   a category in its `.sxmeta` (see
>   [SXE_FORMAT.md](SXE_FORMAT.md#all-programs-the-catalog-discovers-itself)),
>   with `/disk/progman.ini` on top as the user's arrangement.
> - The WM↔client contract is `savanxp/wm_protocol.h` (protocol v4, fds 3..6;
>   see [Descriptor budget](#descriptor-budget)).
> - The taskbar is replaced by the Task List (Ctrl+Esc), which is WM UI.
> - Modules shared with clients (`desktop_icons`, `desktop_wallpaper`) keep
>   their names on purpose: they do not belong to the WM.
>
> **Phase B** is next (an MDI primitive in sxgui for Progman's groups).
>
> Of **Phase C** (the maturing work that motivated all of this), **edge
> resizing** and **Alt-Tab** are done; the latter uses the Task List as the
> switcher, with the cycle confirmed on releasing Alt. Still open:
> **focus/activation** (the mechanics exist — `raise_overlay`,
> `active_overlay_slot` — what is missing is making the active window look
> different from the rest) and **repaint correctness**.
>
> **From here down, this document is the record of the original plan** — with
> the decisions and findings exactly as they were made. It is written in the
> future tense and names files by their old names (`desktop.c`, `desktop_*`);
> it is kept that way because the value is the reasoning, not the status.
>
> **Crux decision, resolved:** the **shell draws the background** (pure NT).
> The shell client owns a full-screen background surface; `windowd` only
> composes it at the bottom of the z-order and knows nothing about wallpaper.

## Motivation

Today `subsystems/posix/userland/desktop.c` is **two things welded together**:

1. **The window manager**: owner of the connection to `compositord`, of
   `struct desktop_session` (z-order in `overlay_order[]`, focus, drag,
   minimize/maximize/fullscreen), of input routing to the active client, of the
   frame cycle (submit/retire/compose) and of the software composition pass
   that puts every client surface into the display buffer.
2. **The shell chrome**: taskbar, start menu, desktop icons and wallpaper —
   drawn *directly* into the display buffer through `desktop_render.c`, with
   their input handled inline in the `for(;;)` loop of `main()`.

This is exactly the inverse of the NT 3.5 layering, where the window manager
(USER) lived in user mode inside **CSRSS** (a separate subsystem) and Program
Manager (`progman.exe`) was **just another client** — the shell — never the WM.
We want that layering because the goal is to *mature the compositor and the
usage experience*: that work (resize, focus/activation, Alt-Tab, repaint
correctness) is WM work, and it is far cleaner against a dedicated server than
against a file that also draws the start menu.

## Target layering

```
  compositord          (owns the GPU, a single display surface; UNCHANGED)
      ▲
      │ compositor_protocol.h  (fds 3/4/5: request/reply/display section)
      │
  windowd  (WM server)  ← new process = desktop.c MINUS the chrome
      ▲
      │ WM↔client protocol (fds 3..10, see below)
      │
   ┌──┴───────────────┬──────────────┐
 shell-client       app (aboutapp,  app (...)
 (progman /         filesapp, ...)
  desktop chrome)
```

- **`compositord`** — unchanged. Owner of the GPU, its only privileged client
  being `windowd`.
- **`windowd`** — the current `desktop.c` process, **minus** the chrome
  rendering. It keeps: the connection to compositord, `desktop_session`,
  `overlay_clients[]` + `overlay_order[]`, input routing, the frame cycle, the
  generic composition pass, `start_client_process`, reaping, launch relay.
- **shell-client** — the chrome (taskbar / start menu / icons / wallpaper),
  running as a **client** of `windowd` over the **same** `gfx_*` protocol any
  app uses, plus a privileged extension (see the crux).
- **apps** — clients, unchanged.

## The WM↔client protocol already exists (implicitly)

`start_client_process()` (desktop.c:1265) does a `fork` + `dup2` of a fixed fd
contract onto every client (this is the v3 layout the plan started from; v4
replaced it, see [Descriptor budget](#descriptor-budget)):

| fd | contents                        | direction        |
|----|---------------------------------|------------------|
| 3  | shared-memory surface (header + dirty-rect batches + pixels) | WM→app (map RW) |
| 4  | keyboard input                  | WM→app           |
| 5  | mouse input (position, buttons, wheel) | WM→app    |
| 6  | *submit* event (frame ready)    | app→WM           |
| 7  | *retire* event (frame released) | WM→app           |
| 8  | *shutdown* event                | WM→app           |
| 9  | launch pipe (app asks to launch another app) | app→WM |
| 10 | cursor hint pipe (cursor shape) | app→WM           |

### The pointer channel carries a wheel, and it accumulates

`savanxp_gui_pointer_event` (on the event channel) has a `wheel` field alongside `x`, `y` and
`buttons`: signed ticks since the previous event, positive away from the user,
same sign as evdev's `REL_WHEEL`. Unlike the other three it is **not a state** —
it is an increment that is zero in most events.

That difference is a rule for anyone touching the input path: **every stage must
sum `wheel`, never overwrite it.** A dropped motion delta corrects itself,
because the cursor is an absolute position the next event repositions; a dropped
tick is scroll that never happens. The two stages that merge events —
`enqueue_mouse_event()` when the kernel queue overflows, and
`coalesce_mouse_events()` in the WM — both fold the field instead of keeping the
newest one, and `windowd --selftest` asserts the WM half. The client runtimes
(`gfx_impl.inc`, `sx_gui.c`) are a third stage: when their pointer stash is
full they fold a new event into the last one, summing the wheel the same way.

The wheel goes to the client **under the cursor**, not the focused one, which
falls out of the existing routing for free. The WM has no wheel behavior of its
own. Neither does the middle button: it travels in the `buttons` mask like any
other, and no WM interaction keys off it.

The **client half** of this protocol is already factored out as a library in
`subsystems/posix/sdk/v1/runtime/gfx_impl.inc` (behind
`gfx_open/acquire/present/poll_event` + `sxgui_app_*`); apps do **not**
hardcode fd numbers. The **server half** is the only part embedded in
desktop.c. Extracting the WM = moving the server half into `windowd` intact,
and turning the shell into a client of it.

## Crux decision: root window + routing input to the shell

The current `main()` fuses the WM dispatch and the chrome input into a single
switch. Splitting the shell into another process leaves **one** genuinely new
thing to design: how input reaches the shell.

- **SUPER** (open the start menu) must reach the shell *even when an app has
  focus* → the WM needs **global hotkeys** delivered to the shell client.
- **Clicks outside every app window** (wallpaper, taskbar) must route to the
  shell → the WM treats the shell as the **root/background window** (the bottom
  of the z-order) plus a **reserved taskbar strip**.

### Recommended option (for Phase A)

Register the shell as a client with a **privileged role** (`ROLE_SHELL`):

1. Its surface is the **background/root window**: full-screen, always at the
   bottom of the z-order. It draws the wallpaper + icons there. This replaces
   the direct wallpaper drawing the desktop process does today.
2. The WM delivers to it: (a) every click that does **not** land on an app
   window (background + taskbar), and (b) a small set of **global hotkeys**
   (SUPER at minimum) over the normal input pipe (fd 4), tagged so the shell can
   tell "global" apart from "I have focus".
3. In Phase A the **taskbar** can stay as a strip the WM reserves from the work
   area (the same way `desktop_layout.c` today clips the menu away from the
   taskbar), with its input routed to the shell.

This keeps the observable behavior identical (wallpaper, menu and taskbar look
and respond the same), only now the chrome is drawn from a client process.
`F11` (composited fullscreen) and routing to the active client **stay in the
WM** — they are WM concerns, not shell concerns.

## Execution plan (Phase A: behavior-preserving)

A two-hop approach, to de-risk cutting a boot-critical process:

1. **A1 — In-process boundary (= input arbitration refactor).** The `main()`
   dispatch is today an interleaved switch where WM and shell mix inside the
   same event, with shared locals (`menu_open`, `selected_shortcut`,
   `context_menu`, `confirm_action`, `welcome_visible`) and precedence implied
   by the order of the switch. A1 rewrites it as explicit arbitration:

   - **`wm_handle_key` / `wm_handle_pointer`** — first turn. Consumes: F11
     (global hotkey), window drag, hit on a window → `route_*` to the active
     client, activate/raise. Returns *consumed/not consumed*.
   - **`shell_handle_key` / `shell_handle_pointer`** — second turn, only if the
     WM did not consume. Consumes: start menu, context menu, shortcuts,
     taskbar, power, welcome. All chrome state (`menu_open`, etc.) moves into a
     `struct shell_state` private to the shell module.

   That WM→shell precedence *is* the process boundary of A2 (the WM decides:
   does it hit a window? is it a global hotkey? otherwise → to the shell). It
   is kept inside a single process.

   **Finding while implementing (keyboard vs. mouse):** the keyboard bisects
   cleanly (`shell_notify_key` → `wm_handle_key` → `shell_handle_key`) because
   each key is consumed by exactly one owner. The **mouse does not**: within a
   single event the cursor/hover update (WM) runs first because everything
   depends on it, the shell's modals (confirm dialog, context menu) consume
   with `continue`, and left-click dispatch is one if/else chain that mixes
   chrome (start/power/menu/taskbar/shortcut) with window hit testing (WM),
   with precedence by ordering. Forcing a two-way split there reorders and
   risks the behavior. Effective decomposition of the mouse in A1:
   - **Cleanly extractable now:** the two modal blocks →
     `shell_pointer_handle_confirm` / `shell_pointer_handle_context_menu`.
     These are the "the shell grabbed the input" cases, which A2 forwards whole
     to the shell client. They return consumed; `main()` does `last_buttons` +
     `continue`.
   - **Bisected in A2, not A1:** the mixed left-click chain (chrome vs. window
     hit) stays inline. A2's process boundary forces it naturally (the WM
     decides forward-to-shell vs. handle-the-window) and there it is testable
     with the shell already a separate client. Verify with `desktop --selftest`
     / `build.ps1 desktop-smoke` (headless compositor) + QMP mouse driving.
     Zero behavior change.

   **Chrome state migrating to `struct shell_state`:** `menu_open`,
   `selected_index`, `selected_shortcut`, `context_menu`, `confirm_action`,
   `welcome_visible`/`welcome_until_ms`, `last_shortcut_click(_ms)`. **Staying
   in the WM:** `drag_overlay_slot`/offsets, `cursor_x/y`, `last_buttons`, all
   of `session`.

   **✅ Result (A1 DONE):** a `desktop_shell.h/.c` module with `struct
   shell_state` + `shell_state_init()`; keyboard as the arbitration
   `shell_notify_key`→`wm_handle_key`→`shell_handle_key`; pointer modals in
   `shell_pointer_handle_confirm`/`_context_menu`; the per-event pointer body
   in `handle_pointer_event()`; rendering with `shell_state` +
   `paint_layer`→`wm_paint_layer` (client/cursor) / `shell_paint_layer`
   (chrome). `main()` ended at ~247 lines (601 before). Every cut verified with
   `desktop-smoke` (`DESKTOP SMOKE PASS`). The mixed left-click chain and
   `shell_paint_layer` are the pieces A2 moves to the shell client.

2. **A2 — Lifting the shell into a process.** Turn the `shell_*` layer into a
   separate client (`shell-client`) that talks to `windowd` over the WM↔client
   protocol + the `ROLE_SHELL` extension. The wallpaper becomes the shell's
   background surface. `windowd` = what is left. Verify with the smoke + QMP
   mouse driving (tray / context menu / wallpapers).
3. **A3 — Rename/structure.** `desktop.c` → `windowd.c` (or its own
   subsystem), and document the WM↔client protocol as an explicit public
   header.

## A2 — detailed design (the z-order knot)

**Constraint of the current protocol:** each client receives ONE surface (fd 3,
`gfx_open_client` in `gfx_impl.inc`), placed by the WM at
`window_x/y/width/height`. The WM sizes it; the client may *suggest* its
content size at startup (size hint, in the surface header), which the WM
applies once and clamps. Input over the event channel (fd 4); launch requests
and the cursor hint also travel in the header.

**The knot:** the shell chrome lives at two z-order levels that are
incompatible with a single surface:

- **wallpaper + desktop icons** → *below* every window.
- **taskbar + start menu + context menu + confirm** → *above* every window
  (today they are `TASKBAR`/`MENU`/... layers on top of `CLIENT` in
  `build_layers`).

A single-surface client cannot be both background and foreground.

**Extra coupling:** `draw_taskbar` reads the WM's window list
(`desktop_taskbar_button_client` → `overlay_clients`, active/minimized). In A2
the WM would have to send that list to the shell (WM→shell protocol:
window-list updates) so it can draw the buttons.

**Strategic angle:** the Win95 chrome (taskbar + start menu) is temporary —
Phase B replaces it with Program Manager (NT 3.5, no taskbar). Lifting it
faithfully with a multi-surface protocol is work that Phase B throws away.

### Surface model options

- **A) Multi-surface, faithful to Win95.** The shell creates 2 surfaces with
  z-roles: background (full-screen, bottom) + overlay (full-screen, top,
  alpha). A bounded protocol extension (a `ROLE_SHELL` client with two surfaces
  at fixed z-roles) + window-list updates for the taskbar. Faithful to the
  current chrome; part of the work is thrown away by Phase B.
- **B) Background + starting the NT 3.5 pivot (recommended).** The shell owns
  only the **background** surface (wallpaper + icons, at the bottom). The
  launcher stops being taskbar/start menu and becomes a **normal window**
  (proto-Progman), a top-level like any app — no special z-role, no
  multi-surface. It brings forward a minimal piece of Phase B, avoids the
  multi-surface protocol, and moves toward the goal. Minor open item: the
  desktop context menu (floating above windows) is deferred or handled as a
  transient top-level.
- **C) Protocol design only.** Write the full spec of the WM↔shell protocol
  (ROLE_SHELL, surfaces/z-roles, window list, input routing) and decide the
  model before touching code.

**Decided: option B.** Progress: **A2.1 DONE** (shellui draws the wallpaper as
a client) and **A2.2 DONE** (windowd launches shellui at boot as
`background_client` and composes its surface as the background layer; it falls
back to a wallpaper drawn by windowd if the client is not ready; verified
end to end in the `desktop-smoke` soak). Still to come: A2.3 (icons + input to
the shell), A2.4 (retire the Win95 chrome, proto-Progman launcher), A2.5
(cleanup).

## Following phases (outside Phase A)

- **Phase B** — `progman`: an alternative shell client in the Program Manager
  style (MDI groups, File/Options/Window/Help menu bar). Trivial once `windowd`
  does not know who the shell is. It requires a new MDI primitive in sxgui
  (child window with a title bar, drag clamped to the client area, minimize to
  an icon) — which does not exist today.
- **Phase C** — maturing the WM against the clean server. **Done**: edge
  resizing, and Alt-Tab over the Task List (while Alt is held, each Tab moves
  the selection; releasing it confirms). **Open**: focus/activation between
  windows — the mechanics are there, what is missing is showing which one is
  active — and repaint correctness.

  Design note for what comes next: every per-client channel is paid for in
  `windowd`'s descriptor table, see [Descriptor budget](#descriptor-budget).

## Descriptor budget

`windowd` is one process, and every channel it keeps to a client is a
descriptor in **its** table: 64 per process (`process::kMaxFileHandles`). Every
pipe is also one of the 64 pipes of the **whole system** (`kMaxPipeCount` in
`kernel/process.cpp`). A window is only as cheap as the channels it needs, and
that — not `WINDOWD_MAX_OVERLAY_CLIENTS` — is what decides how many fit.

Protocol v3 cost nine descriptors and five pipes per client (section, keyboard
and pointer pipes, submit/retire/shutdown events, launch/cursor/size-hint
pipes). With the background client and the taskbar that left room for about
four application windows, and the pipes ran out before twelve anyway.

Protocol v4 (`savanxp/wm_protocol.h`) costs **two descriptors and one pipe**:

| fd | v4 channel | what `windowd` keeps |
|----|------------|----------------------|
| 3  | surface section: header, batches, pixels, **and the client's requests** (`savanxp_wm_client_requests`) | nothing — closed after `fork`, the mapping keeps it alive |
| 4  | event pipe: `savanxp_wm_event` records, keyboard and pointer | the write end |
| 5  | wake event: composed/retired advanced, or shutdown requested | the event |
| 6  | submit event, **one for the whole session** | one descriptor in total |
| 7, 8 | taskbar only: window list section, shell request pipe | the pipe's read end |

With every slot full — twelve windows plus background, taskbar and keyboard
popup — `windowd --selftest` measures 40 descriptors, and asserts at least 8
remain free for the terminal client and a launch in progress.

The rules that keep it there:

- **A window is not a channel.** A process's owned windows (dialogs) reuse its
  event pipe and wake event through duplicates, and their surfaces arrive by
  handle passing on that pipe; see [OWNED_WINDOWS.md](OWNED_WINDOWS.md).
- **A new per-client channel is a descriptor times every window.** Before
  adding one, see whether it fits in the surface header (state, or a small
  queue with a single producer) or in a device node the client opens and closes
  (that is why the clipboard is `/dev/clipboard`).
- **Client→WM data goes in the header.** The cursor shape is plain state; the
  size hint is a seqlock (odd sequence = being written); launches are a ring
  where the client writes the entry *before* publishing `launch_head`. The WM
  copies each value before validating it and keeps its own `launch_tail`: the
  header is memory another process writes at any time.
- **One wake event serves several conditions**, so every waiter resets it,
  re-reads the header and only then sleeps. Shutdown is a header flag
  (`SAVANXP_GPU_CLIENT_SURFACE_FLAG_SHUTDOWN`) set before the wake, never the
  other way round. The shared submit event is reset by the WM *before* it
  services every client, so one client resetting it costs the others at most
  one 16 ms loop timeout.
- **Event records are 32 bytes and always written and read whole.** The kernel
  pipe holds 8 KiB, an exact multiple, so free space is always a whole number
  of records and a non-blocking write never lands half a record. Keyboard and
  pointer share the pipe, so the runtime drains both kinds even when the app
  asks for one, stashing the other: a keyboard-only app that left pointer
  records behind would fill the pipe and lose keys. The flip side: an app that
  `poll`s `input_fd` may wake for a pointer record, so `gfx_poll_event`
  returning 0 after a readable `poll` is normal, not end of input. The desktop
  Shell treated it as end of input and closed as soon as the mouse crossed it.
- **A child closes everything above the protocol before `exec`.** `fork`
  copies `windowd`'s whole table and `exec` closes nothing; without the sweep
  an app launched into a full session started with dozens of foreign
  descriptors and little room to open files.

A v3 client started by a v4 WM fails in `gfx_open` (the header version does
not match) instead of talking to channels that no longer exist. External apps
built against the SDK, such as `doomgeneric`, must be rebuilt.

## Fixed-size windows

Some programs have a layout with nowhere to go. A board of cells, a calculator
keypad: made bigger they gain no room, only an empty margin around content that
stays the size it always was. Before this existed the WM had no way to know,
so every window was resizable and those programs could only mitigate it — the
Minesweeper board centres itself in whatever window it is given, which reads as
a choice rather than a bug, and is still a workaround.

A program declares itself fixed with `SAVANXP_WM_WINDOW_STYLE_FIXED_SIZE`, via
`window_flags=fixed_size` in its `.sxres`
([SXE_FORMAT.md](SXE_FORMAT.md#window_flags-is-not-launch_flags)). The WM reads
it from the binary when it creates the window, so it holds no matter who
launched the program.

**One flag, not two.** Win32 separates `WS_THICKFRAME` from `WS_MAXIMIZEBOX`
and so can express "resizable but not maximizable". Nothing here needs that
combination, and two flags would mean two gates in four places, so fixed means
both: the edges do not grab, and maximize is off.

**The maximize button is disabled, not removed.** Removing it would leave a gap
between minimize and close — the title bar layout is anchored to the right and
counts three buttons — and a title bar whose button count varies per app reads
as inconsistent chrome rather than as a property of the window. So it stays,
greyed with the same etched relief the toolkit gives a disabled control
(`sxchrome_draw_glyph_disabled`, see
[SYSTEM_LAYERING.md](SYSTEM_LAYERING.md#the-two-layers)).

**What it forbids is the *user* resizing, not resizing.** The program keeps
asking for its own size with `gfx_request_content_size`, and in a fixed window
the WM honours **every** hint instead of only the first. The reason that channel
is one-shot is that the WM must not let an app fight the user over geometry;
where the user has no geometry to defend, the reason does not apply. That is
what lets Minesweeper switch from Beginner to Expert and have the window follow,
instead of staying at the old size with the new board centred inside it.

Repeat hints resize in place: only the first one re-centres the window on its
cascade slot, because after that the user has moved it and re-centring would
teleport the window out from under the cursor.
