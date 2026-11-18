# Source and relinking information

This file ships in the image as part of FFmpeg's license notice
(`/system/licenses/ffmpeg/`). It says where the source of `/lib/libffmpeg.so.0.4`
is, and how to replace that library, as the GNU LGPL 2.1 asks.

## What is distributed

`/lib/libffmpeg.so.0.4` is FFmpeg `n7.1.1` (commit
`db69d06eeeab4f46da15030a80d539efb4503ca8`), **unmodified**: `UPSTREAM` records
`patch_count=0`. Five upstream libraries (libavformat, libavcodec, libavutil,
libswscale, libswresample) are linked into that one shared library and nothing
else. It is built LGPL-2.1-or-later only: the configure step refuses a build
with GPL, version 3 or nonfree parts enabled.

## Where the source is

- **The FFmpeg source** is the release archive `ffmpeg-7.1.1.tar.xz`, SHA-256
  `733984395e0dbbe5c046abda2dc49a5544e7e0e1e2366bba849222ae9e3a03b1`. Every
  SavanXP release whose image contains this library carries that archive as
  an asset of the same GitHub release, next to the ISO:
  <https://github.com/SavanDev/SavanXP/releases>. It is also the upstream
  release at <https://ffmpeg.org/releases/ffmpeg-7.1.1.tar.xz>, signed by the
  key recorded in `UPSTREAM`.
- **The scripts that control the build** are in `ports/ffmpeg/` of the SavanXP
  repository at the release's tag: <https://github.com/SavanDev/SavanXP>.
  - `fetch.sh` downloads the archive and checks its size, SHA-256 and signature;
  - `env.sh` sets the compiler, linker and SDK sysroot;
  - `runtime.sh` builds the SavanXP runtime archives;
  - `configure.sh` holds the exact configure arguments and the LGPL guard;
  - `make.sh` runs upstream GNU Make;
  - `link.sh` links the five archives into `libffmpeg.so.0.4`.

To rebuild the library from them:

```bash
./ports/ffmpeg/build.sh --no-install   # build/external/libffmpeg.so.0.4
```

## Replacing the library

The Media Player (`/bin/mediaplayer`, source in
`subsystems/posix/userland/mediaplayer/` of the same repository) uses FFmpeg
only through `libffmpeg.so.0.4`, which the system loader maps at startup. To
run it against a modified FFmpeg, build the library with the scripts above and
replace `/lib/libffmpeg.so.0.4`; the player is not relinked. When that exact
file is absent, the loader takes the highest `libffmpeg.so.0.N` with `N`
above 4 instead.
