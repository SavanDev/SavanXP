# Xash3D FWGS — the port that pays for `dlopen` and SxGL

> **Status: the dedicated server and graphical client run in step 2.**
> The server loads the HLSDK and filesystem modules, starts Half-Life's
> `c0a0` and answers IPv4 UDP queries through POSIX. The client opens MainUI
> and plays `c0a0` with `ref_soft` in a POSIX window. Audio playback remains
> pending.
>
> Reviewed 2026-11-14 against the rest of the system. Since it was written,
> SxGL ([SXGL_ROADMAP.md](../done/SXGL_ROADMAP.md), batches 0–4), user-space
> threads ([THREADS_ROADMAP.md](../done/THREADS_ROADMAP.md), with `pthread.h`)
> and a GPU backend for SxGL ([VIRGL_ROADMAP.md](VIRGL_ROADMAP.md)) have all
> landed, with consumers of their own. Step 3 no longer builds SxGL, it
> consumes it, and step 4 is down to SDL.

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

- **The C++ runtime of the game modules.** `operator new`/`delete`,
  `__cxa_atexit`, `__dso_handle`, `__cxa_pure_virtual` and the guard functions
  for local statics. The SDK still has none: `kernel/runtime.cpp` and
  `kernel/heap.cpp` provide most of them, for the kernel only.

Library names follow upstream's scheme for a non-Win/Linux/Mac OS:
`client_savanxp_amd64.so`, `hl_savanxp_amd64.so`, `menu_savanxp_amd64.so`, which
means adding SavanXP to `public/build.h` and to the HLSDK's library-naming files.

## Why a native port and not the Linux build

[SSL](LINUX_SUBSYSTEM_ROADMAP.md) runs unmodified Linux programs, and upstream
ships a Linux build with an fbdev/evdev/ALSA backend and an SDL one. Running that
build under SSL is **deliberately not the path**, and would not be even once SSL
loads dynamic ELFs (today it runs static musl programs only).

The point of this port is to exercise the system's own stack end to end, with a
real engine as the consumer: `dlopen` and its scopes in the SavanXP loader, the
window and present path, `audiod`, and above all **SxGL** — `ref_gl` is the first
consumer of SxGL that was not written for it, and the first one loaded as a
plugin. Through SSL the game would draw through Linux's interfaces instead, and
none of those pieces would be tested. A Linux Xash3D under SSL can be a test of
SSL some day; it is not a substitute for this port.

## The order

Each step names its acceptance condition. A step without one is a wish.

### Step 1 — `dlopen` in the loader — **done**

