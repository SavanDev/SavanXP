# SUN — Savan Userland Native

> Status: **named 2026-10-21; first application built.** SUN is the userland
> native API: what a program that does not use POSIX writes against. The
> Command Prompt ([CONSOLE.md](../desktop/CONSOLE.md)) is its first application
> and the consumer that drove slices 20-22 and the windowing additions.

## What SUN is

SavanXP has two native APIs, one on each side of the system call:

| Layer | Name | Where | Windows analogue |
|---|---|---|---|
| Kernel native API | **Sx** | `sx::` inside the kernel, `SXN_SYS_*` numbers in `include/abi/savanxp_native_abi.h` | NT (`Nt*` system services) |
| Userland native API | **SUN** | `include/sxn/*.h`, `system/runtime/sxn_runtime.c` | Win32 (`kernel32`, `user32`) |
| Translated subsystem | **POSIX** | `subsystems/posix` | the POSIX/Interix subsystem |

Sx is the contract the kernel keeps: numbers, structs and semantics
([KERNEL_NATIVE_API.md](../kernel/KERNEL_NATIVE_API.md)). SUN is what a
program includes: typed calls over Sx and the conventions that turn them into
an application platform. A SUN program is a native program
(`EI_OSABI=0x53`); how to build, stamp and launch one is in
[NATIVE_APPS.md](NATIVE_APPS.md).

The identifiers keep the `sxn_` prefix (`sxn_spawn`, `sxnwm_connect`). SUN is
the name of the layer, not a rename of its symbols.

## The pieces

| Piece | Header / source | Role | Windows analogue |
|---|---|---|---|
| Base | `include/sxn/sxn.h` | processes, handles, events, waits, sections and views, pipes, devices, ports, the clock | `kernel32` |
| Windowing | `include/sxn/sxnwm.h` | connect to `windowd`, present the whole surface or rectangles, read input from the ring, ask for a size, follow resizes and closing, publish the title and icon | `user32` (window + message loop) |
| Widget seed | `include/sxn/sxnwidgets.h` | Label, Button, Field with a 5x7 font; a proof, not a toolkit | — |
| Runtime | `system/runtime/sxn_runtime.c` | start-up, `malloc` over SXN sections ([NATIVE_HEAP.md](NATIVE_HEAP.md)) | the CRT |
| Resources | `<program>.sunres.h`, generated | the program's `.sxres` compiled in: title, accent, window style, 16x16 icon | `.rsrc`, `LoadIcon` |
| Shared rendering | SXGFX, SXCHROME, SXGUI-C | 2D painting, system bevels, the Win9x toolkit; call-free code, usable from SUN | GDI, `DrawEdge`, `comctl32` |

The rendering libraries are not SUN: they belong to the platform and both
subsystems link them. SXGFX and SXCHROME make no system call at all, so they
work unchanged in a native program; the Command Prompt draws its text with the
painter's console face and its scroll bar with SXCHROME. SXGUI-C makes three
(clipboard read and write, and the uptime for double clicks) through POSIX, so
a SUN program cannot use the toolkit yet: the first one that needs it brings a
SUN implementation of those three, by rule 1.

### Resources compiled in

A POSIX program's window reads its name and icon from its own `.sxe` at start
(the `.sxmeta` and `.sxicon` sections). A SUN program cannot: SUN has no files.
So the build compiles them in. `tools/gen_sxe_resources.py --native-program`
writes `<program>.sunres.h` next to the `.sxe` resources for every program CMake
registers as `NATIVE`, defining `sun_resources`, a `struct
sxnwm_presentation_source`; the program includes it and publishes it with
`sxnwm_set_presentation`. The `.sxe` sections are still stamped too, for the
launcher and anything else that scans binaries.

## The direction: our own Win32

SUN grows into a Win32 of our own: the same *shape* — handles, waitable
objects, processes started with explicit standard handles, a console that is
a separate program from the shell — with our names and our error space. It is
not a Win32 clone and it will not run Windows binaries.

Rules that keep it honest:

1. **Every call has a consumer.** A new SUN call lands together with the
   program that needs it and a `nativetest` assertion that proves it. No call
   is added because Win32 has one.
2. **Shape from Win32, names from SavanXP.** When a need has a Win32 answer,
   that answer is the default design (`CreateProcess` with `STARTUPINFO` std
   handles, `CreatePipe`, `WaitForMultipleObjects`, job-style termination).
   The deviation, when there is one, is written next to the call.
3. **Errors stay `-errno`** (`enum sx_status`) until there is a reason for a
   status type; a SUN call returns the value or a negative status.
4. **POSIX keeps working.** SUN stands beside POSIX, not instead of it. A SUN
   program starts POSIX programs and talks to them over pipes; nothing in a
   POSIX program has to know its parent is native.
