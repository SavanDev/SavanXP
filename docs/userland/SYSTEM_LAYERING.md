# System layering model (C + POSIX)

> Status: **layering fixed (2026-07-21), Haxe experiment removed
> (2026-10-15).** This document is the **source of truth** for SavanXP's
> layering. If another document contradicts it, this one wins.

## The rule in one line

**Everything SavanXP ships is written in C against the POSIX SDK
(`subsystems/posix`): the kernel, the drivers, the graphics stack and every
app.**

The layering is about *roles*, not languages: the platform (kernel, drivers,
`compositord`, `windowd`, SXGFX, SXGUI-C) is one thing, and the apps that
consume it are another. An app never reimplements a piece of the platform,
and the platform never grows a dependency on an app.

## The two layers

```
  ┌─────────────────────────────────────────────────────────────┐
  │  Apps (C, POSIX SDK v1)                                      │
  │  shell · files · notepad · calculator · task manager · games │
  │  paint with SXGUI-C / SXGFX, talk to windowd over the WM     │
  │  protocol, and to the kernel through the POSIX syscalls      │
  └─────────────────────────────────────────────────────────────┘
                              ▲
                              │  POSIX SDK v1 (subsystems/posix/sdk/v1)
                              ▼
  ┌─────────────────────────────────────────────────────────────┐
  │  Native platform (C)                                         │
  │  kernel · drivers · compositord · windowd                    │
  │  SXGFX · SXCHROME · SXGUI-C                                  │
  └─────────────────────────────────────────────────────────────┘
```

## The Haxe experiment is removed

> Status: **removed (2026-10-15).** It replaces the
> "managed layer deferred to after v1.0" decision (2026-09-15).

There is no managed app layer, no VM, and no `subsystems/native` tree. A new
app goes in **C against the POSIX SDK**, whatever kind of app it is. Platform
interfaces are designed for the C apps that exist, not for a runtime that is
not being built.

## Direction: NT-style kernel API, Win32-like userland API

> Status: **built (2026-10-16, slices 1-13 plus isolation A-E).** What follows
> describes what landed, not what is planned. See
> [`KERNEL_NATIVE_API.md`](../kernel/KERNEL_NATIVE_API.md) for the slice record
> and [`NATIVE_APPS.md`](NATIVE_APPS.md) for writing native programs.

The shape that was fixed in advance is what shipped:

- The **kernel** exposes its own **native API, NT-style**: objects, handles,
  sections/views, waits, processes, VM and I/O. Subsystems talk to the kernel
  through it (`sx::` surface plus the `SXN_SYS_*` dispatcher at `0x1000+`).
- **POSIX is one subsystem** on top of that API: a translation layer
  (`subsystems/posix/kernel/posix_translation.cpp`), not the shape of the
  kernel. The kernel includes no userland header and keeps no POSIX
  per-process state.
- The **userland native API, Win32-like**, stands beside POSIX, not instead
  of it: header-only `sxn.h` / `sxnwm.h` / `sxnwidgets.h` over the kernel
  native API, proven by `sdk/nativetest`, `sdk/nativedemo` and
  `sdk/nativewidgets`. Nothing existing was rewritten for it to exist, and
  nothing in the boot path depends on it.

## What goes in C (all of it, today)

The platform, always:

- **Kernel, drivers, HAL** (display/audio/GPU/network/storage).
- **`compositord`** — owner of the GPU and of the single display surface.
- **`windowd`** — the window manager (extracted from `desktop.c`; see
  [WM_SUBSYSTEM.md](../desktop/WM_SUBSYSTEM.md)).
- **SXGFX** — the SDK's 2D rasterization layer
  (`subsystems/posix/sdk/v1/.../gfx2d.*`): surfaces, painter, blit, clipping,
  damage. The analogue of GDI; the gaps against GDI32 and the order for closing
  them are in [SXGFX_ROADMAP.md](../desktop/SXGFX_ROADMAP.md).
- **SXCHROME** — the system's 3D edges and the disabled relief
  (`subsystems/posix/sdk/v1/.../sxchrome.*`), between SXGFX and SXGUI-C. Not
  part of the toolkit because the toolkit is not the only thing that paints
  chrome: `windowd` and any app drawing its own content need the same bevels,
  and `windowd` deliberately does not link SXGUI-C. It ships in the base
  runtime of every userland binary, so nothing has to opt in.
- **SXGUI-C** — the Win9x widget toolkit on top of SXGFX and SXCHROME
  (`subsystems/posix/sdk/v1/.../sxgui.*`). The analogue of USER/comctl32.

And the apps, all of them, against the POSIX SDK:

