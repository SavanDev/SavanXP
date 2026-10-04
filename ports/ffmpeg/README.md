# FFmpeg Media Player port

This is the official SavanXP port of the FFmpeg 7.1.1 libraries and the
Media Player adapter. FFmpeg itself is not forked and is not patched. The
port supplies the target runtime, a small SavanXP Media Player overlay, and
the exact `configure`/GNU Make integration.

The versioned port is the only FFmpeg integration kept in the repository.

## Upstream and license

[`UPSTREAM`](UPSTREAM) pins the upstream tag, commit, tree, archive, hash, and
release-signature information. The source is downloaded into the external work
directory rather than vendored into the repository.

FFmpeg's combined library license is LGPL-2.1-or-later for this configuration.
The exact license texts are [`LICENSE.md`](LICENSE.md) and
[`COPYING.LGPLv2.1`](COPYING.LGPLv2.1). Static-link relinking material and the
Media Player adapter remain part of the distributed source package; see
[`SOURCE.md`](SOURCE.md).

## Build

```bash
./ports/ffmpeg/build.sh
```

Useful options:

```bash
./ports/ffmpeg/build.sh --no-install
./ports/ffmpeg/build.sh --skip-port
./ports/ffmpeg/build.sh --with-test-media
./ports/ffmpeg/build.sh --no-compact
./ports/ffmpeg/build.sh --work /path/to/ffmpeg-work
```

The pipeline deliberately remains:

```text
fetch.sh -> runtime.sh -> configure.sh -> make.sh -> link.sh
```

`configure.sh` keeps the selected demuxers, decoders, parsers, H.264/HEVC
link dependency, single-threaded/no-assembly profile, and LGPL configuration
guard. The work directory must not contain whitespace because upstream
configure expands compiler flags without quoting. Set
`SAVANXP_VERIFY_SIGNATURE=1` to additionally download and verify the release
signature with GPG; the archive SHA-256 is always mandatory.

The build produces one artifact:

```text
build/external/libffmpeg.so.0.4
```

That is the whole port. **It builds no program.** The Media Player itself lives in
`subsystems/posix/userland/mediaplayer/` and is built by the system tree, which
links it against this library; when the port has not been built the player is not
built either, and the launcher registry drops the entry the way it drops Doom's.

## Installation

`install.sh` stages one file, `/disk/lib/libffmpeg.so.0.4`, and when requested the
three deterministic test clips under `build/media/`. It requires an existing valid
`build/disk.img` and `build/tools/sxfs-cli`, then uses `tools/sxfs_sync.py` with
the persistent-image lock, candidate validation, atomic replacement, rollback, and
safe compaction. It never passes `--reset`.

Nothing under `/disk/bin` is installed, so an image built before this change can
keep its stale `/disk/bin/mediaplayer-ffmpeg`; remove it once with
`build/tools/sxfs-cli rm build/disk.img /bin/mediaplayer-ffmpeg` if you want the
volume tidy. It is inert: nothing lists it and nothing has a `DT_NEEDED` for it.

Stop QEMU or another VM before installing; the image lock coordinates SavanXP
builders but cannot stop an external writer that bypasses the protocol.

## Test media

`--with-test-media` generates deterministic files using only Python/Pillow:

```text
build/media/tono.wav
build/media/clip.mjpeg
build/media/avsync.avi
```

They are installed under `/disk/media/` and are not committed to Git.

## Smoke

The port owns the complete two-phase smoke flow:

```bash
./build.sh smoke ffmpeg-smoke
# equivalent, when the linked ELF already exists:
./ports/ffmpeg/smoke.sh --skip-port
```

The first boot runs `/bin/mediaplayer --selftest` over the generated WAV, MJPEG
and AVI clips. The second boot runs `--gpu-hold`; after the guest reports
`MEDIAPLAYER DISPLAY READY`, the Linux runner captures the held frame through
QMP and rejects a blank or single-colour screen. Both boots use disposable SxFS
copies, and the port restores a clean normal build (without `/SMOKE`) when the
flow finishes.

## Persistence and scope

After installation, run the normal Linux build and verify that these remain:

```text
/disk/lib/libffmpeg.so.0.4
/disk/bin/mediaplayer
/disk/bin/doomgeneric
/disk/games/doom/doom1.wad
```

The current port is single-threaded, assembly-free, and uses the SavanXP SDK
runtime and SXGUI. Networking, encoders, muxers, and the upstream FFmpeg
command-line programs are intentionally outside this port's scope. Decoding
H.264, HEVC, and MPEG-4 still requires a separate assessment of patent
licensing for any distribution.
