# Official ports

`ports/` contains versioned third-party integrations that SavanXP supports
officially. Each port owns its upstream pin, small patches, SavanXP overlay,
build entry point, and documentation. A port is built independently from the
base CMake target and installs through the SxFS candidate flow.

The official ports are `doomgeneric` and `ffmpeg`. FFmpeg intentionally
keeps its upstream archive outside Git; its `UPSTREAM` pin and build recipe
make the download reproducible.

The first official port is `doomgeneric`. Its persistent-data regression is
`/disk/bin/doomgeneric` plus the WAD under `/disk/games/doom`.

## Port layout

```text
ports/<name>/
  UPSTREAM          # repository, revision, and source URL
  source/           # pinned upstream tree when it is vendored (optional)
  patches/          # focused upstream patches (when source is vendored)
  overlay/          # SavanXP adapter files
  build.sh          # standalone host build/install entry point
  README.md         # behavior, provenance, and test instructions
```

Do not commit generated binaries, downloaded IWADs, or other personal assets.
Keep local experiments outside the official port directories.

## Common rules

- Pin the exact upstream revision; do not build from an unversioned checkout.
- Keep upstream changes small and reviewable in `patches/`.
- Keep OS integration in `overlay/`, not mixed into upstream files.
- Use `tools/sxfs_sync.py`/the `disk.img.lock` contract for installation.
- Run the normal Linux build after installing the port and verify persistence.
