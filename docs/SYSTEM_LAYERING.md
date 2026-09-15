# System layering model (native C + Haxe apps on a VM)

> Status: **decision fixed (2026-07-21).** This document is the **source of
> truth** for SavanXP's language layering. If another document contradicts it,
> this one wins.

## The rule in one line

**The low-level core and the system programs are written in C. The rest of the
user apps are written in Haxe wherever possible, running on the VM
(HashLink).**

This is the **Android/ART** model: a mature native core, with a managed app
layer on top. It is not "replace C with Haxe" — it is "C is the platform, Haxe
is the application layer".

## The three layers

```
  ┌─────────────────────────────────────────────────────────────┐
  │  User apps (Haxe)                                            │
  │  run on the VM; bind SXGUI-C through the FFI                 │
  └─────────────────────────────────────────────────────────────┘
                              ▲
                              │  FFI / interop (sxn_*), stable native ABI
                              ▼
  ┌─────────────────────────────────────────────────────────────┐
  │  Intermediate runtime (staged: AOT reflaxe.CPP today → VM)   │
  │  SavanXP's "CLR/ART": runtime + GC + interop to C            │
  └─────────────────────────────────────────────────────────────┘
                              ▲
                              │  native ABI v1 (savanxp_native_abi.h)
                              ▼
  ┌─────────────────────────────────────────────────────────────┐
  │  Native platform (C)                                         │
  │  kernel · drivers · compositord · windowd · SXGFX · SXGUI-C  │
  │  + system apps: shell · file manager · task manager          │
  └─────────────────────────────────────────────────────────────┘
```

## What goes in C (platform + system apps)

Low level, always C:

- **Kernel, drivers, HAL** (display/audio/GPU/network/storage).
- **`compositord`** — owner of the GPU and of the single display surface.
- **`windowd`** — the window manager (extracted from `desktop.c`; see
  [WM_SUBSYSTEM.md](WM_SUBSYSTEM.md)).
- **SXGFX** — the SDK's 2D rasterization layer
  (`subsystems/posix/sdk/v1/.../gfx2d.*`): surfaces, painter, blit, clipping,
  damage. The analogue of GDI; the gaps against GDI32 and the order for closing
  them are in [SXGFX_ROADMAP.md](SXGFX_ROADMAP.md).
- **SXGUI-C** — the Win9x widget toolkit on top of SXGFX
  (`subsystems/posix/sdk/v1/.../sxgui.*`). The analogue of USER/comctl32.

System programs, in C **at this initial stage**:

- **Shell** — the desktop (`subsystems/posix/userland/desktop*.c`): taskbar,
  start menu, icons, wallpaper. After the WM extraction it becomes
  `shell-client`, a client of `windowd`, **but it stays in C**.
- **File manager** — `subsystems/posix/userland/filesapp.c`.
- **Task manager** — does not exist yet; it will be written in C (the `ps.c`
  CLI can seed it).

> More system programs may be added in C in the future if needed. The list
> above is this stage's minimum, not a ceiling.

### An in-tree app has no floating point

`Get-UserFlags` (`build.ps1`) compiles the whole in-tree userland with
`-mno-sse -mno-sse2 -mgeneral-regs-only`. That is not an oversight: without SSE
there is no floating-point ABI to preserve, so the kernel does not have to save
and restore FPU/SSE state for these processes. The consequences are concrete —
a `double` does not compile, and `%f` in `printf`/`snprintf` is behind
`#if defined(__SSE2__)` in `runtime/posix.c`, so it silently prints nothing.

A program that needs real numbers has two ways out, and they are not
interchangeable:

- **Integer arithmetic**, for a system app that must stay in the image. The
  reference case is the calculator (`subsystems/posix/userland/calc.c`): a
  decimal float of 16 significant digits, mantissa in `int64_t` and exponent of
  ten, with the intermediates in `__int128`. Decimal because a calculator is
  read by a person — `0.1 + 0.2` has to be `0.3` — and integer because of the
  flags above. Note that 128-bit **division** is not free: the compiler resolves
  it with `__divti3` from compiler-rt, which this system does not link, so
  `calc.c` does the long division by hand.
- **An external app built with `-Sse`** (`tools/build-user.ps1 -Sse`), which
  gets the hardware unit and the libm in `runtime/math.c`. It is the path of
  `sdk/floatsmoke` and of the ported programs, and it leaves the main build:
  the binary lives in `/disk/bin`, not in the image's `/bin`.

## What goes in Haxe (user apps, via the VM)

Everything else, **wherever possible**, is written in Haxe and runs on the VM.
The Haxe layer **does not reimplement** the platform: it consumes it.

