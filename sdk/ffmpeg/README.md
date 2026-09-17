# FFmpeg port and Media Player

libavutil / libavcodec / libavformat / libswresample / libswscale compiled
against the SavanXP libc, and **Media Player** (`/disk/bin/mediaplayer`), the
windowed audio/video player built on them.

How the player works -- the single-threaded loop, the clock, the audio latency
it compensates, what is missing -- is in
[`docs/MEDIA_PLAYER.md`](../../docs/MEDIA_PLAYER.md). This file is about
building it.

## Building

### Windows

```powershell
.\tools\bootstrap.ps1          # once: bakes GNU make into toolchain/ with the rest
.\sdk\ffmpeg\build.ps1         # fetch, configure, build, link, stamp, install
.\build.ps1 run                # Media Player is in the Accessories group
```

Needs Git for Windows (its `bash` runs the `.sh` scripts) and the baked
toolchain: the LLVM that builds the OS and a pinned native GNU make
(`tools/toolchain.lock.json`). No WSL, no MSYS2.

Switches: `-NoInstall` builds and stamps without touching `build/disk.img`;
`-SkipPort` only stamps and installs the already linked ELF; `-WithTestMedia`
also installs the test clips into `/disk/media`; `-Work <dir>` moves the work
directory.

### Linux / macOS

```sh
bash sdk/ffmpeg/all.sh         # needs clang, ld.lld, llvm-ar and GNU make in PATH
pwsh sdk/ffmpeg/install.ps1    # stamp and install into build/disk.img
```

### The work directory

FFmpeg's source never enters the repository. It is downloaded (sha256-pinned)
and built in `$HOME/savanxp-ffmpeg`, overridable with `WORK=` / `-Work`. The
first build takes a few minutes; later ones only relink.

The path **must not contain whitespace**: `configure` expands `CFLAGS` unquoted,
so a path with a space silently splits into two arguments. On Windows a profile
directory with a space is common, so the scripts switch to its 8.3 short form
(`C:\Users\OASISD~1\...`) on their own. For the same reason the SDK headers and
linker script are copied into `$WORK/sysroot` instead of referenced in place.

## Scripts

`all.sh` chains the steps; each one can also run alone.

| script | what it does |
| --- | --- |
| `env.sh` | shared environment: paths, target flags, the whitespace rule |
| `fetch.sh` | downloads and verifies the FFmpeg tarball |
| `runtime.sh` | builds the SavanXP runtime as `libsavanxp.a` and `libsxgui.a` |
| `configure.sh` | runs FFmpeg's configure with the player's format set; skipped when the arguments did not change (`FORCE_CONFIGURE=1`) |
| `build.sh` | `make -k`, and a summary of what failed |
| `link.sh` | compiles `mediaplayer/*.c` and links `build/external/mediaplayer.elf` |
| `install.ps1` | stamps the SXE resources on a copy and installs it |

## Formats

The set lives in `configure.sh`; the rest is resolved by configure from each
component's dependencies.

| | |
| --- | --- |
| Containers | MP4/MOV, Matroska/WebM, AVI, Ogg, MPEG-PS, MPEG-TS, MP3, FLAC, WAV, ADTS AAC, AC-3, IVF, raw H.264/HEVC/MPEG video, MJPEG |
| Video | H.264, HEVC, VP8, VP9, MPEG-4 Part 2, MS-MPEG4v3, Theora, MPEG-1/2, MJPEG |
| Audio | AAC, MP3, MP2, Vorbis, Opus, FLAC, AC-3, PCM |

Everything decodes in scalar C on one core: no assembly (the toolchain has no
nasm) and no threads (the kernel has none). What that costs is measured in
`docs/MEDIA_PLAYER.md`.

## Testing

```powershell
.\sdk\ffmpeg\build.ps1 -NoInstall
.\build.ps1 ffmpeg-smoke
```

`ffmpeg-smoke` installs the player and three generated clips and boots twice:

- `mediaplayer --selftest` decodes `tono.wav` (audio only), `clip.mjpeg` (raw
  video, no container) and `avsync.avi` (MJPEG + PCM interleaved, resampled
  from 44.1 kHz mono): every frame converts, audio lasts as long as the file,
  a seek lands on the requested frame and sample, and on `avsync.avi` each
  flash frame starts within 20 ms of its beep.
- `mediaplayer --gpu-hold` shows `avsync.avi` through `/dev/gpu0` and holds the
  last frame; the harness takes a QMP screenshot into `build/shots/player/` and
  fails if it is a flat colour.

The clips are generated with Python + Pillow by `make-tone.py`, `make-clip.py`
and `make-avclip.py`. They are drawn so that errors are visible: a bar that
moves one step per frame, pure colour stripes that change hue if the U/V planes
are swapped, and in `avsync.avi` a flash and a 1 kHz beep at the start of every
second, which is also the manual sync check in the window.

`mediaplayer --probe <file>` prints what a file contains and decodes it whole,
for trying a new file from the shell.

## Three things that are not obvious

All three share a shape: they do not fail loudly, they produce a wrong
configuration.

1. **`runtime.sh` exists because FFmpeg's configure LINKS.** Without a libc to
   link against, its checks do not error out: they conclude the libc has
   nothing and carry on.

2. **The link goes through the Linux target triple.** Compiling targets
   `x86_64-unknown-none-elf`, but with that triple clang's driver hands the link
   to a `gcc` that does not exist on Windows. With `x86_64-unknown-linux-gnu`
   it calls `ld.lld` directly on every host; `-nostdlib -static -no-pie` and
   the SDK linker script decide the output, so the ELF is the same. Without
   `-no-pie` lld rejects every program and configure decides there is no
   `trunc`; without `-static` the ELF gets an `.interp` that misaligns the
   second `PT_LOAD`.

3. **H.264 does not link without HEVC in FFmpeg 7.1.1.** `h2645_sei.o`
   references `ff_aom_uninit_film_grain_params`, which is only compiled with
   the HEVC decoder. The library build succeeds and the error only shows up
   when linking the program.
