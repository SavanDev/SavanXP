# SDK 2.0

This directory holds the sources of the public surface for building external
C programs for SavanXP. The contract it carries is **SDK 2.0**; the exact list
of what is stable, and the versioning rules, are in [`REFERENCE.md`](REFERENCE.md).

Programs do not build against this directory but against the **sysroot** the
build assembles from it, `build/sdk/2.0` (below). The directory keeps its `v1`
name; the SDK 2.0 roadmap
([`docs/roadmaps/open/SDK_V2_ROADMAP.md`](../../../../docs/roadmaps/open/SDK_V2_ROADMAP.md))
records where it is going.

## Breaking changes since 1.2

SDK 1.2 was declared on 2026-03-22. Three changes since then stop a program
written against it from building or running, which is why this is 2.0 and
not 1.3:

- **The raw calls in `savanxp/libc.h` carry a `savanxp_` prefix.** `read`,
  `write`, `open`, `close`, `readdir`, `ioctl`, `socket`, `bind`, `sendto`,
  `recvfrom`, `connect`, `getpid`, `stat`, `fstat`, `chdir` and `getcwd` are
  now `savanxp_read` and so on, and still return `-errno`. The standard names
  live in the standard headers (`<unistd.h>`, `<fcntl.h>`, ...) with the POSIX
  convention: `-1` and `errno`. To port a 1.2 program, either include the
  standard headers and check for `-1`, or add the prefix and keep the
  negative-result checks.
- **`/dev/fb0` and the `FB_IOC_*` ioctls are gone.** Draw through `gfx_*`.
  `/dev/gpu0` is the low-level device, and it belongs to `compositord` and
  diagnostic tools.
- **`gfx_open` needs the compositor.** It opens the client surface that the
  desktop hands a program it launches, and fails when there is none. There is
  no direct full-screen fallback.

## What the SDK contains

- `include/savanxp/libc.h`, `include/savanxp/syscall.h`: the low layer, raw
  system calls prefixed `savanxp_*` returning `-errno`, plus process,
  descriptor, graphics and device helpers.
- Standard headers under `include/`: `unistd.h`, `fcntl.h`, `poll.h`,
  `sys/select.h`, `signal.h`, `stdio.h`, `stdlib.h`, `string.h`, `dirent.h`,
  `sys/stat.h`, `sys/socket.h`, `sys/mman.h`, `termios.h`, `pthread.h`,
  `dlfcn.h` and the rest. They declare the standard name and nothing else:
  there is no `#define name sx_name`, and the symbol really exists at link
  time. The `sx_*` functions are the runtime's internal implementation.
- `include/savanxp/gfx2d.h`, `sxchrome.h`, `sxgui.h`: the platform graphics
  headers (`include/sxgfx/`) plus what is POSIX-only.
- `include/savanxp/audio.h`, `midi.h`, `img.h`, `sxe.h`: PCM playback and
  the mixer, the MIDI synthesizer, image decoding, and SXE resources.
- `runtime/`: `crt0.S` and the C runtime. `runtime/units.txt` lists every
  unit by group (`core`, `math`, `gui`, `audio`, `midi`, `gl`, `setjmp`), and
  every build reads it.
- `linker.ld`: programs load at `0x400000`.

## The sysroot

`./build.sh build` runs `tools/make-sdk.sh`, which assembles
`build/sdk/<MAJOR.MINOR>`:

```text
build/sdk/2.0/
  VERSION
  include/            this directory's include/, plus the headers from the
                      repository's include/ that public-headers.txt lists
  linker.ld
  lib/nosse/          the runtime prebuilt without SSE: crt0.o and every unit
                      of the core, gui, audio and setjmp groups, sxn_runtime.o
  lib/sse/            the same with SSE, plus the math and gl groups
  lib/*/units.txt     each object's group, in link order
  share/sdk/          build-user.sh and the tools it runs, and the system
                      icon catalogue a .sxres names with icon=
```

- **Headers.** `public-headers.txt` is the allowlist of what the repository's
  `include/` contributes. Everything else there is private to the system. The
  `sdk-sysroot-test` host test fails if a header in the sysroot needs a file
  outside it.
- **The runtime is compiled once per variant**, with the same flags a program
  gets (`tools/lib/sdk_flags.sh`). A program links every object of each group
  it asks for, in manifest order, so the result is the same binary that
  compiling the runtime with it would give.
- **`tools/build-user.sh` brings the sysroot up to date before every build**,
  so it never lags behind an edited runtime. `make-sdk.sh` returns at once when
  nothing changed.
- **The sysroot stands alone.** A copy anywhere builds programs with its own
  `share/sdk/build-user.sh`, needing only `clang`, `ld.lld`, `llvm-objcopy`,
  `llvm-readelf` and Python with Pillow. Output goes to `$PWD/build` unless
  `SAVANXP_OUTPUT_ROOT` says otherwise. Installing into a disk image needs
  `SAVANXP_DISK_IMAGE` and `SAVANXP_SXFS_CLI`, which default to `disk.img` and
  `tools/sxfs-cli` under that output root.

```bash
cp -R build/sdk/2.0 ~/savanxp-sdk
~/savanxp-sdk/share/sdk/build-user.sh --source ~/myapp --name myapp --no-install
```

## ABI rules

- System calls go through `int 0x80`.
- `tools/build-user.sh` produces static `ELF64` images for `x86_64`. The
  system has a dynamic loader and shared libraries in `/lib`
  ([SHARED_LIBRARIES.md](../../../../docs/kernel/SHARED_LIBRARIES.md)), but
  linking an external program against them is not part of the SDK yet: the
  ports that do it (`doomgeneric`, `ccleste`, FFmpeg) set it up themselves.
  The roadmap's phase 4 moves it into the SDK.
- `main(int argc, char** argv)` is supported, with at most
  `SAVANXP_MAX_ARGC = 15` arguments of up to `SAVANXP_MAX_ARG_LENGTH = 63`
  bytes.

## Workflow

Build and install an external program:

```bash
./build.sh build
./tools/build-user.sh --source sdk/hello --name hello
```

Build only the ELF:

```bash
./tools/build-user.sh --source sdk/multifile --name multifile --no-install
```

Start a program from the public template:

```bash
./tools/new-user-app.sh --name myapp
./tools/build-user.sh --source sdk/myapp --name myapp
```

The tool:

- takes a single `.c` file or a directory, and compiles every `.c` and `.S`
  under a directory recursively;
- adds `-I <source>` and, when it exists, `-I <source>/include`;
- links the runtime groups the program asks for: `--gui` for the SXGUI
  toolkit, `--audio` for PCM audio, `--sse` for SSE/SSE2, `libm` and SxGL;
- with `--heap-mib N`, gives the program an N MiB bootstrap heap arena in
  place of the runtime's default 256 KiB;
- builds a SUN program instead with `--native`
  ([NATIVE_APPS.md](../../../../docs/userland/NATIVE_APPS.md)).

A graphical example:

```bash
./tools/build-user.sh --source sdk/gfxhello --name gfxhello
```

## Minimal layout

```text
myapp/
  main.c
  include/
    myapp.h
  helper.c
```

`sdk/template` is the starting point and `sdk/multifile` shows a program with
several translation units.
