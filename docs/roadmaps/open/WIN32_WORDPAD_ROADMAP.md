# Windows XP WordPad on SSW — the second Win32 profile

> **Status: WP1 to WP6 complete (2026-11-26); WP7a done (2026-11-27). WordPad
> starts up to its first window; SSW handles x86 software exceptions, runs a
> second thread, has WordPad's files, heaps, atoms and locale, the hooks,
> positions, standard controls and clipboard MFC needs, TrueType text and
> the pictures its frame draws, and its own RichEdit control's text.** This is the plan of
> [W7](WIN32_SUBSYSTEM_ROADMAP.md#w7--later-profiles)'s first named profile.
> Its inventory, the W0 record taken before any code, is
> [WIN32_WORDPAD_PROFILE.md](../../userland/WIN32_WORDPAD_PROFILE.md); every
> count and function name below comes from there. What SSW already is, is
> [WIN32.md](../../userland/WIN32.md).

## Objective

The user's original Windows XP `wordpad.exe` (5.1.2600.7297), unchanged,
with the user's own `mfc42u.dll` and `mfc42loc.dll`, edits a formatted
document on the SavanXP desktop, saves it as RTF and as text, and opens it
again. Copy and paste reach other SavanXP programs in both directions. The
Microsoft files are supplied by the user, as for Solitaire, and never enter
the repository, the build, the image or the ISO.

A WordPad written for SavanXP, Wine's WordPad or a source port does not
satisfy this goal. A RichEdit control written for SavanXP does: the control
is a system DLL, like `user32`, and the decision below says why.

## Decisions taken

### RichEdit is written here (2026-11-21)

WordPad's document is a `RICHEDIT50W` window from `msftedit.dll`. SSW
provides its own `msftedit.dll` with that class, as it provides its own
`user32.dll`, instead of running Microsoft's RichEdit 4.1.

- **What it saves.** The load-time closure drops from 704 imports to 599
  (`wordpad.exe` and MFC alone). Microsoft's control draws with real font
  metrics (`GetOutlineTextMetricsA`, `GetCharWidthW`, 1,659 `ExtTextOutW`
  calls in the trace) and builds pictures as OLE objects; ours measures and
  draws with what `gdi32` has and draws a picture itself.
- **What it costs.** The 74 messages the control received, 32 of them
  `EM_*`, its `EN_*` notifications, its OLE interface to MFC, and an RTF
  reader and writer.
- **What the OLE question turned out to be.** It was not what forced the
  choice. A Wine experiment of 2026-11-21 ran the original `msftedit.dll`
  with its `ole32` lookups (it loads every `ole32` function at run time,
  through `GetProcAddress`) either hidden or made to fail. An absent
  function crashes it: paste without `OleGetClipboard`, start-up without
  `RegisterDragDrop`. A function that fails with an error is survived:
  failing clipboard calls fall back to the plain clipboard (RTF and
  `CF_UNICODETEXT`) and everything works; a failing `RegisterDragDrop` only
  disables dragging text; failing `OleCreateDefaultHandler` or
  `CreateStreamOnHGlobal` drops a picture, a failing `OleDraw` leaves its box
  blank. The tools and logs are kept with the user's files
  (`WordPadXP/w0/oleexp/`), not here.

### OLE is the minimum WordPad and MFC need

With our own control, the OLE that is left is MFC's and `wordpad.exe`'s.
`ole32.dll` exists, exports everything the closure imports, and implements
only what start-up and exit need: `OleInitialize` must succeed, because
WordPad's string 100 is "La inicialización de OLE ha fallado. WordPad no
puede continuar." The rest fails with an error HRESULT, the rule the
experiment above established for this kind of caller.

Not implemented, and not planned here:

- **The OLE clipboard.** Copy and paste go through `user32`'s clipboard
  (WP5), which reaches SavanXP's. Only Pegado especial (Paste Special) would
  need the OLE one; it reports nothing to paste.
- **OLE drag and drop.** The control moves a dragged selection inside the
  document by itself. Dragging between programs belongs to the desktop's
  drag and drop ([below](#what-the-rest-of-the-system-gets)), and
  `DoDragDrop` becomes a bridge onto it when that exists.
- **Embedded objects.** Insertar objeto lists nothing: SavanXP has no OLE
  servers. A picture in an RTF document (`\pict`) is drawn by the control; an
  `\object` shows its `\result`.
- **Structured storage**, until WP11 (Word documents).

### Exceptions are dispatched in user mode first

MFC reports its errors with C++ exceptions (`_CxxThrowException`), and every
C++ function with an object to destroy installs a frame at `fs:[0]`. Software
exceptions (`RaiseException`, C++ `throw`) never enter the kernel: WP2
dispatches them inside the guest. Hardware faults delivered to the guest
(S3 in [WIN32_SUBSYSTEM_ROADMAP.md](WIN32_SUBSYSTEM_ROADMAP.md#deferred-s1-s2-and-s3))
stay deferred: no trace reached one. `IsBadWritePtr`, the one probe on the
path, is answered from the guest's map without touching the memory.

### Guest threads take turns (2026-11-25)

A guest thread is a host thread, a SUN thread under `winhost`, with its own
TEB, guest stack and host stack, as WP3 planned. They do not run guest code
at the same time: one holds the turn, and gives it up when it waits on an
object, sleeps, waits for input, or calls the host while another thread is
ready.

- **Why.** Every DLL of SSW was written for one thread and keeps its state
  in plain globals: the heap, the handle tables, user32's windows, the
  registry and loader services. Taking turns makes all of it correct
  without a lock in each, and no lock of SSW's can take part in a deadlock.
  It also makes a run repeat itself, which the fixtures need to write the
  same bytes as Windows XP at 1, 2 and 4 processors.
- **What it costs.** Two guest threads never use two processors at once,
  and a thread that spins in guest code without calling the host keeps the
  others waiting. WordPad's second thread asks for the default printer and
  ends. Running guest threads at once, with locks in the DLLs, is a later
  decision if a program needs it.

### Help is out of scope

As for Solitaire. `F1` reaches `HtmlHelpA` in `hhctrl.ocx`, which does not
exist; WordPad's handling of that is measured in WP9 and kept.

### D1: real fonts, rasterized at run time by a system library (2026-11-21)

A word processor needs scalable text: sizes from 8 to 72 points, bold,
italic, underline, several faces, and the widths the control lays lines out
with. `gdi32` draws every font with the 13 px MS Sans Serif strike today,
and SavanXP has no font rasterizer at run time: its UI fonts are baked into
tables at build time ([UI_FONTS.md](../../desktop/UI_FONTS.md)).

- **A system library, not part of `gdi32`.** Fonts are loaded from files and
  rasterized at run time by a library every subsystem and the toolkit can
  use ([feature parity](../../../AGENTS.md#feature-parity-between-subsystems)
  in `AGENTS.md`). `gdi32` is its first consumer, not its owner.
  The baked UI tables stay what the desktop draws its own interface with.
- **FreeType is the candidate**, already used on the build host. It is
  offered under the FreeType License (BSD-style, with a credit in the
  documentation) or GPLv2; SavanXP would take the FreeType License, which an
  MIT project can carry. Its adoption goes through
  [THIRD_PARTY_PROVENANCE.md](../../third-party/THIRD_PARTY_PROVENANCE.md)
  like every vendored library: a pinned version, the license texts of the
  modules actually built, and the credit. If that review fails, the
  rasterizer is written here.
- **The faces.** Free fonts metric-compatible with Arial, Times New Roman
  and Courier New ship with the system, so a document lays out as it does on
  Windows XP. The user's own fonts, copied from their Windows XP disk like
  the other files of this profile, are used when present and never enter
  the repository.

The library's own design (its API, the cache, where font files live, how a
face is chosen) is a document of its own, written before WP6.

**Done (2026-11-25).** The library is `libsxfont`
([FONT_LIBRARY.md](../../desktop/FONT_LIBRARY.md)), over FreeType 2.15.0,
which passed the review under the FreeType License. Liberation Sans, Serif
and Mono ship in `/system/fonts`. Text follows each face's gasp table, as
Windows XP's standard smoothing did, with only the v35 interpreter
compiled. `gdi32` reaches the library through `winhost` services, WP6's to
write.

### D3: no printer (2026-11-21)

SavanXP has no printers. Configurar página, Vista preliminar and Imprimir
report that no printer is installed, as Windows XP does without one. A
printer that writes a file can be a later decision; nothing here depends on
it. `EM_FORMATRANGE` is not written until then.

### D2: `comdlg32` draws its own file dialogs (2026-11-21)

WordPad needs a choice of type (RTF, Word, text, Unicode text), and it
appends its own dialog template to Save As (`DEFAULTFORMATDIALOG`, the
"Establecer este formato como predeterminado" check box). In Windows XP's
model that template becomes a child window inside the common dialog, and
the program talks to the dialog through `CDM_*` messages and `CDN_*`
notifications, through MFC's hook procedure.

`comdlg32` therefore draws Open and Save As itself, as Win32 windows, where
all of that works as on Windows XP. It does not open SavanXP's
[`sxnfiledlg`](../../desktop/FILE_DIALOG.md): that would mean hosting a
Win32 child window inside a window that is not Win32, and answering `CDM_*`
for a window `user32` does not own. The layout follows Windows XP's classic
dialog, from a template written here. `sxnfiledlg` gains a list of types
on its own account, since its programs need one too.

## What the rest of the system gets

[Feature parity between subsystems](../../../AGENTS.md#feature-parity-between-subsystems)
in `AGENTS.md` applies: what this profile adds to SSW that
other subsystems lack is added to them too, not only to Win32.

- **The clipboard.** The device already holds a set of formats with
  `text/rtf` among the shared names ([CLIPBOARD.md](../../desktop/CLIPBOARD.md)).
  What is missing is Win32's layer over it (WP5), which the clipboard's
  document already lists. `winhost` is a SUN program, so it is the first
  consumer of the typed `sxn_` clipboard calls SUN has been waiting to add.
- **Drag and drop** — done with WP5
  ([DRAG_AND_DROP.md](../../desktop/DRAG_AND_DROP.md)): a file dragged from
  Files onto a Win32 window (`DragAcceptFiles`, `WM_DROPFILES`,
  `DragQueryFileW`) and onto Notepad. OLE's `DoDragDrop` is a later bridge
  onto it.
- **Scalable fonts** for every subsystem and the toolkit, per D1.
- **A list of file types** in SavanXP's own file dialog (D2), for its own
  programs.

## Phases

Each phase is one or more commits that leave the tree green, with fixtures
built here that give the same output on Windows XP, in the build-host
harness and on SavanXP, as W1 to W5 did. A test that needs the user's files
is skipped without them.

```text
WP1 ─► WP2 ─┬─► WP3 ─┐
            └─► WP4 ─┴─► WP5 ─► WP6 ─► WP7 ─► WP8 ─► WP9 ─► WP10 ─► WP11
RTF reader and writer (host only) ─────────┘
```

The RTF reader and writer of WP7 are portable C tested on the build host and
can start at any time.

### WP1 — loading WordPad and MFC

Size M.

- `wordpad.exe` at `0x01000000`, which has no relocations, as `sol.exe`.
  `mfc42u.dll` and `mfc42loc.dll` from the system folder, the latter by full
  path as MFC asks for it.
- `mfc42u.dll`'s 6,386 exports by ordinal; `wordpad.exe` imports 719 of
  them. MFC's thirteen delay-loaded DLLs go through MFC's own helper, over
  `LoadLibraryA` and `GetProcAddress`; a DLL that is never called need not
  exist.
- **Every load-time import resolves**, 599 of them. A function the trace
  never reached is still exported, and fails cleanly when called:
  `ERROR_CALL_NOT_IMPLEMENTED`, or `E_NOTIMPL` for a function that returns
  an HRESULT. They are listed by DLL in WIN32.md, so a later profile sees
  which entry points are only declared.
- The Unicode start-up (`__wgetmainargs`, `_wcmdln`) and the C++ runtime
  MFC links against: `operator new` and `delete`, `_purecall`,
  `__CxxFrameHandler` as a frame (dispatch is WP2), the wide-string
  functions.
- `ole32.dll`'s eleven load-time imports of `wordpad.exe` as exports, and
  `shlwapi.dll`.

**Done when:** a Unicode C++ fixture with static objects, `operator new`
and frames to unwind runs alike on all three environments; with the user's
files, a host test loads `wordpad.exe` and MFC, resolves every import, and
runs MFC's initialization up to its first window call.

**Done (2026-11-21).** Under `win32-harness` the user's `wordpad.exe`, with
`mfc42u.dll` and `mfc42loc.dll`, loads with its 599 load-time imports
resolved, runs MFC's and the C runtime's start-up and WordPad's own through
`OleInitialize` and its registry settings, and shows its first window: its
own message box saying it cannot load the RichEdit DLL, which is WP7's.
`win32-wordpad-test` checks that much, skipped without the files.
`wstartup.exe` writes byte-identical output with status 5 on Windows XP, in
`win32-harness-test` and in `win32-hello` ([WIN32.md](../../userland/WIN32.md#wordpad)).

What changed from the plan above, and why:

- **Declared entry points** are generated from a table per DLL
  (`declared.txt`, `dlls/declared.py`): 491 functions at first, each
  reporting its first call on the debug output, which is how the start-up
  was driven function by function. A function leaves the table when it is
  written.
- **More than the plan listed was on the path to the first window**, and
  is written: moveable memory (WP4's), TLS and critical sections for one
  thread, code pages, the locale (one: Spanish, Argentina), resources,
  module paths (a new host service, ABI version 6), `GetSystemMetrics`,
  registered clipboard formats, `GetObjectW`, and `ole32`'s initialization.
  MFC's delay-loaded `CoRegisterMessageFilter` is one: a delay-loaded
  function that is called must exist too.
- **The fixture is C, not C++.** It starts as a Unicode program and
  registers exit functions as a C++ program's static destructors are;
  `operator new` comes from MFC, not `msvcrt.dll`, in this profile, and a
  frame to unwind needs WP2's exception dispatch. Both are WP2's fixture.
- **Windows XP corrected two guesses.** Its `msvcrt.dll` prints floating
  point with 17 significant digits and zeros after them, and has no `ll`
  length; its `GetModuleFileNameW` leaves the last error alone when it cuts
  the path short. The heap now keeps the size asked for, which
  `GlobalSize`, `LocalSize` and `_msize` answer exactly there.
- **MFC's second thread is not on this path yet.** WordPad starts it right
  after loading the RichEdit DLL and before its main window (the thread asks
  for the default printer), so it is WP3's to meet once WP7's control
  exists. What WordPad does when it cannot start the thread is not known.

### WP2 — exceptions in user mode

Size M. No kernel change.

`RaiseException`; the walk of the `fs:[0]` chain, with the SafeSEH table
honoured for an image that has one (`wordpad.exe` lists 123 handlers);
`RtlUnwind` and the unwinding `_except_handler3` does; `__CxxFrameHandler`
and `_CxxThrowException`, with catch by type, by reference and `...`, a
rethrow, and the destructors of every unwound frame. An exception nobody
catches ends the process with Windows XP's status.

**Done when:** an exception fixture (filters, nested handlers, a C++ throw
through three frames with objects to destroy, an uncaught one) writes the
same bytes on Windows XP, in the harness and on SavanXP.

**Done (2026-11-23).** `sehscope.exe` exercises nested filters, termination
handlers and `RtlUnwind`. `cxxthrow.exe` exercises destructors across three
frames, catches by reference and by value, `catch (...)` and a rethrow.
Both match Windows XP byte for byte in the harness and on SavanXP.
`sehfail.exe` preserves the exception code. `cxxfail.exe` reproduces the
Windows XP CRT's termination status 3. See [WIN32.md](../../userland/WIN32.md#software-exceptions).

### WP3 — a second thread

Size M to L. MFC starts one at start-up (`_beginthreadex` with
`CREATE_SUSPENDED`, `SetThreadPriority`, `ResumeThread`); it made 44 calls in
the trace, and the main thread waits for it on events. What it does is
read from the trace's log before this phase starts.

A guest thread is a SUN thread with its own TEB, guest stack, host stack and
FS base (K1 made the selectors per-thread). With it: TLS slots, the loader
lock, `DLL_THREAD_ATTACH` and `DETACH` with `DisableThreadLibraryCalls`,
suspend counts, critical sections, events, `WaitForSingleObject` and the
interlocked functions; in `user32`, a message queue per thread and windows
owned by their thread.

**Done when:** a thread fixture (suspended start, TLS, attach and detach
order, an event handshake, a window on the second thread) runs alike on all
three environments, at 1, 2 and 4 vCPUs on SavanXP.

**Done (2026-11-25).** `threads.exe`, with `tlog.dll` and `tquiet.dll`,
writes the same bytes on Windows XP, in `win32-harness-test` and in
`win32-hello` at 1, 2 and 4 vCPUs, under TCG and KVM: a thread started
suspended and its suspend count, `TlsAlloc` and static TLS with its
callbacks, `DLL_THREAD_ATTACH` and `DETACH` in Windows XP's order and
`DisableThreadLibraryCalls`, events, mutexes, semaphores and the waits on
them, a contended critical section, `_beginthreadex` with a priority, and a
message-only window on the second thread that is sent, posted and thread
messages from the first. See [WIN32.md](../../userland/WIN32.md#threads).

What changed from the plan above, and why:

- **The threads take turns** ([decided above](#guest-threads-take-turns-2026-11-25)).
- **user32's message queue is per thread** and a window belongs to the thread
  that made it; focus, capture and the active window stay one for the
  process. Hooks are still WP5's: MFC's thread installs its message-filter
  hook and goes on without it.
- **ExitProcess stops the other threads first**, which then get no
  `DLL_THREAD_DETACH`, as Windows XP's output showed.

### WP4 — memory and the rest of `kernel32`

Size S to M.

- Moveable memory: `GlobalAlloc` and `LocalAlloc` with `GMEM_MOVEABLE`,
  handles with lock counts, `GlobalReAlloc`, `GlobalSize`, `GlobalFlags`,
  `GlobalHandle`; `GMEM_DDESHARE` accepted. Private heaps (`HeapCreate`,
  `HeapDestroy`, `HeapReAlloc`).
- Files with wide names: create, read, write, seek, size and times,
  `FindFirstFileW`, `GetFullPathNameW`, `GetVolumeInformationW`, the
  file-time conversions.
- Atoms, local and global. The locale: `GetLocaleInfoW`, the date and time
  formats and their enumeration for the user's locale (Fecha y hora), and
  `GetLocalTime`, which [TIMEZONE_ROADMAP.md](TIMEZONE_ROADMAP.md) already
  counts. `CompareStringW`.
- `win.ini`: RichEdit read `[windows]` and `[FontSubstitutes]`; with our own
  control, whoever still reads it gets fixed answers, documented.

**Done when:** a fixture covering each group, failure paths included, writes
the same bytes on all three environments.

**Done (2026-11-25).** `files.exe`, `memory.exe` and `locale.exe` write the
same bytes on Windows XP (`files.exe` from a folder on its NTFS drive), in
`win32-harness-test` and in `win32-hello`. See [WIN32.md](../../userland/WIN32.md#files).

What changed from the plan above, and why:

- **SUN gained the file calls the host needed**: opening an existing file to
  write, `sxn_seek_file`, `sxn_set_file_size`, `sxn_query_file` and
  `sxn_delete_directory`, with `winhost` as their consumer
  ([SUN.md](../../userland/SUN.md#files-and-small-profile-saves)).
- **A file's times cannot be set.** Neither SUN nor POSIX can change a
  file's time on SavanXP, so `SetFileTime` stays declared, and the hosts keep
  one time per file, its last write, in seconds.
- **Local time is UTC** until [TIMEZONE_ROADMAP.md](TIMEZONE_ROADMAP.md)'s T6;
  the fixture checks only that the conversions agree with each other.
- **The locale's values are Windows XP's own**, taken from `locale.exe`'s
  output there, and `CompareStringW` sorts as that locale does by letters,
  accents and case, with the hyphen and apostrophe weighed last.
- **`GlobalGetAtomNameW` with a short buffer is not compared**: Windows XP
  writes the whole name past the buffer there.

### WP5 — `user32` for MFC

Size L.

- Hooks: `WH_CBT` and `WH_MSGFILTER` on each thread, `WH_CALLWNDPROC`,
  `CallNextHookEx`. MFC attaches its objects to windows from the CBT hook.
- Subclassing: `SetWindowLongW` with `GWL_WNDPROC`, `CallWindowProcW`, and
  window properties, correct for every class, since MFC subclasses all of
  them.
- Deferred positioning, `SetParent`, carets, `ScrollWindowEx` and scroll
  bars; menus built and changed at run time, `TrackPopupMenu`, `SetMenu`;
  modeless dialogs (`CreateDialogIndirectParamW`, `IsDialogMessageW`).
- The standard controls Solitaire did not need: single-line Edit, ListBox,
  ComboBox (simple, drop-down, drop-down list, owner-drawn; the font box
  received 1,334 `CB_ADDSTRING`), ScrollBar.
- The clipboard over SavanXP's ([CLIPBOARD.md](../../desktop/CLIPBOARD.md)):
  `OpenClipboard` to `GetClipboardSequenceNumber`, `CF_UNICODETEXT` as
  UTF-8 text, `Rich Text Format` as `text/rtf`, `CF_HDROP` as
  `text/uri-list`, other registered formats under `application/x-win32-`.
  Delayed rendering is rendered eagerly, as the device requires.
- `DragAcceptFiles` and `WM_DROPFILES`, once the desktop's drag and drop
  exists.

**Done when:** fixtures for hooks and subclassing, for the controls driven
by an input script, and for the clipboard (text and RTF set by a fixture and
read by a SUN program, and the other way round) match Windows XP, and
`windowd-smoke` still passes.

**Done (2026-11-25).** `hooks.exe`, `controls.exe` (driven by
`fixtures/controls/input.txt`) and `clip.exe` write the same bytes on Windows
XP, in `win32-harness-test` and on SavanXP (`win32-hello`,
`win32-controls`); `controls.exe`'s two dumps equal the harness's golden
images on SavanXP, and Windows XP's outside the text. `clip.exe put` leaves
text, RTF and a format of its own, which `win32test`, a SUN program, reads
from `/dev/clipboard` and writes back itself, and `clip.exe get` then prints
what it printed on Windows XP. See [WIN32.md](../../userland/WIN32.md#hooks-and-subclassing)
and the sections after it.

What changed from the plan above, and why:

- **SUN's typed clipboard calls** (`sxn/sxnclip.h`) arrived with `winhost`,
  their first consumer, as the clipboard's document expected.
- **`CF_OEMTEXT` is code page 437**, which `kernel32` now converts, since
  Windows XP makes it from the text with that page.
- **Pixels outside the text, not glyphs.** The dumps differ from Windows
  XP's only in the text (the font is the platform's) and in what Windows XP
  adds itself: the menu's drop shadow, as for `dialog.exe`.
- **Drag and drop came with WP5** ([DRAG_AND_DROP.md](../../desktop/DRAG_AND_DROP.md)):
  `/dev/dragdata`, `windowd`'s drag events, Files as a source and Notepad
  as a target. A window with `WS_EX_ACCEPTFILES` gets `WM_DROPFILES`;
  `clip.exe drop` prints what it got in `win32-harness-test` and in
  `win32-drop`, where a SUN window drags two files onto it. Windows XP
  cannot record that output (an input script cannot drag from another
  program there), so `clip-drop.out` is the harness's. WordPad itself is
  dropped on once WP7 lets it reach its main window: today it stops at its
  message box about the RichEdit DLL, and after OK ends with `0xc06d007f`.
  A Win32 drag source and `RegisterDragDrop` targets are OLE's bridge (WP9).

### WP6 — text and pictures in `gdi32`

Size L. The font library of D1 exists
([FONT_LIBRARY.md](../../desktop/FONT_LIBRARY.md)); its
[path from `gdi32`](../../desktop/FONT_LIBRARY.md#win32-through-winhost-wp6)
is this phase's.

Fonts at any size, weight, slant and underline from `CreateFontIndirectW`;
`EnumFontFamiliesExW` (the font box and the font dialog),
`GetTextCharsetInfo`, and the widths and metrics our control lays lines out
with (`GetCharWidthW`, `GetTextExtentPoint32W`, `GetTextMetricsW`);
`ExtTextOutW` with clipping and per-character advances. The UI strike stays
for the faces the desktop draws with. Also what the frame and bars use:
`StretchDIBits`, 4-bit bitmaps, `SetROP2(R2_XORPEN)` for the ruler, the
raster operations of the profile's list.

**Done when:** a text fixture reports metrics and widths per face and size
that agree with Windows XP within a stated tolerance, and its pixels outside
text equal Windows XP's.

**Done (2026-11-26).** `text.exe liberation`, with the Liberation faces,
gives Windows XP RTM's metrics, widths, outline metrics, charset answers and
enumeration entries number for number, and its `StretchDIBits`,
`R2_XORPEN`, `Rectangle`, `CreateBitmap` and `PATCOPY` pictures have XP's
CRCs; the ink of its text is within 3% of XP's, in the same boxes and the
same greys (`win32-harness-test`, through `win32_text_compare.py`). On
SavanXP `winhost` prints the harness's bytes (`win32-hello`). With the
user's own Arial, Times New Roman and Courier New every metric and
enumeration entry equals XP's and a few glyphs' advances differ by a pixel
(`win32-text-xp-test`, local only). The interface's faces keep the UI
strike. See [WIN32.md](../../userland/WIN32.md#user32-and-gdi32), "Text".

### WP7 — the RichEdit control

Size XL, in parts, each its own commits.

- **7a, text.** `msftedit.dll` with `RICHEDIT50W`: the text, lines broken
  with `gdi32`'s widths, painting, caret, selection by keyboard and mouse
  (words, lines, a drag that moves the selection), scrolling, undo and redo,
  `EN_*` notifications as `EM_SETEVENTMASK` asks, copy and paste through
  `user32`'s clipboard in RTF and text.

  **Done (2026-11-27), RTF on the clipboard excepted.** `richedit.exe`,
  driven by messages and by its input script, writes byte for byte what it
  wrote with Microsoft's `msftedit.dll` on Windows XP RTM, and the control
  with a selection is XP's on screen but for one pixel of a glyph's ink
  (`win32-harness-test`, `win32-hello`, `win32-richedit`). Copy and paste
  carry text; the RTF on the clipboard comes with 7c's writer and reader,
  and until then the control knows its own copy by the clipboard's sequence
  number, so pasting it keeps its last paragraph mark as XP's RTF does. See
  [WIN32.md](../../userland/WIN32.md#richedit), "RichEdit".

  Inside WordPad's process the DLL does not load yet: WordPad and MFC have
  used the loader's 64 regions by then (`SXW_MAX_REGIONS`), and on SavanXP
  each region is one of `winhost`'s section handles, which the kernel
  bounds at 64 per process. That limit has to move before WP9's "WordPad
  starts" can be met ([WIN32.md](../../userland/WIN32.md#wordpad)).
- **7b, formats.** `CHARFORMAT2` (face, size, bold, italic, underline,
  colour, charset) and `PARAFORMAT2` (alignment, indents, tab stops,
  bullets), set and read by range; the word-wrap modes WordPad's Options
  set.
- **7c, streams.** `EM_STREAMIN` and `EM_STREAMOUT` in text, Unicode text
  and RTF. The RTF reader takes what WordPad and Microsoft's RichEdit write
  and skips groups it does not know; the writer writes what it reads back.
  `\pict` (DIB first) is drawn by the control; `\object` shows its
  `\result`.
- **7d, the rest.** `EM_FINDTEXTEXW` and the selection messages Buscar and
  Reemplazar use; `EM_GETOLEINTERFACE` returning an `IRichEditOle` with no
  objects and `EM_SETOLECALLBACK` keeping MFC's callback, which MFC's
  `CRichEditView` needs whatever OLE does. No `EM_FORMATRANGE` (D3).

**Done when:** a fixture that creates `RICHEDIT50W` gives the same answers
on Windows XP, with Microsoft's control, as on SavanXP with ours: formats
read back by range after setting them and after an RTF round trip, the text
streamed out, the found ranges, the notifications. Line layout is compared
only where WP6's metrics allow. The RTF reader and writer pass a corpus on
the build host, documents written by Windows XP's WordPad among them.

### WP8 — common controls and dialogs

Size L.

- `comctl32`: `ToolbarWindow32` (bitmap strips, separators, combo boxes
  inside it, tooltips), `msctls_statusbar32` (parts and the simple mode for
  menu help), `tooltips_class32`, image lists, `PropertySheetW` with
  `SysTabControl32`. Drawn in the classic style; the manifest's request for
  version 6 is accepted.
- `comdlg32`: `ChooseFontW`; `FindTextW` and `ReplaceTextW`, modeless, with
  `FINDMSGSTRING`; `GetOpenFileNameW` and `GetSaveFileNameW`, drawn here
  (D2), with their list of types, `DEFAULTFORMATDIALOG` appended to Save
  As, `CDM_*` and `CDN_*`; `GetFileTitleW`; `CommDlgExtendedError`;
  `PageSetupDlgW` and `PrintDlgExW`, which find no printer (D3).

**Done when:** a fixture drives each control and dialog by pointer and by
keyboard and matches Windows XP outside text, as `dialog.exe` did for W5.

### WP9 — OLE, the shell and the rest

Size S.

`ole32.dll` as decided above, with the running object table and file
monikers MFC asks for when a document opens, or their failure if MFC
tolerates it (measured first); `CoRegisterMessageFilter`,
`CoLockObjectExternal`, `CoDisconnectObject`, `CoTaskMemFree`, the CLSID
strings, `OleRegGetUserType` from the registry. `oledlg.dll`: Insertar
objeto and Pegado especial end as cancelled, `OleUIAddVerbMenuW` adds
nothing. `shell32`: `ShellExecuteExW`, `SHGetSettings`,
`SHGetSpecialFolderPathW`, `SHGetFileInfoW`. Help as above.

**Done when:** the user's WordPad starts, opens each menu and dialog,
and exits with "No" to saving, in the harness and on SavanXP.

### WP10 — Windows XP WordPad

Size M.

Launched from the Start menu with its own icon, and from Files on an `.rtf`
file. Its settings live under its own key in the per-user registry.

**Done when:** a recorded session on SavanXP types three lines with
accents, formats them from the keyboard, the bars and the dialogs, sets and
drags a tab stop, finds and replaces, copies to and pastes from a SavanXP
program in text and RTF, saves as RTF and as text, opens the RTF again, and
exits; a second start restores the window and the bars. Captures equal
Windows XP's outside text. The documents and settings survive
`./build.sh build` (rule 4). `win32-wordpad` is a smoke scenario, skipped
without the user's files.

### WP11 — Word documents

Size M. After WP10, and only if wanted.

The converters (`mswrd6.wpc`, `mswrd8.wpc`, `write.wpc`) import
`StgOpenStorage` and `StgCreateDocfile`: Word 6 and Word 97 documents are
compound files. This phase writes the compound file format, read and write,
tested on the build host against real documents, and loads the converters,
which prefer `0x00400000` (where `winhost` sits) and are relocated.
`OleLoad`, `OleSave` and `OleConvertIStorageToOLESTREAM` fail; what the
converters do with an embedded object then is measured.

**Done when:** a Word 97 document opens with its text and formats, and a
document saved as Word 6 opens on Windows XP.

## Not promised

Embedded and linked objects, OLE automation, printing (D3), Help,
right-to-left and complex scripts (Uniscribe was never loaded for Latin
text), WordPad as an OLE server for other programs, and any Microsoft file
in the tree.
