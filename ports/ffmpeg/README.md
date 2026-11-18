# FFmpeg Media Player port

This is the official SavanXP port of the FFmpeg 7.1.1 libraries and the
Media Player adapter. FFmpeg itself is not forked and is not patched. The
port supplies the target runtime and the exact `configure`/GNU Make
integration; the player itself lives in the tree, not in the port.

The versioned port is the only FFmpeg integration kept in the repository.

AV1 is decoded by [dav1d](https://code.videolan.org/videolan/dav1d) 1.5.1
(BSD-2-Clause), which `dav1d.sh` builds with Meson and `link.sh` links into the
same library. FFmpeg's own `av1` decoder only drives hardware decoders, so it is
not enabled.

## Upstream and license

[`UPSTREAM`](UPSTREAM) pins the upstream tag, commit, tree, archive, hash, and
release-signature information. The source is downloaded into the external work
directory rather than vendored into the repository.

FFmpeg's combined library license is LGPL-2.1-or-later for this configuration.
The exact license texts are [`LICENSE.md`](LICENSE.md) and
[`COPYING.LGPLv2.1`](COPYING.LGPLv2.1); dav1d's is
[`licenses/DAV1D-COPYING.txt`](licenses/DAV1D-COPYING.txt). Where the
corresponding source is, and how to replace the library, is in
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
fetch.sh -> runtime.sh -> dav1d.sh -> configure.sh -> make.sh -> link.sh
```

Everything compiles through the SDK: `runtime.sh` brings the SDK sysroot
(`build/sdk/2.0`) up to date and checks a test link, and the compiler `configure`
and Meson are given is the sysroot's `share/sdk/sxcc` with its static SSE profile,
plus `-fPIC` so the objects can go into the shared library. sxcc supplies the
target flags, the headers, and for a test link crt0, the runtime and the linker
script. `libmath.so.0.4`, which the library declares, comes from the sysroot's
`lib/shared`.

`dav1d.sh` writes a Meson cross file naming the same compiler driver
`configure` uses, builds `libdav1d.a` without assembly and with both 8- and
16-bit depths, and installs it with its `dav1d.pc` under the work directory,
where `configure.sh` finds it through `pkg-config` and nothing else.

`configure.sh` keeps the selected demuxers, decoders, parsers, H.264/HEVC
link dependency, single-threaded/no-assembly profile, and LGPL configuration
guard. The work directory must not contain whitespace because upstream
configure expands compiler flags without quoting. Set
`SAVANXP_VERIFY_SIGNATURE=1` to additionally download and verify both release
signatures (FFmpeg's and VideoLAN's for dav1d) with GPG; both archives' SHA-256
is always mandatory.

The build produces one artifact:

```text
build/external/libffmpeg.so.0.4
```

That is the whole port. **It builds no program.** The Media Player itself lives in
`subsystems/posix/userland/mediaplayer/` and is built by the system tree, which
links it against this library; when the port has not been built the player is not
built either, and the launcher registry drops the entry the way it drops Doom's.

## Installation

`install.sh` stages one file, `/lib/libffmpeg.so.0.4`, and when requested the
five deterministic test clips under `build/media/`. It requires an existing valid
`build/disk.img` and `build/tools/sxfs-cli`, then uses `tools/sxfs_sync.py` with
the persistent-image lock, candidate validation, atomic replacement, rollback, and
safe compaction. It never passes `--reset`.

Nothing under `/bin` is installed, so an image built before this change can
keep its stale `/bin/mediaplayer-ffmpeg`; remove it once with
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
build/media/ffv1.avi
build/media/mulaw.wav
```

`ffv1.avi` (FFV1 video, PCM audio) and `mulaw.wav` (mu-law audio only) carry
codecs this port does not enable, so the selftest can check that the player
names them. They are installed under `/media/` and are not committed to Git.

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
/lib/libffmpeg.so.0.4
/bin/mediaplayer
/bin/doomgeneric
/games/doom/doom1.wad
```

The current port is single-threaded, assembly-free, and uses the SavanXP SDK
runtime and SXGUI. Networking, encoders, muxers, and the upstream FFmpeg
command-line programs are intentionally outside this port's scope. Decoding
H.264, HEVC, and MPEG-4 still requires a separate assessment of patent
licensing for any distribution.
