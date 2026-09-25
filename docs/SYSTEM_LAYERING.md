# System layering model (C + POSIX, and a managed layer deferred to after v1.0)

> Status: **layering fixed (2026-07-21), managed layer deferred
> (2026-09-15).** This document is the **source of truth** for SavanXP's
> language layering. If another document contradicts it, this one wins.

## The rule in one line

**Everything SavanXP ships is written in C against the POSIX SDK
(`subsystems/posix`): the kernel, the drivers, the graphics stack and every
app.** A managed app layer on a VM (the Haxe work in `subsystems/native`)
is **not part of the road to v1.0** — see
[The managed layer is deferred to after v1.0](#the-managed-layer-is-deferred-to-after-v10),
which governs the rest of this document.

The layering that does apply today is about *roles*, not languages: the
platform (kernel, drivers, `compositord`, `windowd`, SXGFX, SXGUI-C) is one
thing, and the apps that consume it are another. An app never reimplements a
piece of the platform, and the platform never grows a dependency on an app.

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

  ┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈
  ┊  Managed layer (Haxe on a VM) — DEFERRED until after v1.0    ┊
  ┊  subsystems/native: validated experiment, nothing ships on it┊
  ┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈
```

## The managed layer is deferred to after v1.0

> Status: **decision fixed (2026-09-15).** It supersedes, for everything up to
> v1.0, the parts of this document that read as "user apps are written in Haxe
> wherever possible".

**Until v1.0, everything is C and POSIX.** The managed layer is not being built,
not being designed and not being planned around. The question of whether SavanXP
wants one — and of what it would be made — is **reopened from scratch after
v1.0**; nothing written here or in `subsystems/native` commits the answer to
HashLink, to reflaxe.CPP or even to Haxe.

**Why it is deferred and not cancelled.** A runtime with a GC in the middle of a
system whose kernel, WM, drivers and apps are still moving multiplies the surface
that has to be kept working, and it buys a user exactly nothing before v1.0: the
apps that exist are the ones that make the system usable, and they are already
written. Deciding it now would mean designing an app model for a platform whose
shape is not final yet.

**If it turns out to be needed, the shape is a selective port, UWP-style.** Not a
migration: a *second* app model that coexists with the native one. Win32 did not
go away when UWP arrived, and neither do SXGUI-C and the C apps here — they stay
canonical, and only what is actually worth moving moves, one app at a time. Two
consequences follow, and they are the ones to check a future design against:

- The managed layer must be an **addition**. No existing app may have to be
  rewritten, recompiled or relinked for it to exist, and nothing in the boot path
  may come to depend on it.
- The platform is consumed, never duplicated. A managed app binds SXGUI-C through
  an FFI; a second toolkit written in the managed language is what this document
  has said from the start it will not have.

**What that means for work done today** — this is the operative part:

- A new app goes in **C against the POSIX SDK**, whatever kind of app it is. The
  first game (`/bin/mines`) is the reference case, not an exception.
- Nothing in the image may depend on `subsystems/native`. It stays in the tree as
  a **validated experiment** — the ABI, the AOT chain and the GUI demo all work
  and are worth keeping — but it is not maintained as a product, it is not in
  the main CMake path, and its binaries are not installed.
- Do not design platform interfaces "for the VM". An API that exists today
  because a future runtime might want it is an API nobody is using.

## What goes in C (all of it, today)

The platform, always:

- **Kernel, drivers, HAL** (display/audio/GPU/network/storage).
- **`compositord`** — owner of the GPU and of the single display surface.
- **`windowd`** — the window manager (extracted from `desktop.c`; see
  [WM_SUBSYSTEM.md](WM_SUBSYSTEM.md)).
- **SXGFX** — the SDK's 2D rasterization layer
  (`subsystems/posix/sdk/v1/.../gfx2d.*`): surfaces, painter, blit, clipping,
  damage. The analogue of GDI; the gaps against GDI32 and the order for closing
  them are in [SXGFX_ROADMAP.md](SXGFX_ROADMAP.md).
- **SXCHROME** — the system's 3D edges and the disabled relief
  (`subsystems/posix/sdk/v1/.../sxchrome.*`), between SXGFX and SXGUI-C. Not
  part of the toolkit because the toolkit is not the only thing that paints
  chrome: `windowd` and any app drawing its own content need the same bevels,
  and `windowd` deliberately does not link SXGUI-C. It ships in the base
  runtime of every userland binary, so nothing has to opt in.
- **SXGUI-C** — the Win9x widget toolkit on top of SXGFX and SXCHROME
  (`subsystems/posix/sdk/v1/.../sxgui.*`). The analogue of USER/comctl32.

And the apps, all of them, against the POSIX SDK:

- **Shell** — the desktop (`subsystems/posix/userland/shellui.c`, `taskbar.c`,
  `progman.c`): taskbar, launcher, icons, wallpaper. Client of `windowd` since
  the WM extraction, **and in C**.
- **File manager** — `subsystems/posix/userland/filesapp.c`.
- **Notepad**, **Calculator**, **Task Manager**, **Minesweeper** — and whatever
  comes next, including games, which used to be the clearest candidate for the
  managed layer.

> The list grows; the language does not change with it. Deciding per app what it
> is written in is exactly the fork this document exists to close.

### An in-tree app has floating point

The in-tree CMake userland target compiles the whole userland with
`-msse -msse2`, and every program links the libm in `runtime/math.c`. A
`double` and a `%f` work in any app of the tree, with no switch to remember.

What that rests on is the kernel, and it was already there for the native
subsystem: SSE is enabled at boot, the FPU/SSE area travels per process and the
scheduler saves and restores it with `fxsave64`/`fxrstor64` on the real context
switch (`kernel/process.cpp`); `fork` copies it, so the child inherits the
floating-point registers the same way it inherits the integer ones. `crt0.S`
leaves `rsp` aligned to 16 and `sx_malloc` returns 16-aligned memory, which is
what SSE's aligned accesses need.

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

## What a managed layer would have to respect (after v1.0)

Nothing in this section is being built. It is the **shape** any future managed
layer has to fit, written down while the reasons are fresh, so that the question
reopened after v1.0 starts from a design and not from a blank page.

- **SXGUI-C is the canonical, permanent toolkit.** The C apps use it directly; a
  managed app would **bind** it through the runtime's FFI. This is the
  **WinForms/JNI** model: a thin managed facade over native controls, not a
  second toolkit. Maintaining two parallel toolkits would be absurd when the C
  one is never going away.
- The hard design piece in that binding is **callback marshalling** (managed
  closures ↔ C function pointers in `sxgui_widget.on_action`), typically through
  a C trampoline with a `void* ctx`. It is the piece to prototype first, because
  it is the one that decides whether the binding is thin.
- It is an **app model, not a migration** (the UWP point above): it ships beside
  the native one or it does not ship.

### Status of the existing Haxe apps

`sxguiapp` (in `subsystems/native/haxe-sxgui`) is a **validation demo** for the
ABI and the AOT chain — **not** a replacement for the C apps, which are the
official ones. The `aboutapp-hx` and `filesapp-hx` ports were retired: they had
served their purpose (proving the Haxe chain reaches a real app) and keeping
them alive duplicated system apps that belong to C in this layering. Likewise,
`haxe-toolkit/` (a reimplementation of Painter/Button/... in Haxe) is a
**bootstrap** that validated the chain — and the clearest example of what a
managed layer must not become: a second toolkit next to SXGUI-C.

## Games, and the first one

A game used to be the clearest candidate for the managed layer — which is not
arriving before v1.0, so the first one ships in C like everything else.
**Minesweeper (`/bin/mines`) is written in C against SXGUI-C**, and it is
nevertheless split the way a managed app would be, which costs nothing and keeps
the seam visible:

- The **rules** live in one module with no window (`mines_board.c`): board
  generation, first-click safety, cascade, chord, win and loss, and the best
  times. It is what a port would carry over unchanged, and what
  `./build.sh smoke mines-smoke` asserts — so the port has a specification, not
  a screenshot, to be judged against.
- The **window** is a thin client of the toolkit (`mines.c`): layout, drawing
  and input, and nothing else. That is the half a managed port would rewrite
  against the FFI.

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

## Where the runtime work stopped

The middle tier was planned in two stages, and it stopped at the first one, which
is where it stays until after v1.0:

1. **Done, and frozen — AOT** with
   [reflaxe.CPP](https://github.com/SomeRanDev/reflaxe.CPP): Haxe → minimal
   C++17 (no GC) → freestanding ELF, running as a real native process with its
   own ABI (`sxn_*`), its own heap and graphics syscalls. It did what it was for:
   it proved the chain reaches a program that puts pixels on screen. The ELFs are
   **validation artifacts** and are not installed.
2. **Not started, and not on the road to v1.0 — the VM** (HashLink was the
   candidate). Reopened after v1.0, with the candidate itself back on the table.

`subsystems/native` is kept, not maintained: it is a result, and deleting a
result to "clean up" would only mean paying for it again. What it must not do is
grow — no new apps, no new ABI surface, no place in the main build.
See [../subsystems/native/README.md](../subsystems/native/README.md) for what
exactly was reached and how it was verified.

## Relationship with the WM extraction

This decision and [WM_SUBSYSTEM.md](WM_SUBSYSTEM.md) are **compatible and
complementary**:

- WM_SUBSYSTEM.md pulls `windowd` (the WM) out of `desktop.c`. That is platform
  work, **in C**, and independent of the clients' language.
- After the extraction, the shell and `progman` **stay in C** — they are not
  rewritten. Nothing in the boot path ever depended on a runtime that is not
  being built, which is the reason the deferral above costs nothing.

## Analogy (to fix the mental model)

| Role | SavanXP | Windows | Android |
|---|---|---|---|
| 2D rasterization | **SXGFX** | GDI32 | Skia / hwui |
| System chrome (3D edges) | **SXCHROME** | DrawEdge / DrawFrameControl | (native) |
| Control toolkit | **SXGUI-C** | USER32 / comctl32 | (native) |
| Window manager | **windowd** | win32k / USER | WindowManager / SurfaceFlinger |
| Display server | **compositord** | DWM | SurfaceFlinger |
| Shell / launcher | **shellui + taskbar + progman** (C) | explorer.exe | SystemUI / Launcher |
| Apps | **C on the POSIX SDK** | Win32 | (native) |
| Managed runtime | *deferred to after v1.0* | CLR | ART |
| Managed apps | *deferred; UWP-style beside the native ones* | .NET / UWP | Java/Kotlin |
