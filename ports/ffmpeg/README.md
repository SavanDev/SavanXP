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

The build produces the raw link output at:

```text
build/external/mediaplayer.elf
```

and a stamped copy at:

```text
build/ffmpeg/mediaplayer-ffmpeg
```

## Installation

`install.sh` stages only the optional Media Player backend and, when requested,
the three deterministic test clips under `build/media/`. The base image already
contains `/bin/mediaplayer`; when this backend is absent, opening that system
application shows an in-window message explaining that the FFmpeg port must be
built. It requires an existing valid `build/disk.img` and
`build/tools/sxfs-cli`, then uses
`tools/sxfs_sync.py` with the persistent-image lock, candidate validation,
atomic replacement, rollback, and safe compaction. It never passes `--reset`.
The installed backend has the distinct name `mediaplayer-ffmpeg`; the system
launcher remains `/bin/mediaplayer`, and the base manifest owns media file
associations. When upgrading an image that used the old
`/disk/bin/mediaplayer` backend, run this installer once before the next normal
build so the backend is preserved under its new name.

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
/disk/bin/mediaplayer
/disk/bin/mediaplayer-ffmpeg
/disk/bin/doomgeneric
/disk/games/doom/doom1.wad
```

The current port is single-threaded, assembly-free, and uses the SavanXP SDK
runtime and SXGUI. Networking, encoders, muxers, and the upstream FFmpeg
command-line programs are intentionally outside this port's scope. Decoding
H.264, HEVC, and MPEG-4 still requires a separate assessment of patent
licensing for any distribution.
