# The clipboard

`/dev/clipboard` is the system clipboard: one device node, in the kernel
(`kernel/clipboard.cpp`), shared by every program whatever subsystem it is
written against. This document pins its model, its formats and the rules each
subsystem follows to talk to it, and lists what is still missing for programs
of different subsystems to copy and paste between each other.

## Why a kernel device

Every channel of the window manager costs a descriptor per client against the
64 a process may hold, so a clipboard channel per client would lower the
ceiling on simultaneous windows ([`WM_SUBSYSTEM.md`](WM_SUBSYSTEM.md)). A
device node is opened to copy or paste and closed at once, costs no permanent
descriptor, and serves processes without a window too (a console program, a
compatibility subsystem's own layer).

## The model: a set of formats, replaced whole

The content is a **set** of up to `SAVANXP_CLIPBOARD_MAX_FORMATS` (8) entries.
Each entry is a format name and its bytes. The same content is usually offered
in more than one format: a file list as `text/uri-list` and as plain text, a
formatted paragraph as `text/rtf` and as plain text. A reader picks the one it
understands best.

- **A writer replaces the whole set at once.** `CLIP_IOC_SET_FORMATS` copies
  every entry into the kernel first and only then swaps the set; a bad
  pointer, a bad name or a set over the capacity fails and leaves the
  previous set exactly as it was.
- **A reader asks by name.** `CLIP_IOC_GET_FORMAT` returns the bytes of one
  format (`ENOENT` if the set does not have it), a short buffer gets the
  prefix and the full length. `CLIP_IOC_ENUM_FORMAT` lists the names in the
  order the writer gave them, which is the writer's preference, richest
  first.
- **It is a value, not a stream, and there is no owner.** Reading twice gives
  the same bytes. Whoever copies leaves every format it offers and forgets
  about it: the program that copied may have ended before anyone pastes.
  Delayed rendering (Win32's `SetClipboardData(format, NULL)`, X11's
  selection owner) is therefore rendered eagerly by the subsystem that offers
  it, when its program copies or at the latest when it ends.
- **`CLIP_IOC_GET_INFO`** gives the text entry's length, the capacity, a
  summary (`EMPTY`, `TEXT` when a text entry is present, `OTHER` when the set
  has formats but no text), the number of formats, and `sequence`, which
  rises with every change, `CLEAR` included.
- **Writing needs write access.** `CLEAR` and `SET_FORMATS` refuse a
  read-only descriptor with `EACCES`.

### Following changes

A descriptor is **readable** while the content has changed since that
descriptor last called `CLIP_IOC_GET_INFO`. A program that follows the
clipboard keeps one descriptor open (read access is enough), asks `GET_INFO`,
and waits on the descriptor with the rest of its loop: `poll` with `POLLIN` in
POSIX, `sxn_wait_io` with `SXN_IO_READ` or `sxn_wait_many` in SUN. When it
wakes it asks `GET_INFO` again, which both tells it what changed and clears
the change, and goes back to waiting.

- **A descriptor just opened is readable at once.** It has seen nothing, so a
  change between opening and the first `GET_INFO` cannot be lost.
- **Only `GET_INFO` clears it.** Reading the content (`read`, `GET_FORMAT`,
  `ENUM_FORMAT`) does not, so a change that lands while a reader is halfway
  through the formats still shows up.
- **Every change counts**, the program's own copies and `CLEAR` included, as
  in Windows' `WM_CLIPBOARDUPDATE`. Changes are not queued: three copies
  while a program was busy wake it once, and `sequence` says how far it moved.
- **A change wakes the waiters at once**, not at the next tick: the device
  calls `process::notify_device_readable`, which looks at the parked waits
  again the way an object signal does.
- **`read` still never blocks.** Readable means "changed", not "has data":
  an empty clipboard reads zero bytes whatever `poll` says.

The kernel keeps what each descriptor has seen in the open handle
(`object::IoObject::device_state`, the sequence plus one so that zero is
"nothing"). A device opts in through the `ioctl_state` and `can_read_state`
hooks of `device::Device`, which see that word; the clipboard is the first.
Keeping a descriptor open for this costs one of the process's 64 handles,
which is the trade the device node made in the first place: only a program
that follows the clipboard pays it.

### Capacity

`SAVANXP_CLIPBOARD_CAPACITY` is 1 MiB for **all formats together**. The bytes
live in one kernel heap block allocated per set and freed when the next set
replaces it, so an empty clipboard costs nothing. A larger set is refused with
`ENOSPC`, never truncated: half a text pasted without a word is worse than an
error on copy. A block larger than the heap's 64 KiB arenas gets an arena of
contiguous pages sized to it, so a very fragmented machine can answer `ENOMEM`
below the capacity.

### Format names

A name is printable ASCII (0x20-0x7e), not empty, not starting with a space,
under `SAVANXP_CLIPBOARD_NAME_CAPACITY` (64) bytes with its NUL, and unique in
its set. The kernel compares names byte for byte and gives none of them a
meaning except the text one, which plain `read` and `write` use.

Formats shared between subsystems are named with **lowercase media types**.
A subsystem maps its own identifiers to these names in its own layer:

| Name | Content | Win32 | X11 |
|---|---|---|---|
| `text/plain;charset=utf-8` (`SAVANXP_CLIPBOARD_TEXT`) | UTF-8 text, LF line ends, no NUL | `CF_UNICODETEXT`, `CF_TEXT`, `CF_OEMTEXT` (converted) | `UTF8_STRING`, `STRING`, `TEXT` |
| `text/uri-list` (`SAVANXP_CLIPBOARD_URI_LIST`) | `file://` URIs, one per CRLF-ended line (RFC 2483) | `CF_HDROP` | `text/uri-list` |
| `application/x-savanxp-drop-effect` | `copy` or `move`: whether a pasted file list is copied or moved ([`FILES.md`](FILES.md#cut-copy-and-paste)) | registered `Preferred DropEffect` (a `DWORD`) | — |
| `text/rtf` | RTF | registered `Rich Text Format` | `text/rtf` |
| `image/bmp` | a BMP file | `CF_DIB` (converted) | `image/bmp` |

A format only one subsystem knows keeps a name of that subsystem's own,
prefixed so it cannot collide with a media type (for a Win32 registered
format, `application/x-win32-` followed by its name, for instance). Only the
text and URI-list names are in the ABI today; Files writes the drop effect,
and the rest of the table is the naming to follow when their first consumer
arrives.

### The text format

The text entry is **UTF-8 with LF line ends**, and that is the only text
form stored. A subsystem whose programs expect another encoding or CRLF
converts in its own layer, in both directions: the Win32 subsystem offers
`CF_TEXT` (ANSI code page), `CF_OEMTEXT` and `CF_UNICODETEXT` (UTF-16, CRLF)
from the one entry, and turns any of them back into it on copy. The kernel
stores bytes and does not check the encoding, so a reader still copes with
invalid sequences (replaces them) instead of trusting the writer.

### Plain `read` and `write`

They are the text format alone, which is what kept every program written
before the formats working unchanged:

- `write` replaces the whole set with one text entry (a zero-length write
  never reaches the device; emptying is `CLEAR`);
- `read` returns the text entry from its start, or zero bytes when the set
  has no text.

## How each subsystem reaches it

| Subsystem | How | Status |
|---|---|---|
| POSIX | `savanxp/libc.h`: `clipboard_set_text`, `clipboard_get_text`, `clipboard_get_info`, `clipboard_clear`; `clipboard_set_formats`, `clipboard_get_format`, `clipboard_enum_format` | done; `cliptest` in `smoke` |
| SUN | `sxn_open_device("clipboard")` and `sxn_device_control` with the ioctls above; the toolkit's SUN host (`system/runtime/sxngui.c`) for text | done at the device level; typed `sxn_` calls wait for their first consumer ([`SUN.md`](../userland/SUN.md), rule 1); `nativetest` |
| SXGUI-C toolkit | text through `struct sxgui_host`; formats through `struct sxgui_host_clipboard` and `sxgui_clipboard_set_formats`, `_get_format`, `_enum_format`, `_has_format`, for platform code and programs of either subsystem ([`PLATFORM_GRAPHICS.md`](PLATFORM_GRAPHICS.md#the-clipboards-formats)) | done; `seltest` (POSIX) and `guicliptest` (SUN) in `smoke`; Files' Cut, Copy and Paste is the first program on it |
| Win32 | `user32` has no clipboard calls yet | missing |
| Linux | guest paths are rooted at `/system/linux`, so `/dev/clipboard` does not exist for a guest | missing |
| Console | no mark, copy or paste | missing |

## What is still missing

In the order they depend on each other:

1. **Formats** — done: the set, the names, UTF-8 text, 1 MiB.
2. **Change notification** — done ([above](#following-changes)): what
   Win32's `AddClipboardFormatListener` and an X11 selection bridge stand on,
   and what lets a menu enable Paste without asking on every repaint.
3. **Per-session content — deferred until there is a second user.** There is
   one set for the whole machine, readable by every process. Today that
   separates nothing: there is one user, and the only other processes are the
   system's, which may read anything anyway. The kernel has no session either,
   only the token's `{uid, euid, gid}`, and more than one user is out of scope
   ([`USERS_AND_PERMISSIONS.md`](../kernel/USERS_AND_PERMISSIONS.md#deliberately-out-of-scope)).
   It comes with that work: one set per session, the session inherited on
   `spawn` and `fork` and given by `sessiond`. Not one per uid, which would
   give the system's services and the smokes a clipboard apart from the
   user's while an elevated program (uid 1000, euid 0) shared the user's.
4. **The subsystems' layers.** Toolkit formats — done
   ([above](#how-each-subsystem-reaches-it)). Left: Win32's `OpenClipboard`, `EmptyClipboard`,
   `SetClipboardData`, `GetClipboardData`, `CloseClipboard`,
   `IsClipboardFormatAvailable`, `EnumClipboardFormats`,
   `RegisterClipboardFormat` and `GetClipboardSequenceNumber` (which is
   `sequence`), which WordPad's copy and paste go through
   ([`WIN32_WORDPAD_ROADMAP.md`](../roadmaps/open/WIN32_WORDPAD_ROADMAP.md#wp5--user32-for-mfc));
   OLE's clipboard is not planned; the
   device visible to Linux guests; and copy and paste in the console. Files'
   Cut, Copy and Paste of files stand on the toolkit's formats
   ([`FILES.md`](FILES.md#cut-copy-and-paste)).