5. **Apps may be SUN programs.** Until 2026-10-21 only the platform services
   were native ([SYSTEM_LAYERING.md](SYSTEM_LAYERING.md)). An app may now be
   written against SUN when SUN already has what it needs, or when the app is
   the consumer of the slice that adds it. The Command Prompt is the first.

### Win32 map

What exists, what the console adds, and what is still missing:

| Win32 | SUN | Status |
|---|---|---|
| `CreateProcess` | `sxn_spawn` (clones the table), `sxn_spawn_stdio` (stdio only) | done |
| `CreateProcess` + `STARTUPINFO.hStd*` + handle list | `sxn_spawn_ex` (`struct sxn_spawn_request`) | done (slice 21) |
| `CreatePipe` | `sxn_pipe_create` | done (slice 20) |
| `TerminateProcess` / `TerminateJobObject` | `sxn_terminate` (any descendant, not only a child) | done (slice 22) |
| `WaitForSingleObject` / `WaitForMultipleObjects` | `sxn_wait_one`, `sxn_wait_many` (pipes and devices included) | done |
| `GetExitCodeProcess`, Toolhelp | `sxn_process_query` | done |
| `CreateThread`, `ExitThread`, `GetExitCodeThread` | `sxn_thread_create`, `sxn_thread_exit`, `sxn_thread_exit_code`; the stack is the caller's | done (slice 23); threads of one process run in parallel on several cores |
| `CreateEvent`, `SetEvent`, `ResetEvent` | `sxn_event_*` | done |
| `CreateFileMapping`, `MapViewOfFile` | `sxn_section_create`, `sxn_map_view` | done |
| `ReadFile` / `WriteFile` on pipes and devices | `sxn_read`, `sxn_write`, `sxn_set_nonblocking` | done |
| `DuplicateHandle`, `CloseHandle` | `sxn_duplicate`, `sxn_close` | done |
| ALPC ports | `sxn_port_*`, `sxn_pipe_send_handle` | done |
| `QueryPerformanceCounter` | `sxn_clock_ns` | done |
| `CreateWindow` + message loop | `sxnwm_connect`, `sxnwm_poll_input` | done |
| `InvalidateRect` / partial `BitBlt` to screen | `sxnwm_submit_rects`, `sxnwm_wait_idle` | done |
| `SetWindowPos` (size), `WM_SIZE`, `WM_CLOSE` | `sxnwm_request_size`, `sxnwm_check_resize`, `sxnwm_close_requested` | done |
| `SetWindowText`, `WM_SETICON` | `sxnwm_set_title`, `sxnwm_set_presentation` | done |
| `.rsrc` resources | `<program>.sunres.h` (above) | done |
| Console API (`GenerateConsoleCtrlEvent`, `FillConsoleOutputCharacter`) | the console control channel ([CONSOLE.md](../desktop/CONSOLE.md#the-control-channel)) | done |
| Clipboard | `/dev/clipboard` through `sxn_open_device` | missing: no consumer yet |
| `CreateFile` and the file API | — | missing: SUN has no files |
| Registry / profile settings | — | missing (needs files) |
| Owned windows and dialogs | — | missing: SXGUI-C draws dialogs inside the window instead |

## Slices driven by the console

The Command Prompt needs three kernel slices and some userland; each one is
recorded in [KERNEL_NATIVE_API.md](../kernel/KERNEL_NATIVE_API.md) when it
lands.

- **Slice 20 — `SXN_SYS_PIPE_CREATE`.** A native process creates an anonymous
  pipe and gets both ends. Until now the only way was to connect to a port.
- **Slice 21 — `SXN_SYS_SPAWN_EX`.** Start a child with exactly the handles
  the caller names: three standard handles and up to four more, placed at 3,
  4, 5 and 6 in the child, in order. `STARTUPINFO` plus the inherit list, in
  one request struct. Nothing else crosses.
- **Slice 22 — `SXN_SYS_TERMINATE` reaches descendants.** Having started a
  process tree is the right to end any process in it, which is what a job
  object gives Windows. Used for Ctrl+C: the console ends the foreground job
  its shell reports, never the shell itself.
- **Userland — `sxnwm.h`** grew the partial present a terminal needs so that
  a key costs a row and not the window (`sxnwm_submit_rects`), size requests
  and resizes, the close request, and the title and icon
  (`sxnwm_set_title`, `sxnwm_set_presentation`); and the resources compiled in
  (above).

## Not there (deliberately)

Files, a registry, owned windows, the clipboard, the SXGUI-C toolkit, signals (the kernel
has none; see Isolation D in [KERNEL_NATIVE_API.md](../kernel/KERNEL_NATIVE_API.md)),
and a native `fork`. Each one waits for a consumer, by rule 1.
