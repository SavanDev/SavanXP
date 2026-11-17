# DoomGeneric port

This is the official SavanXP port of DoomGeneric. The pinned upstream is
`ozkl/doomgeneric` at the revision recorded in [`UPSTREAM`](UPSTREAM).

The versioned port is the only Doom integration kept in the repository.

## i386 branch status

The port builds as ELF32 PIE with the embedded native loader and the shared
`libsxmidi.so.0.4` synthesizer, preserving the reference application design.
The reference desktop and AC'97/RTL8139 services support graphics and audio.
`./build.sh smoke doom` checks the installed game, FM music and silence after
closing its window; it requires a real installed IWAD and Ccleste for the
reference Games catalog order. Persistence remains a separate regression.
See [service contracts](../../docs/I386_SERVICES.md).

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
[BUILD_CMAKE.md](../../docs/BUILD_CMAKE.md), plus `clang`, `ld.lld`, `llvm-objcopy`, `llvm-readelf`,
Python/Pillow, and Git. It produces:

```text
build/external/doomgeneric.elf
```

Installation uses `tools/sxfs_sync.py`; it never resets or deletes
`build/disk.img` and preserves files outside the port's install stage. Stop
QEMU or another VM before installing: the image lock coordinates SavanXP
builders, but it cannot stop an external writer that bypasses the protocol.

## Persistence regression

After installing the port, run the normal Linux build and verify:

```text
/disk/bin/doomgeneric
/disk/games/doom/<selected-wad>
```

Existing Doom configuration and `savegames/` data must remain byte-for-byte
identical across the rebuild. The current regression assets are
`doom1.wad`, `default.cfg`, `doomgenericdoom.cfg`, and `savegames/`.

## Runtime porting work

The retained overlay converts MUS tracks with upstream `mus2mid.c` and renders
them through `sx_midi_song_create_with_bank`, using the WAD's `GENMIDI` bank
when present. The engine and overlay retain the reference implementation;
the build adapts the target triple, 16-byte stack ABI, ELF32 startup and integer
division helpers. The synthesizer is shared as in v0.4.0.
