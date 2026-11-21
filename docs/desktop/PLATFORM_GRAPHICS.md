# Platform graphics: what is the platform's and what is a subsystem's

SXGFX, SXCHROME and SXGUI-C were written for SavanXP, not ported from POSIX.
They lived in the POSIX SDK only because the POSIX SDK came first. They now
sit in the platform layer ([SYSTEM_LAYERING.md](../userland/SYSTEM_LAYERING.md)):
**neither POSIX nor SUN**, linked by both subsystems, the way GDI is neither a
Win32 nor a POSIX library in Windows.

## The rule

**Platform graphics code makes no system call.** It draws into memory it is
handed, reads input structs it is handed, and asks for anything else through a
table the subsystem fills in. Whatever talks to the window system (connect,
present, read events, open owned windows) belongs to a subsystem, because each
subsystem talks to `windowd` its own way: POSIX through file descriptors and
`poll`, SUN through `sxnwm`.

The check is mechanical. Nothing under `system/gfx/` or `include/sxgfx/` may
include a header from `subsystems/`:

```bash
grep -n '#include "savanxp/\|#include "libc.h"' system/gfx/* include/sxgfx/*
```

must print nothing. The only vocabulary they share with the rest of the system
is the neutral wire contract, `abi/savanxp_abi.h` (`savanxp_fb_info`,
`savanxp_input_event`, `SAVANXP_KEY_*`, ...), plus the C library's `string.h`
and `stdlib.h`.

## Where each piece lives

| Piece | Platform (`system/gfx`, `include/sxgfx`) | POSIX (`subsystems/posix/sdk/v1`) | SUN (`system/runtime`, `include/sxn`) |
|---|---|---|---|
| Pixel primitives and fonts | `pixels.c`, `pixels.h`, the font tables, `gfx_utf8.inc` | — | — |
| Painter, bitmaps, regions (SXGFX) | `gfx2d.c`, `gfx2d.h` | `runtime/gfx_scaled.c`: `sx_scaled_presenter`, which presents onto a POSIX window | — |
| System 3D edges (SXCHROME) | `sxchrome.c`, `sxchrome.h` | — | — |
| 3D rasterizer (SxGL, [SXGL.md](SXGL.md)) | `sxgl.c`, `sxgl.h` | — (the program presents what it drew) | — |
| Widget toolkit (SXGUI-C) | `sxgui.c`, `sxgui.h` | `runtime/sxgui_app.c`: the application frame over a POSIX window, and the toolkit's POSIX host | `sxngui.c` + `sxngui.h`: the frame over `sxnwm`, and the SUN host |
| Window client | — | `runtime/gfx.c` + `gfx_impl.inc`: `gfx_open`, `gfx_present`, `gfx_poll_event`, owned windows, launching | `sxnwm.h`: `sxnwm_connect`, `sxnwm_submit_full`, the input ring, owned windows |

The POSIX SDK keeps `savanxp/gfx2d.h`, `savanxp/sxchrome.h` and
`savanxp/sxgui.h`. Each includes its platform header and adds what only a
POSIX program has (the scaled presenter, the application frame). POSIX apps
and ports keep including them unchanged. Platform code and SUN programs
include `sxgfx/*.h` directly.

## The toolkit's host

The toolkit needs three things from the system: the clock (to tell a double
click from two clicks) and the clipboard (read and write). They come from a
`struct sxgui_host`:

```c
struct sxgui_host {
    unsigned long (*uptime_ms)(void);
    int (*clipboard_get_text)(char *buffer, int capacity);
    int (*clipboard_set_text)(const char *text);
};
void sxgui_set_host(const struct sxgui_host *host);
```

`sxgui.c` defines `sxgui_default_host()` as a **weak** symbol returning NULL.
The POSIX runtime (`sxgui_app.c`) defines the strong one, which returns the
POSIX host. Every POSIX program that links the toolkit therefore has a host
without calling anything, including the ones that drive the widgets without
`sxgui_app` (`seltest`). A program without a host keeps working, degraded: no
double click, nothing to paste, copy fails.

