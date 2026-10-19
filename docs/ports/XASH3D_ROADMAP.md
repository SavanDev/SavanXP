# Xash3D FWGS — the port that pays for `dlopen` and SxGL

> **Status: step 1 (`dlopen`) landed; the port itself is not started.** This
> document is the order of work, written down before the work so that each piece
> arrives with a consumer instead of ahead of one.

[Xash3D FWGS](https://github.com/FWGS/xash3d-fwgs) is a GoldSrc-compatible engine:
it runs Half-Life and its mods. It is worth porting for the game, and worth more
for what it forces the system to grow. This document records which missing pieces
it actually needs, which it only exercises, and which it does not touch, so that
"the port needs it" is never used to justify something it does not.

## What the engine needs from the platform

Measured against the upstream tree, not remembered:

- **Run-time library loading, by name.** Everything except the engine core is a
  module loaded with `dlopen`: the renderer (`ref_soft` or `ref_gl`), the
  filesystem (`filesystem_stdio`), the menu, the client (`client.so`) and the game
  logic (`hl.so`). Upstream's porting guide is explicit that a platform without
  dynamic libraries is unsupported, and that a full-static build conflicts with the
  GPL. `engine/platform/posix/lib_posix.c` calls `dlopen(…, RTLD_NOW)`, `dlsym`,
  `dlclose`, `dlerror` and `dladdr`.
- **`dlsym` that walks the handle's dependency tree.** The engine asks for a symbol
  on the library it opened and expects to find it in that library's dependencies
  too — upstream's own comment says so.
- **`dladdr` with a symbol name.** Save/restore stores function pointers by name
  (`COM_NameForFunction`). Without `dladdr` a save game cannot be written.
- **Per-handle symbol scope.** `client.so` and `hl.so` both carry the HLSDK's
  shared player-movement code (`PM_Move`, `PM_Init`, …). A loader with one global
  namespace would bind one library's calls into the other's copy.
- **No GL.** `ref_soft` is a complete software renderer. The engine draws into a
  pixel buffer and the platform presents it — the same contract `doomgeneric`
  already uses.
- **No SDL.** `engine/platform/` has non-SDL backends — `dos` and `linux`
  (fbdev + evdev + ALSA) — that implement video, input, sound and system hooks
  directly. A SavanXP backend is one more directory next to them.
- **No threads.** The DOS backend has none; sound can be mixed from the main loop.
- **Constructors.** The HLSDK client and server are C++ with global objects
  (`CHud gHUD`, among others), so `dlopen` has to run `DT_INIT_ARRAY`. The C++
  runtime they link against (`operator new`, `__cxa_atexit`, `__dso_handle`) is step
  2's problem: the SDK has no C++ runtime today.

Library names follow upstream's scheme for a non-Win/Linux/Mac OS:
`client_savanxp_amd64.so`, `hl_savanxp_amd64.so`, `menu_savanxp_amd64.so`, which
means adding SavanXP to `public/build.h` and to the HLSDK's library-naming files.

## The order

Each step names its acceptance condition. A step without one is a wish.

### Step 1 — `dlopen` in the loader — **done**

`<dlfcn.h>` in the SDK: `dlopen`, `dlsym`, `dlclose`, `dlerror`, `dladdr`, with
per-handle scopes, `RTLD_LOCAL`/`RTLD_GLOBAL`, `RTLD_NOLOAD`, constructors for what
`dlopen` brings, and a failed `dlopen` that leaves nothing behind. The semantics and their limits are in
[SHARED_LIBRARIES.md](../kernel/SHARED_LIBRARIES.md#dlopen).

**Done when:** `dltest` proves that two libraries defining the same symbol stay
apart, that `dlsym` reaches a dependency, that a constructor ran, that `dladdr`
names a function, and that
a broken library fails with its symbol in `dlerror()` and leaves the slot count
unchanged. It does, in the `dltest` smoke scenario.

It is the step everything else rests on, and the only one that is useful even if
the port is abandoned: the next port with plugins needs exactly this.

### Step 2 — the engine with `ref_soft` and a SavanXP backend

`ports/xash3d/`, following the [port layout](../../ports/README.md): pinned
upstream, small patches, the backend in `overlay/`.

- `engine/platform/savanxp/`: video over `gfx_open`/`gfx_present_region`, input
  from the window's key and mouse events, sound through `sx_audio_mixer`, and the
  system hooks.
- `hlsdk-portable` built with the same SDK, producing the client and server
  libraries.
- The game data (`valve/`) is the user's, installed under `/disk/games`, like the
  Doom IWAD. Nothing proprietary is vendored or built.

Build the dedicated server first, as upstream's porting guide recommends: it proves
`dlopen` and the network stack with no video, input or sound in the way.

**Done when:** the client reaches the main menu and loads a map with `ref_soft`,
and a save game round-trips (which is the `dladdr` check, end to end).

### Step 3 — SxGL, with `ref_gl` as its consumer

[SXGL_ROADMAP.md](../desktop/SXGL_ROADMAP.md) batches 1–4. `ref_gl` speaks
fixed-function GL 1.x — the subset that roadmap already chose — and needs what its
batches 3 and 4 list: texturing, blending, alpha test, fog.

The order is the point. With step 2 done, a broken frame under `ref_gl` has a
working reference to compare against pixel for pixel, and the port is already
known to be good. Started first, every bug would be either the rasterizer or the
port, with nothing to tell them apart.

**Done when:** a map renders under `ref_gl` and matches `ref_soft` closely enough
to be the same scene, with the triangles-per-second figure the SxGL roadmap asks
for recorded.

### Step 4 — threads, then SDL, as their own project

Neither is on this port's path, and that is why they are last.

- **Threads are kernel work** — scheduling, TLS, a futex or equivalent, a C runtime
  that is safe to share. [SMP_ROADMAP.md](../kernel/SMP_ROADMAP.md) records that
  user space has none today and leans on that. A consumer that runs fine on one
  thread cannot validate them.
- **SDL** would unlock the largest number of future ports, and would let this one
  drop its custom backend. But SDL2's audio runs on a thread of its own, so SDL
  depends on threads, and putting it on this path would tie the port to the kernel
  work.

**Done when:** each has a consumer that needs it. When both exist, the port can
move to upstream's SDL backend and delete its own.

## What not to do

- **Do not start with SxGL.** Two large projects debugged against each other.
- **Do not build a full-static engine** with the game libraries linked in. Upstream
  does not support it, it conflicts with the GPL, and it would leave `dlopen`
  unbuilt — the piece of this work that outlives the game.
- **Do not add threads or SDL "for the port".** It does not need them.