- **Shell** — the `desktop` process (`subsystems/posix/userland/desktop/`:
  wallpaper, taskbar, Start menu and the taskbar's popups) and `progman.c`, the
  launcher. Clients of `windowd` since the WM extraction, **and in C**. See
  [DESKTOP_ARCHITECTURE.md](../desktop/DESKTOP_ARCHITECTURE.md).
- **File manager** — `subsystems/posix/userland/filesapp.c`.
- **Notepad**, **Calculator**, **Task Manager**, **Minesweeper** — and whatever
  comes next, including games.

> The list grows; the language does not change with it. Deciding per app what it
> is written in is exactly the fork this document exists to close.

### An in-tree app has floating point

The in-tree CMake userland target compiles the whole userland with
`-msse -msse2`, and every program links the libm in `runtime/math.c`. A
`double` and a `%f` work in any app of the tree, with no switch to remember.

What that rests on is the kernel: SSE is enabled at boot, the FPU/SSE area
travels per process and the scheduler saves and restores it with
`fxsave64`/`fxrstor64` on the real context switch (`kernel/process.cpp`);
`fork` copies it, so the child inherits the floating-point registers the same
way it inherits the integer ones. `crt0.S` leaves `rsp` aligned to 16 and
`sx_malloc` returns 16-aligned memory, which is what SSE's aligned accesses
need.

Two consequences worth keeping in mind:

- **The compiler now emits SSE on its own**, in a vectorized `memcpy` for
  instance, in apps that never mention a `double`. That is fine — the state is
  saved either way — but it is why the userland links with `--gc-sections` and
  compiles with `-ffunction-sections`: without that every binary carried the
  whole libm, and the tree has dozens of programs.
- **The calculator keeps its integer engine** (`subsystems/posix/userland/
  calc.c`): a decimal float of 16 significant digits, mantissa in `int64_t` and
  exponent of ten. That was never only about the flags — a calculator is read by
  a person and `0.1 + 0.2` has to print `0.3`, which binary IEEE-754 does not
  give. Note that 128-bit **division** is not free: the compiler resolves it
  with `__divti3` from compiler-rt, which this system does not link, so `calc.c`
  does the long division by hand.

An **external** app still opts in with `--sse` (`tools/build-user.sh --sse`): a
port that does not use real numbers prefers the compiler to leave SSE out of it.
`sdk/floatsmoke` is the harness of that path, and of the kernel keeping the
FPU state straight while it multiplexes processes doing math at the same time.

## Games, and the first one

**Minesweeper (`/bin/mines`) is written in C against SXGUI-C**, and it is
split so the rules stay portable:

- The **rules** live in one module with no window (`mines_board.c`): board
  generation, first-click safety, cascade, chord, win and loss, and the best
  times. It is what a port would carry over unchanged, and what
  `./build.sh smoke mines-smoke` asserts — so the port has a specification, not
  a screenshot, to be judged against.
- The **window** is a thin client of the toolkit (`mines.c`): layout, drawing
  and input, and nothing else.

What this does **not** mean: the game is not part of the platform. Nothing in the
kernel, `windowd`, `compositord` or the SDK may grow a dependency on it — it is
an app that consumes the platform, exactly like Doom does, with the single
difference that it **comes in the image** instead of being installed into
`/disk/bin`. Being preinstalled is a packaging fact (an entry in the CMake userland
registry and `category=Games` in its `.sxres`), not a layer.

The one thing the game did push into the platform is the gap it found in the
toolkit: an app that paints its own content — a board of cells, a grid, a canvas
— needs the **system** 3D edges, not a private copy of them, which is why
`sxgui_draw_raised_edge()` / `sxgui_draw_sunken_edge()` are public. The rule
behind it is the one this document already states: SXGUI-C is the canonical
toolkit, and an app never reimplements a piece of it.

Exporting them from the toolkit turned out to be only half the fix. The same
private copies kept appearing where the toolkit could not reach: `calc` and
`taskmgr` had one each, and `windowd` had two — and the WM could not just call
the toolkit, because the edges lived inside the monolithic `sxgui.c` and linking
it would have dragged menus, listbox and textedit into the window manager for
eight lines of bevel. So the edges moved **down**, to SXCHROME, and everyone
consumes them from there — the toolkit included.

The primitive still takes its four colours as parameters, so a caller with a
custom scheme can reuse the algorithm without being forced into the system
palette. System chrome, however, now shares one Windows Standard scheme: the
window frame uses the same face and bevel tones as a control, while active and
inactive captions use the matching system gradients.

## Relationship with the WM extraction

This decision and [WM_SUBSYSTEM.md](../desktop/WM_SUBSYSTEM.md) are **compatible and
complementary**:

- WM_SUBSYSTEM.md pulls `windowd` (the WM) out of `desktop.c`. That is platform
  work, **in C**, and independent of the clients' language.
- After the extraction, the shell and `progman` **stay in C** — they are not
  rewritten. Nothing in the boot path depends on a runtime that is not
  being built.

## Analogy (to fix the mental model)

| Role | SavanXP | Windows | Android |
|---|---|---|---|
| 2D rasterization | **SXGFX** | GDI32 | Skia / hwui |
| System chrome (3D edges) | **SXCHROME** | DrawEdge / DrawFrameControl | (native) |
| Control toolkit | **SXGUI-C** | USER32 / comctl32 | (native) |
| Window manager | **windowd** | win32k / USER | WindowManager / SurfaceFlinger |
| Display server | **compositord** | DWM | SurfaceFlinger |
| Shell / launcher | **desktop + progman** (C) | explorer.exe | SystemUI / Launcher |
| Apps | **C on the POSIX SDK** | Win32 apps | (native) |
| Kernel native API | **`sx::` + SXN syscalls** | NT | (native) |
| Userland native API | **`sxn.h** (process/memory/sync; no toolkit yet) | Win32 (subset) | Java/Kotlin |

The desktop rows describe today. The target is in
[DESKTOP_ARCHITECTURE.md](../desktop/DESKTOP_ARCHITECTURE.md): `compositord`
takes over composition (today it only presents what `windowd` composed), and
`compositord` and `windowd` become native processes, while the shell and the
apps stay clients in whatever subsystem they run in.
