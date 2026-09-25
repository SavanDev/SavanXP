# ccleste port

The official SavanXP port of [ccleste](https://github.com/lemon32767/ccleste), a
C source port of Celeste Classic for PICO-8. Upstream is not forked and not
patched. The port supplies the SavanXP frontend that replaces the SDL one, the
target runtime, and the exact download/build/install recipe.

## Why this port is shaped the way it is

Upstream already draws a clean line: `celeste.c`, `celeste.h` and `tilemap.h`
depend on nothing but the C standard library, and `sdl12main.c` is the host
frontend that implements the PICO-8 callback surface on top of SDL. SavanXP has
no SDL, so the port replaces the frontend and compiles the engine untouched.

That gives `patch_count=0`: there is no `patches/` directory, and not one line of
`sdl12main.c` is compiled or carried. The contract between the two halves is the
`Celeste_P8_set_call_func` callback declared in `celeste.h`, and
`overlay/ccleste_savanxp.c` implements all fourteen call types.

The engine is compiled with `-Wno-strict-prototypes -Wno-extra-semi`. Those are
upstream's K&R-style empty parameter lists and a stray top-level semicolon; the
port silences the two diagnostics rather than editing a file it does not own.

## Upstream and license

[`UPSTREAM`](UPSTREAM) pins the repository, the commit, the tree, and the
archive SHA-256. The source is downloaded into the port work directory rather
than vendored into the repository. The archive size and SHA-256 are mandatory;
there is no release signature to verify.

**ccleste ships no license file.** The repository was archived read-only on
2023-11-07 and the GitHub API reports no license for it, which means the code is
under the default "all rights reserved" and is not licensed for redistribution
by this repository. `UPSTREAM` records this as `license=Unlicensed` and
`docs/THIRD_PARTY_PROVENANCE.md` carries the same finding. The practical
consequences are deliberate:

- no upstream source is committed here, so nothing unlicensed enters the Git
  tree;
- the archive is fetched by `fetch.sh` and verified against the pinned SHA-256
  before it is used;
- the built binary and the game assets land in the image, so the registry entry
  names both as distributed bits.

Anyone who needs a redistributable Celeste Classic should ask the author, or
pick a port with a stated license.

## Build

```bash
./ports/ccleste/build.sh
```

Useful options:

```bash
./ports/ccleste/build.sh --no-install     # build and stamp, leave disk.img alone
./ports/ccleste/build.sh --skip-port      # reuse build/external/ccleste.elf
./ports/ccleste/build.sh --no-assets      # install only the executable
./ports/ccleste/build.sh --no-compact     # no automatic SxFS compaction
./ports/ccleste/build.sh --work PATH      # override the port work directory
```

The pipeline is deliberately:

```text
fetch.sh -> compile -> link -> stamp -> install
```

`fetch.sh` downloads the pinned archive, checks its size and SHA-256, and
unpacks it. The build then copies `celeste.c`, `celeste.h` and `tilemap.h` out of
the pristine tree into a scratch directory, drops the overlay next to them, and
compiles both halves with the SavanXP SDK runtime. Nothing is compiled in place
and no upstream file is written to.

The 128x128 BGRX frame goes to the compositor through the SDK's
`sx_scaled_presenter`, which scales by an integer factor, centers, and presents
only the changed rows. The build produces:

```text
build/external/ccleste.elf
build/ccleste/ccleste                    # stamped copy
```

Celeste Classic uses floating point in its update step, so the port is built with
`-msse -msse2` and links the SDK math runtime, the same choice the FFmpeg port
makes. The 23 sound effects decode to 772 KiB of unsigned 8-bit mono PCM, which
does not fit the 256 KiB default arena, so the runtime is compiled with
`-DSX_HEAP_SIZE=1048576`. The heap lives in `.bss`: it costs address space, not
image bytes.

## Game data

The game needs its tile sheet, its font and its sound effects. They come from
the same pinned archive and are installed under:

```text
/disk/games/celeste/gfx.bmp
/disk/games/celeste/font.bmp
/disk/games/celeste/snd*.wav     # 23 files
```

`build.sh` copies them out of the fetched tree, so nothing third-party is
committed and the ~1.1 MB never has to be re-fetched by hand. The port declares
`data_dir=/disk/games/celeste` in its `.sxres`, which is what lets the
uninstaller offer to take the directory instead of leaving it orphaned. Passing
`--no-assets` installs the executable alone; the port then starts, reports the
missing directory and exits 1.

`gfx.bmp` and `font.bmp` are uncompressed paletted BMPs at 4 and 1 bpp. Their own
palettes are ignored on purpose: every pixel in this game is drawn through the
mutable 16-entry PICO-8 palette, so the colours baked into the files are dead
weight and `overlay/ccleste_savanxp_assets.c` decodes indices only.

## Controls

The port keeps the upstream binding table and adds the SavanXP-specific ones:

| Key | Action |
| --- | --- |
| Arrows | move, look up, look down |
| `Z` / `C` / `N` | jump |
| `X` / `V` / `M` | dash |
| `Esc` | pause |
| `E` | toggle screenshake |
| `R` (hold) | reset the run |
| window close | exit |

Fullscreen is the launcher's decision, not the game's: `launch_flags=fullscreen`
in the `.sxres` makes the window manager hand the port a client surface at the
presentation size.

## Music is out of scope

The five tracks ship as OGG Vorbis and SavanXP has no Vorbis decoder. The SDK
mixer plays raw PCM, and decoding the tracks to PCM on the host would cost about
24 MB, which does not fit the persistent image. `CELESTE_P8_MUSIC` is therefore a
logged no-op, `build.sh` does not copy the `.ogg` files, and the game runs
silent. This is the same kind of boundary the FFmpeg port draws around
networking, encoders and muxers.

Adding music later means a Vorbis decoder, not a change to this port's
structure: the callback already receives `(index, fade, mask)` and the SDK
already has a wall-clock frame sink to feed.

Save and load state (`Shift+S` / `Shift+D` upstream) are announced and dropped.
The port keeps the state in memory only rather than writing an undocumented file
under `/disk`.

## Test

The port ships a headless self-test that needs no window and no audio device:

```bash
./build.sh smoke ccleste-selftest
```

It decodes the two sheets and all 23 sound effects, then drives 120 real
`Celeste_P8_update()` / `Celeste_P8_draw()` frames through the full callback
surface and prints `CCLESTE SELFTEST PASS`. It fails if the sheets decode empty
or if no sound effect decoded, which is what catches a stale or half-synced data
directory. It can also be run by hand from the shell with `ccleste --selftest`.

The visual path is a desktop session, not a smoke scenario: the `/SMOKE` path
never starts the compositor, and `gfx_open` only succeeds for a client the
desktop launched. Use the shared visual driver, which boots the real desktop and
drives the launcher:

```bash
./tools/shoot.sh --scenario ccleste
```

The game prints `CCLESTE DISPLAY READY` after its first frame with content
reaches the compositor, which is the point at which the screen is worth
capturing.

## Persistence and scope

After installation, run the normal Linux build and verify that these remain:

```text
/disk/bin/ccleste
/disk/bin/doomgeneric
/disk/games/doom/doom1.wad
/disk/games/celeste/gfx.bmp
```

The port is single-threaded and uses the SavanXP SDK runtime. The fixed-point
PICO-8 build (`CELESTE_P8_FIXEDP`, which upstream needs a C++ compiler for), the
TAS playback mode, the game controller mappings, the 3DS and Emscripten
frontends, and the fullscreen toggle are all intentionally outside this port's
scope.
