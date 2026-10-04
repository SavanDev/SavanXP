# Source and relinking information

The FFmpeg library source is not vendored in this repository. `fetch.sh`
downloads the exact archive identified by `UPSTREAM` and verifies its SHA-256
before unpacking it into the external work directory. The archive's release
signature and fingerprint are recorded in `UPSTREAM` for release verification.

The corresponding source for the distributed static binary can be reproduced
with:

```bash
./ports/ffmpeg/build.sh --no-install
```

The build keeps the complete build recipe and configuration in this directory:

- `env.sh` records the compiler/linker and SDK sysroot setup;
- `runtime.sh` builds the SavanXP runtime archives;
- `configure.sh` records the exact FFmpeg configure arguments and LGPL guard;
- `make.sh` invokes upstream GNU Make;
- `link.sh` links the five upstream archives into `libffmpeg.so.0.4` and
  nothing else;
- the Media Player source is not here: it is
  `subsystems/posix/userland/mediaplayer/` in the system tree, which builds the
  program and links it against this port's library.

The raw and stamped ELF outputs, generated object files, and FFmpeg source
archive remain outside Git. A distributor that ships the binary should make
the pinned FFmpeg archive and these build materials available as the LGPL
corresponding-source/relinking package, together with the applicable license
texts.
