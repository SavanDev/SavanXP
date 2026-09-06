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
