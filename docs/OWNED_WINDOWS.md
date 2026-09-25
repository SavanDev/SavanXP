# Owned windows

A process has **one connection** to `windowd` and can have **several windows**:
its main window and up to `SAVANXP_WM_MAX_OWNED_WINDOWS` windows *owned* by it.
Today the only user is the toolkit's modal dialog: under `sxgui_app_run`, a
`sxgui_dialog_begin` gets a real top-level window, framed and titled by the WM,
instead of being painted inside the app's own surface.

## Why

A dialog painted inside the app's surface has to fit inside the app. Minesweeper
at Beginner is about 165 px wide; its About dialog is 320. The toolkit centred
it and clamped it to `x = 0`, and half of it was cut off. Best Times had the
same problem, and it opens by itself when a record is beaten at Beginner.

There is no cheap fix. A 150 px About has no room for its text, and growing the
app's window while a dialog is open looks broken. The dialog has to be a window
of its own.

## The model: NT's split of responsibilities

The design copies how NT 3.5 split the work between USER (in CSRSS) and a
client, not how it implemented it:

- **A window is an object of the window manager.** The process holds an id, not
  a channel. Creating a window is a *request* over the connection the process
  already has, so the number of windows does not depend on the number of
  channels.
- **An owned window stays above its owner**, is minimized with it, and is not a
  task of its own.
- **Modality belongs to the client.** `DialogBox` disables the owner and runs
  its own loop; the WM only has to know "owner" and "disabled". `sxgui` already
  handled modality in the client (`ctx->modal`), so it keeps doing so.

What was *not* copied: NT 3.5 drew server-side (GDI inside CSRSS), which was
slow enough that NT 4 moved USER and GDI into the kernel. SavanXP composes
per-window pixel buffers, closer to Vista's DWM — where the surfaces are also
created by the system, not the app. `windowd` creates the section.

### Rejected: a process per dialog

It looks cheap, because `windowd` already knows how to launch processes and
hand them channels at `fork`. But:

- A dialog shares state with its app (Best Times' Reset, Notepad's Yes/No/Cancel,
  the path an Open dialog returns). Two client processes have no channel between
  them, and the launcher never sees the child's exit code — `windowd` reaps it.
- Each process takes one of the 12 window slots plus two descriptors and one of
  the 64 system pipes.
- The WM still needs the owner relationship for z-order and disabling.

## The transport: handle passing over a pipe

An owned window needs a surface section in a process that is already running.
Descriptors used to reach a client only at `fork`. Two NT-shaped primitives
could fill the gap — `DuplicateHandle` into a target process, or
`NtMapViewOfSection` with a process handle — but both need a *process handle
with rights*, and the kernel identifies processes by pid (`object::Type::process`
exists and nothing uses it). A `duplicate_handle(pid, …)` would require
inventing a rule for who may inject handles into whom.

Unix answers that differently: **send the descriptor over a channel both sides
already share**, and let the receiver take it. The capability is holding the
channel. System V did it over pipes with `ioctl(I_SENDFD / I_RECVFD)`, without
sockets, and that is the shape used here:

- `SAVANXP_SYS_PIPE_SEND_HANDLE (pipe write end, handle)` queues a *reference*
  to the object in the pipe, with the sender's granted access.
- `SAVANXP_SYS_PIPE_RECEIVE_HANDLE (pipe read end)` pops it as a new descriptor.
  It never blocks: an empty queue is `EAGAIN`.

Rules the kernel enforces (`kernel/process.cpp`):

- **Only non-I/O objects travel**: sections, events, semaphores, timers. A
  shared `IoObject` would drag its file offset along, and for another pipe its
  reader and writer counts.
- **The queue is small** (`kPipeHandleQueueCapacity`, 4) and separate from the
  byte stream. The pipe does not correlate handles with bytes; the protocol on
  top announces each handle with its own record.
- **Nothing is lost in transit.** When the last reader goes away the queue is
  released, so an unreceived section does not stay pinned in the 64-entry
  global section table.

