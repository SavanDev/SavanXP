# Official ports

`ports/` contains versioned third-party integrations that SavanXP supports
officially. Each port owns its upstream pin, small patches, SavanXP overlay,
build entry point, and documentation. A port is built independently from the
base CMake target and installs through the SxFS candidate flow.

The official ports are `doomgeneric`, `ffmpeg`, `ccleste`, `tinycc` and
`openssl`. `openssl` builds the TLS libraries and installs the Mozilla CA
store; like FFmpeg it builds no program and keeps its upstream archive outside
Git. FFmpeg
intentionally keeps its upstream archive outside Git; its `UPSTREAM` pin and
build recipe make the download reproducible. `ccleste` does the same for a
different reason: upstream ships no license file, so nothing may be vendored.

The first official port is `doomgeneric`. Its persistent-data regression is
`/bin/doomgeneric` plus the WAD under `/games/doom`.

`ccleste` is the port with the least upstream delta: the engine compiles
unpatched and the overlay replaces the SDL frontend entirely, so it carries no
`patches/` directory.

`tinycc` is the compiler that runs on the system: `/bin/tcc` builds and runs
POSIX C programs in the guest, `-run` included. Its `tinycc-smoke` scenario
installs the port before it boots.

[`xash3d`](xash3d/README.md) is in development. Its first build supplies the
HLSDK Portable server module and a POSIX program that verifies game initialization.

## Port layout

```text
ports/<name>/
  UPSTREAM          # repository, revision, and source URL
  source/           # pinned upstream tree when it is vendored (optional)
  patches/          # focused upstream patches (when source is vendored)
  overlay/          # SavanXP adapter files
  build.sh          # standalone host build/install entry point
  licenses/         # *.notice staged into /system/licenses on install
  README.md         # behavior, provenance, and test instructions
```

Do not commit generated binaries, downloaded IWADs, or other personal assets.
Keep local experiments outside the official port directories.

## Common rules

- Pin the exact upstream revision; do not build from an unversioned checkout.
- Keep upstream changes small and reviewable in `patches/`.
- Keep OS integration in `overlay/`, not mixed into upstream files.
- Use `tools/sxfs_sync.py`/the `disk.img.lock` contract for installation.
- Install a license notice for everything the port puts in the image
  ([LICENSE_NOTICES.md](../docs/third-party/LICENSE_NOTICES.md)).
- Run the normal Linux build after installing the port and verify persistence.
