# Drag and drop

How a program drags something onto another program's window: files from
Files onto WordPad, text or files between the toolkit's programs. This
document pins the model, the data store, the protocol between the clients
and `windowd`, and what each subsystem builds on it. The clipboard
([CLIPBOARD.md](CLIPBOARD.md)) is its closest relative, and the two share
their formats on purpose.

## The model

A drag has three parties: the **source**, the program the drag starts
from; the **target**, the program whose window is under the pointer; and
`windowd`, which owns the pointer and is the only one that knows which
window that is. Neither program talks to the other: everything goes through
`windowd` and a data store in the kernel.

1. **The source leaves its data first.** While the left button is held on
   something it can drag, and the pointer has moved far enough, the source
   writes everything it offers into `/dev/dragdata` as a set of formats,
   then asks `windowd` to start a drag with the effects it allows (copy,
   move, link).
2. **`windowd` takes the pointer.** From then on no pointer record reaches
   any window until the drag ends. The window under the pointer is told
   that the drag entered it, moved over it, or left it, with the position
   in its own client coordinates and the effects allowed.
3. **The target answers, without blocking anything.** For every enter or
   move it may write its answer in its surface header: the drag it answers
   and the effect it would take at the last position it was told, or 0 for
   none. It does not read the data to answer: the formats' names tell it
   enough, and it may list them from `/dev/dragdata` at any time. `windowd`
   shows the answer as the cursor: the arrow where a drop is taken, the
   unavailable sign where it is not.
4. **The release decides.** When the button comes up over a window whose
   last answer for this drag is an effect the source allowed, that window
   is told the drop, with the effect; it then reads the formats it wants
   from `/dev/dragdata`. Over any other window the drag simply ends. Escape
   cancels it, as does the source's window going away.
5. **The source hears the end** with the effect the drop took, or 0 when
   nothing was dropped. A source that moves deletes its originals only on a
   `move`.

No party ever waits for another. A target that is slow to answer shows the
unavailable cursor until it does; a target that answers after the release
loses the drop, as on Windows when a program is not responding.

### Why `windowd` and not a channel between programs

