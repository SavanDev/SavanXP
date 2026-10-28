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

| Piece | Platform (`system/gfx`, `include/sxgfx`) | POSIX (`subsystems/posix/sdk/v1`) |
|---|---|---|
| Pixel primitives and fonts | `pixels.c`, `pixels.h`, the font tables, `gfx_utf8.inc` | — |
| Painter, bitmaps, regions (SXGFX) | `gfx2d.c`, `gfx2d.h` | `runtime/gfx_scaled.c`: `sx_scaled_presenter`, which presents onto a POSIX window |
| System 3D edges (SXCHROME) | `sxchrome.c`, `sxchrome.h` | — |
| Widget toolkit (SXGUI-C) | `sxgui.c`, `sxgui.h` | `runtime/sxgui_app.c`: the application frame over a POSIX window, and the toolkit's POSIX host |
| Window client | — | `runtime/gfx.c` + `gfx_impl.inc`: `gfx_open`, `gfx_present`, `gfx_poll_event`, owned windows, launching |

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

A SUN program has no host yet. The first SUN program that uses the toolkit
brings two things: a SUN host (`sxgui_set_host`), and an application frame
over `sxnwm`, the counterpart of `sxgui_app.c`. That follows rule 1 of
[SUN.md](../userland/SUN.md#the-direction-the-systems-own-api): a call lands
with its consumer.

Dialogs already work the same way: `struct sxgui_dialog_host` gives them a
window of their own, and `sxgui_app` is the host that provides it.

## The window protocol

The same reasoning moved the WM protocol out of the POSIX SDK:
`include/wm/wm_protocol.h` (the port name and connect timeout, and the
protocol's description) and `include/wm/wm_shell_protocol.h` (the shell's
window list, whose structs it defines itself; the client protocol's structs
are in `abi/savanxp_abi.h`). SUN's `sxnwm.h` and
the native services include `wm/` directly; the POSIX SDK keeps
`savanxp/wm_protocol.h` and `savanxp/wm_shell_protocol.h` as forwarding
headers.

## Builds that list the units by hand

CMake builds the static runtime and the shared libraries from one list
(`SAVANXP_USER_RUNTIME_SOURCES`, `SAVANXP_LIBRARY_REPLACES_*`, whose entries
are repository paths). Three scripts outside CMake compile runtime units one by
one and have to follow any move: `tools/build-user.sh`,
`ports/doomgeneric/build.sh` and `ports/ccleste/build.sh`.
