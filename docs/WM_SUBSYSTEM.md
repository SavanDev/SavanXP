# Extracting the window manager into a subsystem (NT 3.5 model)

> **Status: Phase A COMPLETE (A1 + A2 + A3), on master.**
>
> What was built:
>
> - The WM is `windowd.c` + the `windowd_*` modules (`_session`, `_render`,
>   `_layout`, `_compositor_client`, `_appinfo`). Binary `/bin/windowd`.
> - The shell is a set of client processes: `shellui` draws the background,
>   `progman` is the launcher, with its registry in `/disk/progman.ini`.
> - The WM↔client contract is `savanxp/wm_protocol.h` (fds 3..10).
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
contract onto every client:

| fd | contents                        | direction        |
|----|---------------------------------|------------------|
| 3  | shared-memory surface (header + dirty-rect batches + pixels) | WM→app (map RW) |
| 4  | keyboard input                  | WM→app           |
| 5  | mouse input                     | WM→app           |
| 6  | *submit* event (frame ready)    | app→WM           |
| 7  | *retire* event (frame released) | WM→app           |
| 8  | *shutdown* event                | WM→app           |
| 9  | launch pipe (app asks to launch another app) | app→WM |
| 10 | cursor hint pipe (cursor shape) | app→WM           |

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
content size at startup over fd 11 (size hint), which the WM applies once and
clamps. Input over fds 4/5, launch over fd 9, cursor hint over fd 10.

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

  Design note for what comes next: `windowd` holds **nine** descriptors per
  client against the 64-per-process limit, so it does not reach the 12 clients
  `WINDOWD_MAX_OVERLAY_CLIENTS` declares — it tops out near six. Any new
  protocol channel has to be weighed against that ceiling; it is the reason the
  clipboard became a kernel device node (`/dev/clipboard`) instead of a WM
  service.