A direct channel would cost a descriptor per window in every client
([WM_SUBSYSTEM.md](WM_SUBSYSTEM.md#descriptor-budget)), and the source
cannot know who is under the pointer anyway: only `windowd` hit-tests, and
it already sends every window its pointer records. A drag is one more kind
of record on the same event pipe, and one more small field in the surface
header the client already writes its requests into.

### Why the data is in the kernel, and stored first

The same reasons as the clipboard's: a device node costs nothing while it
is not open, serves every subsystem, and holds a value rather than a stream.
Storing the data before the drag starts, rather than rendering it on the
drop as OLE does, keeps the protocol free of round trips between programs:
the target never waits on the source, and a source that ends during the
drag leaves its data behind. A drag offers what a copy offers, and both are
small for the cases at hand (a list of file names, a selection of text).

## `/dev/dragdata`

A second instance of the clipboard's store (`kernel/clipboard.cpp`), with
every rule of [CLIPBOARD.md](CLIPBOARD.md#the-model-a-set-of-formats-replaced-whole):
a set of up to eight named formats replaced whole, 1 MiB in all, the same
ioctls (`CLIP_IOC_SET_FORMATS`, `GET_FORMAT`, `ENUM_FORMAT`, `GET_INFO`,
`CLEAR`) and the same format names (`text/uri-list` for files,
`text/plain;charset=utf-8` for text, `text/rtf`, ...). It is a separate node
so that dragging never touches what the clipboard holds.

The store keeps only the latest drag's data. A drag that starts while
another target is still reading would replace it, which cannot happen: one
pointer, one drag at a time, and `windowd` starts a drag only on a held
button. Like the clipboard, it is one store for the whole machine,
readable by every process ([CLIPBOARD.md](CLIPBOARD.md#what-is-still-missing),
item 3, applies to both).

## The protocol

### Starting: the header's drag request

The surface header grows a tail after the long title,
`struct savanxp_wm_drag` (`include/abi/savanxp_abi.h`), which the client
writes and `windowd` only reads, copying each value before it uses it:

| Field | Written by | Meaning |
| --- | --- | --- |
| `effects` | the source | the effects it allows: `SAVANXP_DRAG_EFFECT_COPY`, `_MOVE`, `_LINK` |
| `start_sequence` | the source | one more than before to start a drag, after `effects` and the data |
| `reply_drag` | the target | the drag its answer is for |
| `reply_effect` | the target | the effect it would take there, 0 for none |

A start is honoured once per new sequence, and only while the left button
is held and the process asking is the one the button was pressed on;
anything else is ignored, so a stale or stray request cannot steal the
pointer. A client with the request event sets it after writing, as for any
other request. Only the main window's header carries the start; an owned
window's answer is read from its own header.

### The events: `SAVANXP_WM_EVENT_DRAG`

A new kind of event record, `struct savanxp_wm_drag_event` in its payload:
`phase`, `x`, `y` (client coordinates of the window it goes to), `drag` (a
number `windowd` gives each drag, never 0), `effects` (what the source
allows) and `effect`.

| Phase | Goes to | Means |
| --- | --- | --- |
| `ENTER` | the window under the pointer | the drag came over it |
| `OVER` | the same window | the pointer moved over it |
| `LEAVE` | the window the drag was over | the drag left it, was cancelled, or was released over it without its acceptance |
| `DROP` | the window under the pointer | it is dropped there with `effect`: read `/dev/dragdata` now |
| `END` | the source's main window | the drag is over; `effect` is what the drop took, 0 if nothing |

A client that does not know the kind drops the record, as every runtime
does with kinds it does not know; it then never answers and is never
dropped on.

The effect of a drop is the target's answer masked by what the source
allows; of several bits, copy wins over move and move over link.

### What `windowd` does meanwhile

- Every window may be a target, the source's own included, except a
  disabled owner (a window with a modal dialog up).
- No pointer record reaches anyone, the frame's buttons and edges do
  nothing, and the hover does not move. Keys go where they always go,
  except Escape, which cancels.
- The cursor is the arrow over a window that accepts this drag at this
  point, and the unavailable sign anywhere else.
- A source whose process ends cancels the drag; a target that ends is
  left, as if the pointer had moved off it.

## What each subsystem builds on it

| Side | SUN | POSIX | Toolkit (SXGUI-C) | Win32 (`winhost`, `user32`) |
| --- | --- | --- | --- | --- |
| The data | `sxn_dragdata_open` and the `sxnclip.h` calls | `dragdata_set_formats`, `dragdata_get_format`, `dragdata_enum_format` in `savanxp/libc.h` | the subsystem's own calls | `winhost` over SUN |
| Source | `sxnwm_drag_start` | `gfx_drag_start` | `sxgui_app_drag_start` (POSIX), `sxngui_app_drag_start` (SUN) | — (OLE's `DoDragDrop`, later) |
| Target | `SAVANXP_WM_EVENT_DRAG` records, `sxnwm_drag_reply` | `gfx_poll_window_drag`, `gfx_drag_reply` | `on_drag_over`, `on_drag_leave`, `on_drop` | `WS_EX_ACCEPTFILES` windows get `WM_DROPFILES` |

**The toolkit** gives an application, in both of its runtimes, hooks next
to its other callbacks: `on_drag_over`, called for each enter and move with
the point and the effects allowed, answers the effect it would take (it may
list the formats from `/dev/dragdata`); `on_drag_leave`; and `on_drop`, which
reads the formats it wants. A source calls `sxgui_app_drag_start` (or
`sxngui_app_drag_start`) with its formats from a pointer handler while the
button is held, and hears the end through `on_drag_end`. While a drag is on,
the source's window gets no pointer records, so the toolkit ends any press
it was tracking when the drag ends.

**Files** is the first source: a press on an entry of the list and a move
of more than four pixels (`FILESAPP_DRAG_DISTANCE`) drags it as Copy puts
it on the clipboard, its `file://` URI as `text/uri-list` and its path as
plain text, allowing copy. One entry, as Files selects one at a time. **Notepad** is the first
toolkit target: a file dropped on it is opened, as File, Open would.

**Win32.** `winhost` forwards the drag events to `user32` as a host event.
`user32` finds the window under the point that has `WS_EX_ACCEPTFILES`
(the window itself or the nearest ancestor that has it, as Windows looks),
answers copy when the set has `text/uri-list` and such a window exists, and
on the drop reads the list and posts that window `WM_DROPFILES` with a
`DROPFILES` block made from it (`make_drop`): the paths as Windows paths
(`C:\` for the guest's drive, `Z:\` for the rest of SavanXP), the point in
the window's client coordinates. WordPad opens a file dropped on it this
way. A Win32 program as a source, and OLE's `RegisterDragDrop` targets,
are later: `DoDragDrop` becomes a bridge onto this when WordPad's RichEdit
work needs it.

## How it is checked

- `windowd --selftest` checks the effect a drop takes from an answer and
  an allowance; `nativetest` the store's round trip, kept apart from the
  clipboard's, and the refusals of the header calls.
- `win32-harness-test` plays `clip.exe drop` with the input script's
  `dragover` and `drop`, and **`win32-drop`** does it on SavanXP: a small
  SUN window, `win32test --source`, starts a drag of two files when the
  host presses on it; the host drags onto `clip.exe`'s window and releases;
  `clip.exe` must print the same bytes as in the harness and the source
  must hear that the drop took a copy.

## What it does not do

- **No drag image.** The cursor changes; nothing follows it. A picture of
  what is dragged would need the compositor to draw a second cursor-like
  layer, which is the natural next step.
- **No modifier keys.** Windows chooses move or copy with Shift and Ctrl;
  here the target answers one effect and the source's allowance decides.
- **No drop on the desktop or the taskbar yet.** They are targets like any
  window; the desktop has not written its answer.
- **No dragging between machines or sessions.** One store, one session.
