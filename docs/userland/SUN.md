# SUN — Savan Userland Native

> Status: **named 2026-10-21; first application built.** SUN is the userland
> native API: what a program that does not use POSIX writes against. The
> Command Prompt ([CONSOLE.md](../desktop/CONSOLE.md)) is its first application
> and the consumer that drove slices 20-22 and the windowing additions.
> **First game 2026-10-29:** Nightwood ([NIGHTWOOD.md](NIGHTWOOD.md)), drawn
> with SxGL, which is platform code so that a SUN program can link it.
> **Direction clarified 2026-10-24:** SUN is the system's own API, not a
> replica of Win32 ([below](#the-direction-the-systems-own-api)).

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

**A SUN program is always an SXE image, named `<program>.sxe` on disk, and
nothing else runs as SUN.** Foreign formats go to their own subsystem. The
loader still decides by `EI_OSABI` alone; the name is a rule of the build and
the tooling. The platform services are SUN programs too, and they show that
the menu metadata is optional: what makes a program SUN is the stamp and the
name, not a `category`. The rule and how it is enforced are in
[SXE_FORMAT.md](SXE_FORMAT.md#sun-programs-are-named-sxe).

The Windows column is an analogy of *role*: SUN sits where Win32 sits in
Windows, as the API the system itself is written against. It says nothing about
what SUN's calls look like.

The identifiers keep the `sxn_` prefix (`sxn_spawn`, `sxnwm_connect`). SUN is
the name of the layer, not a rename of its symbols.

## The pieces

| Piece | Header / source | Role | Windows analogue |
|---|---|---|---|
| Base | `include/sxn/sxn.h` | processes, handles, events, waits, sections and views, pipes, devices, ports, the clock | `kernel32` |
| Windowing | `include/sxn/sxnwm.h` | connect to `windowd`, present the whole surface or rectangles, read input from the ring, ask for a size, follow resizes and closing, publish the title and icon, capture the pointer | `user32` (window + message loop) |
| Sound | `include/sxn/sxnaudio.h` | a PCM stream to `audiod` through its native port, or to `/dev/audio0` directly when no daemon runs, paced by the clock | `waveOut`, XAudio2's source voice |
| Widget seed | `include/sxn/sxnwidgets.h` | Label, Button, Field with a 5x7 font; a proof, not a toolkit | — |
| Runtime | `system/runtime/sxn_runtime.c` | start-up, `malloc` over SXN sections ([NATIVE_HEAP.md](NATIVE_HEAP.md)) | the CRT |
| Resources | `<program>.sunres.h`, generated | the program's `.sxres` compiled in: title, accent, window style, 16x16 icon | `.rsrc`, `LoadIcon` |
| Shared rendering | SXGFX, SXCHROME, SXGUI-C (`system/gfx`, `include/sxgfx`) | 2D painting, system bevels, the Win9x toolkit; call-free code, usable from SUN | GDI, `DrawEdge`, `comctl32` |

The rendering libraries are not SUN: they belong to the platform and both
subsystems link them. SXGFX and SXCHROME make no system call at all, so they
work unchanged in a native program; the Command Prompt draws its text with the
painter's console face and its scroll bar with SXCHROME. SXGUI-C needs three
services from the system (clipboard read and write, and the uptime for double
clicks), and takes them from a `struct sxgui_host`
([PLATFORM_GRAPHICS.md](../desktop/PLATFORM_GRAPHICS.md#the-toolkits-host)). A
POSIX program gets the POSIX host without asking. A SUN program has none yet:
the first one that uses the toolkit brings a SUN host, and the application
frame it needs over `sxnwm`, by rule 1.

### Resources compiled in

A POSIX program's window reads its name and icon from its own `.sxe` at start
(the `.sxmeta` and `.sxicon` sections). A SUN program cannot: SUN has no files.
So the build compiles them in. `tools/gen_sxe_resources.py --native-program`
writes `<program>.sunres.h` next to the `.sxe` resources for every program CMake
registers as `NATIVE`, defining `sun_resources`, a `struct
sxnwm_presentation_source`; the program includes it and publishes it with
`sxnwm_set_presentation`. The `.sxe` sections are still stamped too, for the
launcher and anything else that scans binaries.

## The direction: the system's own API

SUN is what Win32 is to Windows in one sense only: the native API of the
operating system, its own language, and the subsystem that knows the kernel
best. The other subsystems translate a foreign contract onto Sx. SUN has no
foreign contract to translate. It exposes the kernel's objects (handles,
waitable objects, sections, ports, processes with explicit standard handles)
in the form SavanXP programs want.

**SUN is not a replica of Win32.** It does not follow Win32's calls,
structures, names or behaviour. Being "Win32-like" never meant that, and
nothing in SUN exists to make Windows programs easier to run. Running
programs built for other systems is the job of the binary-compatibility
subsystems (Win32 and Linux, planned), which stand on Sx next to POSIX and SUN
([SYSTEM_LAYERING.md](SYSTEM_LAYERING.md#planned-binary-compatibility-subsystems)).

Rules that keep it honest:

1. **Every call has a consumer.** A new SUN call lands together with the
   program that needs it and a `nativetest` assertion that proves it. No call
   is added because another system has one.
2. **Designed from Sx and the consumer.** A SUN call is shaped by what the
   kernel object offers and what the program needs. Other systems' answers
   (Win32, POSIX, Plan 9) are prior art worth reading, not a default to copy.
   Several existing calls ended up close to their Win32 counterparts
   (`sxn_spawn_ex` with explicit standard handles, `sxn_pipe_create`,
   `sxn_wait_many`, job-style `sxn_terminate`) because that answer fit the
   consumer, not to stay compatible with Win32.
3. **Errors stay `-errno`** (`enum sx_status`) until there is a reason for a
   status type; a SUN call returns the value or a negative status.
4. **POSIX keeps working.** SUN stands beside POSIX, not instead of it. A SUN
   program starts POSIX programs and talks to them over pipes; nothing in a
   POSIX program has to know its parent is native.
5. **Apps may be SUN programs.** Until 2026-10-21 only the platform services
   were native ([SYSTEM_LAYERING.md](SYSTEM_LAYERING.md)). An app may now be
   written against SUN when SUN already has what it needs, or when the app is
   the consumer of the slice that adds it. The Command Prompt is the first.

### Win32 counterparts

A reading aid for anyone who knows Windows: the closest Win32 piece to each
SUN call. It is not a checklist. A Win32 row with no SUN call is not a gap by
itself, because SUN gains a call when a consumer needs one (rule 1). Covering
the Win32 API belongs to the planned Win32 subsystem, not to this table.

| Win32 | SUN | Status |
|---|---|---|
| `CreateProcess` | `sxn_spawn` (clones the table), `sxn_spawn_stdio` (stdio only) | done |
| `CreateProcess` + `STARTUPINFO.hStd*` + handle list | `sxn_spawn_ex` (`struct sxn_spawn_request`) | done (slice 21) |
| `CreatePipe` | `sxn_pipe_create` | done (slice 20) |
| `TerminateProcess` / `TerminateJobObject` | `sxn_terminate` (any descendant, not only a child) | done (slice 22) |
| `WaitForSingleObject` / `WaitForMultipleObjects` | `sxn_wait_one`, `sxn_wait_many` (pipes and devices included) | done |
| `GetExitCodeProcess`, Toolhelp | `sxn_process_query` | done |
| `CreateThread`, `ExitThread`, `GetExitCodeThread` | `sxn_thread_create`, `sxn_thread_exit`, `sxn_thread_exit_code`; the stack is the caller's | done (slice 23); threads of one process run in parallel on several cores |
| `NtCurrentTeb` (the TEB at `%gs`), the TLS a `TlsAlloc` slot lives in | `sxn_set_thread_base` (`SXN_THREAD_BASE_GS`, or `_FS` for POSIX-style TLS) | done (slice 24) |
| `WaitOnAddress`, `WakeByAddressSingle`/`All` (and what `CRITICAL_SECTION` and `SRWLOCK` sleep on) | `sxn_wait_address`, `sxn_wake_address` | done (slice 25); same-process only |
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
| `ClipCursor` + raw input (`WM_INPUT`), `ShowCursor(FALSE)` | `sxnwm_set_pointer_capture`, `SAVANXP_WM_EVENT_POINTER_MOTION` | done (Nightwood); suspended by the WM whenever the window is not active |
| `waveOutOpen`/`waveOutWrite`, XAudio2 | `sxnaudio_open`, `sxnaudio_frames_due`, `sxnaudio_write` | done (Nightwood); mixed by `audiod` when it runs ([AUDIO_DAEMON.md](../media/AUDIO_DAEMON.md#sun-clients)) |
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

## What the first game added

Nightwood ([NIGHTWOOD.md](NIGHTWOOD.md)) is the consumer of three additions,
none of them a kernel slice: everything they need was already open to SUN.

- **SxGL as platform code.** The 3D rasterizer makes no system call and is
  presented by the caller (`sxnwm_submit_full` here), so a SUN program links
  it like it links SxGFX ([SXGL.md](../desktop/SXGL.md)).
- **Sound, `sxn/sxnaudio.h`.** audiod gained a native port beside its UDP
  socket, and a SUN stream speaks the same datagrams over it in fixed-size
  records; with no daemon it writes the device. Asserted by `nativetest`
  (both paths) and `nightwood --selftest`.
- **Pointer capture, `sxnwm_set_pointer_capture`.** A request word in the
  surface header and a new event kind; `windowd` grants it only to the active
  window and drops it the moment that changes
  ([WM_SUBSYSTEM.md](../desktop/WM_SUBSYSTEM.md#pointer-capture)). Asserted by
  `windowd --selftest` (the motion of both device kinds) and
  `nightwood --selftest` (the camera it turns).

## Not there (deliberately)

Files, a registry, owned windows, the clipboard, the SXGUI-C toolkit, signals (the kernel
has none; see Isolation D in [KERNEL_NATIVE_API.md](../kernel/KERNEL_NATIVE_API.md)),
and a native `fork`. Each one waits for a consumer, by rule 1.