- **SXGUI-C is the canonical, permanent toolkit.** The C system apps use it
  directly; the Haxe apps **bind** it through the VM's FFI. This is the
  **WinForms/JNI** model: a thin managed facade over native controls, not a
  second toolkit. Maintaining two parallel toolkits would be absurd when the C
  one is never going away.
- The new design piece in the binding is **callback marshalling** (Haxe
  closures ↔ C function pointers in `sxgui_widget.on_action`), typically
  through a C trampoline with a `void* ctx`.

### Status of the existing Haxe apps

`sxguiapp` (in `subsystems/native/haxe-sxgui`) is a **validation demo** for the
ABI and the AOT chain — **not** a replacement for the C apps, which are the
official ones. The `aboutapp-hx` and `filesapp-hx` ports were retired: they had
served their purpose (proving the Haxe chain reaches a real app) and keeping
them alive duplicated system apps that belong to C in this layering. Likewise,
`haxe-toolkit/` (a reimplementation of Painter/Button/... in Haxe) is a
**bootstrap** that validated the chain, not the end state: the end state is the
binding to SXGUI-C.

### Games, and the first one

A game is a **user app** by the rule above, so its natural home is the Haxe
layer. The first one nevertheless ships in C, and the reason is the stage, not a
change of mind: **the VM does not exist yet**. Putting a program someone plays on
top of a bootstrap runtime would make the program hostage to work that is still
being designed — and the AOT ELFs are validation artifacts, by this document's
own words.

So **Minesweeper (`/bin/mines`) is written in C against SXGUI-C**, and it is
deliberately built in the *shape* of a Haxe app rather than of a system program:

- The **rules** live in one module with no window (`mines_board.c`): board
  generation, first-click safety, cascade, chord, win and loss, and the best
  times. It is what a port would carry over unchanged, and what
  `.\build.ps1 mines-smoke` asserts — so the port has a specification, not a
  screenshot, to be judged against.
- The **window** is a thin client of the toolkit (`mines.c`): layout, drawing
  and input, and nothing else. That is the half a Haxe app would rewrite against
  the FFI.

What this does **not** mean: the game is not part of the platform. Nothing in the
kernel, `windowd`, `compositord` or the SDK may grow a dependency on it — it is
an app that consumes the platform, exactly like Doom does, with the single
difference that it **comes in the image** instead of being installed into
`/disk/bin`. Being preinstalled is a packaging fact (an entry in `build.ps1` and
`category=Games` in its `.sxres`), not a layer.

The one thing the game did push into the platform is the gap it found in the
toolkit: an app that paints its own content — a board of cells, a grid, a canvas
— needs the **system** 3D edges, not a private copy of them, which is why
`sxgui_draw_raised_edge()` / `sxgui_draw_sunken_edge()` are public. The rule
behind it is the one this document already states: SXGUI-C is the canonical
toolkit, and an app never reimplements a piece of it.

## Runtime stages (the middle tier)

SavanXP's "CLR/ART" is built in stages; the native ABI is designed **once** and
both stages target the same contract:

1. **Today — AOT** with [reflaxe.CPP](https://github.com/SomeRanDev/reflaxe.CPP):
   Haxe → minimal C++17 (no GC) → freestanding ELF. It served to validate the
   chain and design the ABI. The AOT ELFs are **validation artifacts**, not the
   final product.
2. **Target — VM (HashLink)** in the OS: HL's runtime/GC is ported onto the
   native ABI and the Haxe apps run on the VM. `sxn_*` and the ABI do not
   change.

See [../subsystems/native/README.md](../subsystems/native/README.md) for the
phase and verification details.

## Relationship with the WM extraction

This decision and [WM_SUBSYSTEM.md](WM_SUBSYSTEM.md) are **compatible and
complementary**:

- WM_SUBSYSTEM.md pulls `windowd` (the WM) out of `desktop.c`. That is platform
  work, **in C**, and independent of the clients' language.
- After the extraction, `shell-client` (and an eventual `progman`) **stay in
  C** — they are not rewritten in Haxe. That removes any concern about putting
  the Haxe runtime in the critical boot path.

## Analogy (to fix the mental model)

| Role | SavanXP | Windows | Android |
|---|---|---|---|
| 2D rasterization | **SXGFX** | GDI32 | Skia / hwui |
| Control toolkit | **SXGUI-C** | USER32 / comctl32 | (native) |
| Window manager | **windowd** | win32k / USER | WindowManager / SurfaceFlinger |
| Display server | **compositord** | DWM | SurfaceFlinger |
| Shell / launcher | **desktop\*.c** (C) | explorer.exe | SystemUI / Launcher |
| Managed runtime | **reflaxe.CPP → HashLink** | CLR | ART |
| User apps | **Haxe / VM** | .NET (WinForms) | Java/Kotlin |
