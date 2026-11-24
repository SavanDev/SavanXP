# Working in `ports/`

The root [`AGENTS.md`](../AGENTS.md) applies; this adds what is particular to
the official ports.

## Read first

- [`README.md`](README.md): the port layout and the common rules, then the
  port's own `README.md`.
- [`THIRD_PARTY_ADOPTION.md`](../docs/third-party/THIRD_PARTY_ADOPTION.md) and
  [`LICENSE_NOTICES.md`](../docs/third-party/LICENSE_NOTICES.md) when adding
  upstream code or something the image ships.

## Rules here

- Ports and external programs are written against POSIX.
- `.gitignore` allow-lists `ports/`: a new port is ignored until its `!` lines
  are added, in the same commit. Never `git add -f`.
- No WADs, generated binaries or downloaded upstream archives in the
  repository.

## Verify

- The port's own scenario (`ffmpeg-smoke`, `tinycc-smoke`, `ccleste-selftest`,
  `doom-music-smoke`, `openssl-posix`, `openssl-sun`, `xash3d-*`):
  `./build.sh smoke --list`.
- Install, then `./build.sh build`: the persistence check in the root
  `AGENTS.md`. `./tools/verify_doom_persistence.sh` runs it on a copy of the image.