`<dlfcn.h>` in the SDK: `dlopen`, `dlsym`, `dlclose`, `dlerror`, `dladdr`, with
per-handle scopes, `RTLD_LOCAL`/`RTLD_GLOBAL`, `RTLD_NOLOAD`, constructors for what
`dlopen` brings, and a failed `dlopen` that leaves nothing behind. The semantics and their limits are in
[SHARED_LIBRARIES.md](../../kernel/SHARED_LIBRARIES.md#dlopen).

**Done when:** `dltest` proves that two libraries defining the same symbol stay
apart, that `dlsym` reaches a dependency, that a constructor ran, that `dladdr`
names a function, and that
a broken library fails with its symbol in `dlerror()` and leaves the slot count
unchanged. It does, in the `dltest` smoke scenario.

It is the step everything else rests on, and the only one that is useful even if
the port is abandoned: the next port with plugins needs exactly this.

### Step 2 — the engine with `ref_soft` and a SavanXP backend

`ports/xash3d/`, following the [port layout](../../../ports/README.md): pinned
upstream, small patches, the backend in `overlay/`.

The first slice builds the pinned HLSDK Portable server library
`hl_savanxp_amd64.so` and loads it into a SavanXP POSIX process. The
`xash3d-module` scenario checks constructors, the game API handshake and
game initialization. The C++ ABI support lives in the port while the server
and client establish what they need from a shared runtime. See the
[port notes](../../../ports/xash3d/README.md).

The next slice builds Xash3D's `filesystem_stdio.so` against the SavanXP POSIX
SDK. `xash3d-filesystem` checks the module's interfaces, game metadata and loose
and PAK reads from SxFS.

The `xash3d-renderer` smoke loads `ref_soft.so` through `GetRefAPI`, renders
color swatches through the software blitter and checks the composited pixels
in a POSIX window. It also checks relative pointer motion, buttons and wheel
input after polling the keyboard on the same window.

- The client backend presents `ref_soft` using `gfx_open` and `gfx_present`
  and reads the window's keyboard and mouse events. Audio playback still
  needs `sx_audio_mixer` and the shared mixer, as in Doom.
  [Audio daemon](../../media/AUDIO_DAEMON.md)
- `ports/xash3d/build.sh --client` builds HLSDK Portable, FreeVGUI and
  MainUI with the POSIX SDK. The port's C++ runtime includes libcxxrt's RTTI
  support for FreeVGUI. `dltest` checks the relocation addends used by C++
  vtables and runs a shared library constructor under `dlopen`.
- The POSIX SDK exposes [window pointer capture](../../desktop/WM_SUBSYSTEM.md#pointer-capture).
  The client uses it for the in-game camera.
- The game data (`valve/`) is the user's, installed under `/games`, like the
  Doom IWAD. Nothing proprietary is vendored or built.

**SxFS v2 inode limit.** A tested `valve/` copy has 4,269 files and 76
directories, which exceeds the 4,096 inodes in a volume even when the data fits
in bytes. The VFS loads and evicts directory entries on demand, so its resident
node limit is a separate constraint. Use the assets needed by a single map to
bring up the dedicated server. For a full install, package static assets such
as `sound/` into an engine-readable `.pak` on the host and keep modules,
configuration and saves as loose files. The POSIX probe mounted a 3,009-entry
sound archive from this copy and read two sounds with matching checksums.
See [SxFS](SXFS_ROADMAP.md#current-baseline-and-the-version-boundary) and
[VFS limits](../../kernel/FILESYSTEMS.md#limits).

`ports/xash3d/build.sh --valve PATH` installs the user's game assets and packs
loose sounds automatically. The client mounts the installed data read-only
and uses the desktop profile for saves and settings.
[Port notes](../../../ports/xash3d/README.md#graphical-client)

The `xash3d-dedicated` smoke starts the dedicated server with user-supplied
data on a disposable disk. It checks the server's UDP reply names `c0a0` in
`valve` and declares dedicated mode. The port uses nonblocking POSIX IPv4
sockets. See the [port notes](../../../ports/xash3d/README.md) for the current
network limits and the test command.

**Client acceptance verified:** MainUI opens, `ref_soft` renders `c0a0`, and
`xash3d-client` checks that the game restores server state and reconnects to
the map after loading the saved slot. The smoke also checks the saved game
and thumbnail headers on disk.

### Step 3 — `ref_gl` on SxGL

SxGL exists ([SXGL.md](../../desktop/SXGL.md)): batches 0–4 of
[SXGL_ROADMAP.md](../done/SXGL_ROADMAP.md) landed with `/bin/gears` and Nightwood
as consumers, and texturing, blending, alpha test and fog — what `ref_gl` was
expected to need — are in. `ref_gl` speaks fixed-function GL 1.x, the subset SxGL
chose. What remains is what a renderer written elsewhere asks of it, measured
against the reference rather than assumed:

- **Lookup by name.** `ref_gl` is itself a `dlopen`ed module and resolves every GL
  entry point through the engine (`GL_GetProcAddress`). SxGL is part of the
  static user runtime today, so it needs either a shared `libsxgl` or a
  name-to-function table the backend can answer from.
- **The gaps the subset documents.** No points and no lines, one texture unit,
  no texture matrix, textures in powers of two up to 1024. `ref_gl` has fallbacks
  for most of them (two-pass lightmaps without multitexture, resampling of
  non-power-of-two textures); each one it cannot avoid is a batch of SxGL with
  `ref_gl` as its consumer, with the same rule as before: the port asks, SxGL
  grows.
- **Two backends.** SxGL draws on the GPU under virgl and in software elsewhere.
  The comparison against `ref_soft` is made with `SXGL_CONTEXT_SOFTWARE` first,
  so it does not depend on the host GPU; the GPU backend is checked afterwards
  against that frame.

The order is the point. With step 2 done, a broken frame under `ref_gl` has a
working reference to compare against pixel for pixel, and the port is already
known to be good. Started first, every bug would be either the rasterizer or the
port, with nothing to tell them apart.

**Done when:** a map renders under `ref_gl` and matches `ref_soft` closely enough
to be the same scene, with the triangles-per-second figure the SxGL roadmap asks
for recorded (`sxgl_get_stats`), for the software backend and, where available,
the GPU one.

### Step 4 — SDL, as its own project

Threads were the other half of this step; they landed on their own
([THREADS_ROADMAP.md](../done/THREADS_ROADMAP.md), `pthread.h` for POSIX), so the
reason that kept SDL off this path — SDL2's audio runs on a thread of its own — is
gone. SDL is still not on the path: the port does not need it, and its custom
backend is what exercises the system's own interfaces.

- **SDL** would unlock the largest number of future ports, and would let this one
  drop its custom backend.
- **Threads in the engine** (upstream can mix sound or load on a thread) are
  optional. The port starts single-threaded, as the DOS backend is, and takes a
  thread only where a measurement asks for it.

**Done when:** SDL has a consumer that needs it. Then the port can move to
upstream's SDL backend and delete its own.

## What not to do

- **Do not start with SxGL.** Two large projects debugged against each other.
- **Do not build a full-static engine** with the game libraries linked in. Upstream
  does not support it, it conflicts with the GPL, and it would leave `dlopen`
  unbuilt — the piece of this work that outlives the game.
- **Do not add SDL "for the port".** It does not need it, and threads, which
  SDL was waiting for, are not a reason either.
- **Do not run upstream's Linux build under SSL instead.** See
  [Why a native port](#why-a-native-port-and-not-the-linux-build).
