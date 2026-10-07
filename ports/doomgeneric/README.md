# DoomGeneric port

This is the official SavanXP port of DoomGeneric. The pinned upstream is
`ozkl/doomgeneric` at the revision recorded in [`UPSTREAM`](UPSTREAM).

The versioned port is the only Doom integration kept in the repository.

## Layout

- `source/doomgeneric/` is the pristine upstream subtree, protected by the
  deterministic digest recorded in `UPSTREAM`.
- `patches/` contains the small reviewed changes required by the engine.
- `overlay/` contains SavanXP video, audio, compatibility, network stubs, and
  SXE resources.
- `sources.txt` is the explicit list of upstream C files compiled by the port.
- `build.sh` assembles the source, builds the ELF, stamps SXE resources, and
  installs through the SxFS candidate flow.

Alternate upstream backends are intentionally not compiled: the list prevents
their duplicate `main` implementations and SDL/X11 dependencies from entering
the SavanXP binary.

## Build and install

From the repository root:

```bash
./ports/doomgeneric/build.sh
```

Useful options:

```bash
./ports/doomgeneric/build.sh --no-install
./ports/doomgeneric/build.sh --no-compact
./ports/doomgeneric/build.sh --wad /path/to/doom1.wad
```

The port uses the same SxFS candidate flow as the base image builder.

The default WAD location is `ports/doomgeneric/wad/freedoom1.wad`. A missing
WAD is reported but does not prevent the binary from being installed.

The build requires the Linux host tools documented in
`docs/BUILD_CMAKE.md`, plus `clang`, `ld.lld`, `llvm-objcopy`, `llvm-readelf`,
Python/Pillow, and Git. It produces:

```text
build/external/doomgeneric.elf
```

Installation uses `tools/sxfs_sync.py`; it never resets or deletes
`build/disk.img` and preserves files outside the port's install stage. Stop
QEMU or another VM before installing: the image lock coordinates SavanXP
builders, but it cannot stop an external writer that bypasses the protocol.

The port links the system's MIDI synthesizer, so the base build has to have
produced it first. `build.sh` fails with the fix if
`build/diskfs/lib/libsxmidi.so.0.4` is missing:

```bash
./build.sh build
```

## Persistence regression

After installing the port, run the normal Linux build and verify:

```text
/disk/bin/doomgeneric
/disk/games/doom/<selected-wad>
```

Existing Doom configuration and `savegames/` data must remain byte-for-byte
identical across the rebuild. The current regression assets are
`doom1.wad`, `default.cfg`, `doomgenericdoom.cfg`, and `savegames/`.

## Music

The port plays the WAD's MUS tracks through the WAD's own OPL2 bank. Each MUS lump
goes to MIDI with the upstream `mus2mid.c`, the `GENMIDI` lump goes with it to
`sx_midi_song_create_with_bank`, and the synthesizer in `/disk/lib/libsxmidi.so.0.4`
fills a buffer with one second of lead and then synthesises a quarter-second chunk
per frame while the mixer loops that buffer on a dedicated voice. Without `GENMIDI`
the same song falls back to the built-in families; the log names the path
(`(FM)` vs `(familias)`). Rendering it in one shot made every music change stall the
game for about a second under TCG. The library is a `DT_NEEDED`; without it in the
volume the game still runs, silent. See `docs/MIDI.md`.

`./build.sh smoke doom-music-smoke` opens the port on the desktop, starts the
level and asserts that the audio QEMU captured is not silent: it is the check
that catches a port which draws but does not sound. It then closes the window and
asserts that the capture falls silent with the last music, which catches an audio
daemon still feeding a client that no longer exists. It needs the port and a WAD
installed in the volume.

## Current scope

The port remains keyboard-only and uses SFX through the SavanXP PCM mixer. Music
is enabled through `libsxmidi`. Mouse input, networking, and multiplayer are
outside this migration slice.