Also rejected: **named sections** (MIT-SHM's original global `shmid`). They
avoid passing descriptors but let anyone who guesses the name attach. MIT-SHM
later added fd passing precisely to close that hole.

## The protocol

Additive to v4; see `savanxp/wm_protocol.h` and `savanxp_wm_client_requests`
in `savanxp/syscall.h`.

1. The client picks the id (1..`SAVANXP_WM_MAX_OWNED_WINDOWS`), like an X11 XID
   or a Wayland `new_id`, and writes a `savanxp_wm_window_request`
   (`OPEN`, id, client-area width and height, title) into its **main** window's
   header under a seqlock (`window_sequence` odd while writing), then signals
   the submit event.
2. `windowd` copies and validates the request, creates the section, queues it
   on the client's **event pipe** with `pipe_send_handle`, and only then writes
   `window_reply_status` and `window_reply_sequence` and signals the wake event.
3. The client sees the reply, receives the handle, maps it, and duplicates its
   own event, submit and wake descriptors for the new `savanxp_gfx_context`.
4. Events for that window travel on the same pipe with
   `savanxp_wm_event.window_id` set. The runtime stashes records per window id,
   so reading the main window's keys never swallows the dialog's.
5. `CLOSE` goes through the same slot. Closing a window that does not exist is
   not an error.

The wait is bounded (`gfx_window_open`, 1 s). On timeout the client publishes a
`CLOSE` for that id, and the next `OPEN` first drains any stale handle from the
queue, so a late reply can never hand a new window the old surface.

## Window manager rules

An owned window lives in an ordinary overlay slot with `owner_slot` set and the
owner's pid. The rules are all expressed as "the family" — owner plus owned:

- **Z-order:** raising any member raises the owner and then its owned windows,
  so a click on the owner can never cover its own dialog.
- **Disabled owner:** while it has an owned window, the owner gets no pointer
  input. A click on it only raises the family and activates the dialog; hover,
  frame buttons and title dragging are ignored, and it shows the arrow cursor.
- **Close button and Alt+F4:** the dialog's X does not destroy it, and neither
  does Alt+F4 while it is the active window (both go through
  `close_overlay_window`). They set `SAVANXP_GPU_CLIENT_SURFACE_FLAG_SHUTDOWN` on
  the dialog's header; the process treats it as Cancel (like ESC) and sends
  `CLOSE`. The process decides what cancelling means.
- **Frame:** close button only, no icon, fixed size, the standard active caption
  gradient tinted with the owner's accent, and the title from the request.
  Placed centred over the owner and clamped to the screen.
- **Not a task:** excluded from the Task List, Alt+Tab and the taskbar's window
  list. With a dialog active, the task shown as active is its owner's.
- **Minimize** acts on the whole family; restoring or switching to the owner
  brings the dialog back on top.
- **Fullscreen:** opening a dialog for a fullscreen owner leaves fullscreen
  first; only the fullscreen app is composed, so the dialog would be active and
  invisible.
- **Lifetime:** owned windows are destroyed before their owner, never killed or
  waited on (they are not processes), and go away with the process.

`windowd` refuses the request, and the toolkit falls back to the in-surface
overlay, when the dialog does not fit on screen with its frame, when the id is
already in use, or when there is no free overlay slot.

## Budgets

- **Descriptors in `windowd`:** an open dialog costs two (duplicates of the
  owner's event pipe and wake event). That is the same as an app window, and it
  uses the same slots, so the full-session assertion in
  [WM_SUBSYSTEM.md](WM_SUBSYSTEM.md#descriptor-budget) still holds.
- **Pipes:** none. The dialog shares its process's event pipe.
- **Sections:** two per open dialog, out of 64 in the whole system (the WM's
  surface and the client's private backbuffer, the same as any window).

## The toolkit

`struct sxgui_dialog_host` gives a context's dialogs a window. `sxgui_app_run`
is the only host: it attaches itself when the loop starts, pumps the dialog
window's keyboard, pointer and close request, and paints it with
`sxgui_paint_dialog_window`. The dialog API did not change, so every app using
`sxgui_app_run` got windowed dialogs without edits.

Two consequences worth remembering:

- **An app with its own event loop keeps overlay dialogs**, because nobody would
  pump the dialog window. Media Player is one.
- **Keys from the dialog window go through the same path as the main window's**
  (`on_key`, then the toolkit), so apps that check `sxgui_dialog_active` before
  handling keys behave as before. Pointer events from the dialog skip `on_pointer`,
  whose coordinates belong to the app's window.

## Tests

- `handletest` (in `./build.sh smoke smoke`): shared memory through a received
  section, handles crossing `fork`, the queue limit, wrong pipe ends and I/O
  objects rejected, and queued sections released with the pipe.
- `windowd --selftest` (`./build.sh smoke windowd-smoke`): launches
  `widgetsdemo --dialog-selftest`, which opens its About through the real
  runtime, and asserts the family rules, the dialog frame, the task count, the
  descriptor cost, Alt+F4 closing it (and then closing a main window with its
  process), and both windows disappearing when the owner is terminated with a
  dialog open.

## Not done yet

- **Popups (menus, combobox lists) are still painted inside the surface** and
  can still be clipped. The next step is Wayland's model
  (`xdg_popup` + `xdg_positioner`): the client describes an anchor rectangle and
  which adjustments it accepts (flip, slide), and the WM places the popup so it
  fits. Do not copy X11's `override-redirect`, where the client places its own
  popups in global coordinates.
- **One section per window.** A per-connection pool (`wl_shm_pool`) would relieve
  the 64-section limit, but sections are committed in full at creation and cannot
  grow, so a pool would only reserve memory up front.
- **The native subsystem runtime** mirrors the new header fields but does not
  open owned windows.