A SUN program has its host in `system/runtime/sxngui.c` (`sxngui_host()`):
the clock is `sxn_clock_ns`, the clipboard `/dev/clipboard` through
`sxn_open_device`. A native program links the whole user runtime, POSIX frame
included, so the strong POSIX `sxgui_default_host` is in it too; that is why
the SUN frame installs its host with `sxgui_set_host` instead of relying on
the default. It came with Task Manager, the first SUN program that uses the
toolkit, as rule 1 of
[SUN.md](../userland/SUN.md#the-direction-the-systems-own-api) asks.

The frame (`sxngui_app_*`, `include/sxn/sxngui.h`) is the counterpart of
`sxgui_app.c` over `sxnwm`: the same hooks (`on_key`, `on_pointer`,
`on_paint`, `on_resize`, `on_tick`), and dialogs in owned windows. Its names
differ because both frames end up linked into a native program. Where the
POSIX loop sleeps 16 ms between passes, this one waits on the window's wake
event and the dialogs' pipe with a timeout until the next tick, so an idle
window does not wake up.

A worker thread hands results back with `sxngui_app_wake`, callable from any
thread: it sets a flag and an event of the frame's own, and the loop calls the
app's `on_wake` hook on the UI thread and repaints. Wakes coalesce, so the hook
drains whatever the worker queued; the hook, not the worker, touches widgets.
The event lives until the process ends, so a worker that wakes a window
already closed does no harm. Web Viewer's network worker is the consumer
([WEBVIEWER.md](WEBVIEWER.md#native-frontend-and-io)). **Pending, POSIX
parity:** `sxgui_app.c` has no `on_wake` yet; it is to gain one (a flag its
16 ms poll reads) with the first POSIX program that loads in the background.

Dialogs work the same way: `struct sxgui_dialog_host` gives them a window of
their own, and each frame is the host that provides it.

### The clipboard's formats

The host above carries the clipboard's text only. Its formats
([`CLIPBOARD.md`](CLIPBOARD.md)) come from a second table, so that platform
code and a program of either subsystem reach them with the same calls:

```c
struct sxgui_host_clipboard {
    int (*set_formats)(const struct sxgui_clipboard_format *formats, int count);
    int (*get_format)(const char *name, void *buffer, int capacity);
    int (*enum_format)(int index, char *name, int name_capacity);
};
int sxgui_clipboard_set_formats(const struct sxgui_clipboard_format *formats, int count);
int sxgui_clipboard_get_format(const char *name, void *buffer, int capacity);
int sxgui_clipboard_enum_format(int index, char *name, int name_capacity);
int sxgui_clipboard_has_format(const char *name);
```

**A table of its own, not three more fields in `struct sxgui_host`.** The
program defines the host and `libsxgui.so` reads it: a library that read a
field an installed program was built without would read past the end of that
program's struct and call whatever it found there. The table is found the
way the host is, through a weak `sxgui_default_host_clipboard`, so a program
built before it defines none and its toolkit keeps text only (the calls
answer < 0). POSIX's (`sxgui_app.c`, over libc's `clipboard_set_formats` and
the rest) is weak; SUN's (`sxngui.c`, over `sxn_device_control`) is strong
and is also installed by `sxngui_app_init`. A later service for the host
gets a table of its own for the same reason.

Files' Cut, Copy and Paste is its first program
([`FILES.md`](FILES.md#cut-copy-and-paste)). `seltest` checks the POSIX table, a text field pasting the text of a set that
also holds a file list, and a host without formats; `guicliptest`, run by the
core smoke, checks the SUN table through the shared `libsxgui`.

## The window protocol

The same reasoning moved the WM protocol out of the POSIX SDK:
`include/wm/wm_protocol.h` (the port name and connect timeout, and the
protocol's description) and `include/wm/wm_shell_protocol.h` (the shell's
window list, whose structs it defines itself; the client protocol's structs
are in `abi/savanxp_abi.h`). SUN's `sxnwm.h` and
the native services include `wm/` directly; the POSIX SDK keeps
`savanxp/wm_protocol.h` and `savanxp/wm_shell_protocol.h` as forwarding
headers.

## Builds outside CMake

CMake and the builds outside it (`tools/make-sdk.sh`, the Doom, ccleste and
FFmpeg ports) take the runtime's units from one manifest,
`subsystems/posix/sdk/v1/runtime/units.txt`, whose entries are repository
paths ([BUILD_CMAKE.md](../build/BUILD_CMAKE.md#the-runtimes-units-one-manifest)).
Moving a unit between `system/gfx` and the POSIX runtime changes that file and
the matching `SAVANXP_LIBRARY_REPLACES_*` entry, nothing else.

## Known gaps in SXGUI-C

What the toolkit does not yet do, and the workaround each program carries
until it does. When a gap is closed, the workarounds listed for it go too.

| Gap | Workaround today | Fix |
| --- | --- | --- |
| `sxgui_textview` draws one item per row and clips the rest: no word wrap and no horizontal scroll | System Properties' Licenses tab wraps every line itself, measuring with `gfx_text_width` (`license_wrap` in `aboutapp.c`) and storing the result in a static 160 KiB / 4096-line buffer | A wrap flag on the text view, wrapping at paint time against its own inner width, so the caller passes the text unwrapped |
| The text view's usable width is private (`sxgui_listbox_inner` in `sxgui.c`) | `aboutapp.c` recomputes it as `width - SXGUI_BORDER_SUNKEN * 2 - SXGUI_SCROLLBAR_THICKNESS - SXGUI_TEXT_PAD * 2`, and assumes the scrollbar is always there | A public `sxgui_listbox_text_width()`, or no need for it once the view wraps |
| The listbox row height is private (`sxgui_row_height`, `gfx_text_height() + 4`) | `aboutapp.c` copies the formula to size its list to whole rows (`ABOUT_LICENSE_LIST_HEIGHT`); `sxbide.c` copies it to keep its Output pane scrolled to the last line (`listbox_rows`) | A public `sxgui_row_height()`, or a constructor that takes a row count |
| `sx_painter_draw_text` does not expand `\t` | `license_wrap` turns tabs into spaces to the next multiple of eight before drawing | Tab stops in the text drawing, or in the text view at least |
| `SXGUI_PROPGRID` has no scrollbar and no drop-down: rows past the bottom follow the selection, and a LIST value steps through its choices in place | Savan BASIC (`sxbide.c`) lists only the properties written in a form, so its grid stays short; no code to remove | The listbox's scrollbar column, and the combobox's popup opened from the value cell |
| No widget hierarchy: a tab page's controls are siblings of the tab control | Each tabbed program hides the other pages' controls with `SXGUI_FLAG_VISIBLE` in its own `show_page` (`sound.c`, `display.c`, `taskmgr.c`, `aboutapp.c`) | A page index on each widget that the tab control honours, so painting, hit testing and focus skip the other pages without per-program code |

## Keyboard menu navigation

`sxgui_popup_menu_step` skips separators and disabled actions in both
directions, wraps between selectable entries, and returns -1 when none are
available. This rule is shared by context menus, menu bars and the desktop;
programs do not compensate by counting or hiding disabled rows. Image Edit
introduced a disabled row for folders and exposed a selector that previously
stopped on it even though Enter correctly refused to run it.

The host `sxgui-menu-test` checks the real selector, including leading/trailing
disabled rows, wraparound and an entirely unavailable menu. The visual
`filesmenu` scenario checks that Down still reaches Send To Desktop for a
folder when Edit is disabled.
