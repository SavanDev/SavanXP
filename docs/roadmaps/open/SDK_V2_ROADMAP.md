# SDK 2.0 roadmap

> **Status: phases 0 (2.0 declared), 1 (no internal include paths) and 2 (the sysroot) done; phases 3–5 planned.** This records why the external SDK is
> due a major version, what it depends on today that an SDK should not, and
> the phases that turn it into one SDK for both POSIX and SUN programs. The
> measurements below were taken on 2026-11-18 at `9a293bfa`.

## Why 2.0

`SDK 1.2` was declared on 2026-03-22 (`982aa637`, v0.1.5) and
`SAVANXP_SDK_VERSION_MINOR` has not moved since, across 259 commits that
touched the SDK. In that time:

| | 1.2 when declared | 1.2 today |
|---|---|---|
| Files in the SDK tree | 41 | 68 (27 added, none removed) |
| C, headers, asm | ~5,200 lines | ~20,800 lines |
| Build tool | `build-user.ps1` | `build-user.sh` with `--gui`, `--audio`, `--sse`, `--native` |

What arrived without a version: threads (`pthread.h`, `sched.h`), `mmap`,
`dlfcn.h`, the signal calls (`sigaction`, `signal`, `sigprocmask`, which
record handlers that nothing delivers to), the SXGUI toolkit and
window protocols, PCM audio, MIDI, images, SXE resources, `libm`, `syslog`,
and SUN programs through `--native`.

What **broke** without a version, so a program written against 1.2 as declared
may not compile today:

- The raw calls in `savanxp/libc.h` were renamed: `read`, `write`, `open` and
  the rest are `savanxp_read`, `savanxp_write`, `savanxp_open` (`634dd47f`,
  2026-09-03). The standard names moved to the standard headers with the
  `-1` plus `errno` convention.
- `/dev/fb0` and `FB_IOC_*` are gone.
- `gfx_open` fails unless the program was launched as a compositor client.

A rename and two removals are a major version. 2.0 is overdue as a
statement of what already shipped. It should not wait for the larger work below.

## What the SDK depends on today

### Include paths

`tools/build-user.sh` compiled every program with the following flags until
phase 1. It now passes only the first two:

```
-I <sdk>/include  -I include  -I subsystems/posix/userland  -I build/generated
```

- The SDK's public headers reach 21 headers under the repository's
  `include/`: `abi/` (both ABIs), `sxgfx/` (`gfx2d`, `pixels`, `sxchrome`,
  `sxgui`), `wm/` (both protocols), `sxe/sxe_format.h`, all ten `sxn/`
  headers, `audio/audiod_protocol.h` and `elevation/elevation_protocol.h`.
  Some of those are service protocols, not API. Phase 2 decides which of them
  are public.
- Neither the headers nor the runtime units nor the examples under `sdk/`
  need `subsystems/posix/userland` or `build/generated`. Phase 1 removed
  both flags. Every example builds to a byte-identical ELF without them,
  and so do the `--gui`, `--audio`, `--sse`, `--native` and `--heap-mib`
  combinations.
- `ports/ccleste` and `ports/doomgeneric` add `subsystems/posix/userland`
  themselves.

### Runtime sources

`subsystems/posix/sdk/v1/runtime/units.txt` lists the runtime by group. Of its
20 units, 7 live outside the SDK, in `system/gfx/` (`pixels.c`, `gfx2d.c`,
`sxchrome.c`, `sxgui.c`, `sxgl*.c`). Every consumer compiles the runtime
from source on each build. No prebuilt archive exists outside
`ports/ffmpeg`.

### Dynamic linking

`build-user.sh` links only static, non-PIE images (`-fno-pie`, `-static`).
`dlfcn.h` ships in the SDK, but its implementation does not: the three
ports that link dynamically each compile `system/loader/ldso.c` and
`subsystems/posix/userland/dlfcn.c` themselves and spell out their own
`-pie`, `--dynamic-linker,/lib/ld.so.0.4` and `--export-dynamic` flags. The
ports are `ports/doomgeneric/build.sh`, `ports/ccleste/build.sh` and
`ports/ffmpeg/runtime.sh` with `env.sh`.

### SUN

SUN's API lives in `include/sxn/` and its runtime in `system/runtime/`.
`build-user.sh --native` links only `sxn_runtime.c`. In-tree SUN programs
also get `sxngui.c`, `sxnfiledlg.c` and `mime_icon.c`
(`cmake/UserPrograms.cmake:37`). So an **external** SUN program can use the
header-inline `sxn.h`, `sxnwm.h` and `sxnwidgets.h`, but not `sxngui.h`, the
toolkit frame Task Manager is built on. That is a parity gap
(AGENTS.md, rule 7) between in-tree and external programs.

### Resources

The SXE step runs `tools/gen_sxe_resources.py --project-root <repo>` and asks
`git` for the build id. Outside a checkout it cannot run.

### A sysroot already exists, once

`ports/ffmpeg/runtime.sh` builds one under its work directory. It copies the SDK
headers **and all of `include/`** (kernel headers included), copies
`linker.ld`, and archives the runtime into `libsavanxp.a`. That is the shape
this roadmap generalises, minus copying private headers.

### Documentation

- `subsystems/posix/sdk/v1/README.md` is in Spanish, against AGENTS.md
  rule 3.
- It also says there is no dynamic linker and no shared library.
- `REFERENCE.md` lists `sigaction`, `signal`, `sigprocmask`, `termios`,
  `chmod` and `chown` as missing. They exist. The signal calls only record
  what they are given, because the kernel delivers no signals, and
  `tcsetattr` is accepted and ignored.

## Decisions

**One SDK, two APIs.** POSIX and SUN ship together, with one version
number, one sysroot and one build tool. Their APIs stay separate: POSIX
keeps the standard headers and `savanxp/*`, and SUN keeps `sxn/*`. What both
use is shipped once: the ABI headers, the SXE format, `gfx2d`, the
toolkit, images, MIDI and SxGL.

**The SDK is a build product, not a source directory.** The sources stay
where the subsystem that owns them lives. The POSIX runtime stays in
`subsystems/posix`, the SUN runtime in `system/runtime`, and the
platform graphics in `system/gfx`. The build assembles a sysroot from an
allowlist:

```text
build/sdk/2.0/
  include/        public headers only: POSIX, savanxp/, sxn/, sxgfx/, abi/, sxe/
  lib/nosse/      crt0.o and the runtime objects, without SSE
  lib/sse/        the same with SSE, plus libm and SxGL
  linker.ld
  share/sdk/      build-user.sh, the SXE tools, the icon catalogue
  VERSION
```

Phase 2 ships objects, not archives: a program links every object of
each group it asks for, which an archive would only add `--whole-archive`
to. Archives with ordinary semantics (`libsavanxp.a`, ...) belong to
phase 5, for build systems that expect `-l`. Phase 4 adds `ldso.o` and
`dlfcn.o`, and phase 5 the CMake toolchain file.

This avoids moving hundreds of lines between directories, and the comment
translation each move would force (AGENTS.md, rule 3b). The allowlist is
the public surface, written down in one place.

**Versioning policy.**

- A minor version for additions, a major version for anything that stops
  a program from compiling or running.
- At most one step per release. `master` carries the version the next
  release ships, as it does for the OS. The first addition after a release
  raises the minor and the first break raises the major; later changes before
  the release do not step it again. The release checks the number against
  the changelog entries that touch the SDK. 2.0 is that step for 0.4.4: 1.2
  shipped in 0.4.3. The full rule is in `REFERENCE.md`.
- A change to SUN's or POSIX's surface counts the same.
- The SDK version is not the soname version: libraries keep `.so.0.4`
  tracking the OS ([SHARED_LIBRARIES.md](../../kernel/SHARED_LIBRARIES.md)).

**The sysroot is checked from outside the repository.** "Self-contained"
is only true if a program builds against a copy of `build/sdk/2.0` in a
directory with no checkout around it.

## Phases

### Phase 0 — declare 2.0 (done)

- Rewrite the SDK README in English. Fix the stale claims (dynamic
  linker, signals, `/dev/fb0`). Add a "Breaking changes since 1.2" section
  listing the three above.
- Write the versioning policy into `REFERENCE.md`.
- `SAVANXP_SDK_VERSION_MAJOR 2`, `MINOR 0`. `sdk/statusdemo` and About
  already print it.
- **Acceptance:** `./build.sh build`, `./build.sh test`,
  `smoke smoke`, `smoke native-test`.

### Phase 1 — drop the leftover include paths (done)

- Remove `-I subsystems/posix/userland` and `-I build/generated` from
  `build-user.sh`.
- Build every program under `sdk/` and run the scenarios that build
  external programs (`smoke_catalog.py` names them).
- Ports keep their own flags until phase 4.
- **Acceptance:** every `sdk/` example builds, and the smoke scenarios
  that use `build-user.sh` pass.

### Phase 2 — the sysroot (done)

- `./build.sh build` assembles `build/sdk/2.0` from a header allowlist
  and archives the runtime per `units.txt` group.
- Decide whether `audiod_protocol.h` and `elevation_protocol.h` are
  public. If they aren't, the public headers stop including them.
- `build-user.sh` compiles against the sysroot only.
- `gen_sxe_resources.py` ships in the sysroot. Without `git`, the build id
  falls back to `unknown`, which it already does.
- A host test fails if a public header includes a header outside the
  allowlist.
- **Acceptance:** copy `build/sdk/2.0` to a temporary directory and build
  `sdk/hello`, `sdk/gfxhello` and `sdk/nativetest` against the copy, with
  only `clang` and `ld.lld`. The image keeps `/bin/doomgeneric` and the
  WADs (AGENTS.md, rule 4).

What landed, and what it settled:

- `tools/make-sdk.sh` assembles the sysroot. The `savanxp_sdk` target runs
  it on every build, and `build-user.sh` runs it before every build. It
  rebuilds only when an input recorded by the last build changed, and the
  compilers' dependency files list those inputs.
- `public-headers.txt` is the allowlist: 24 headers from `include/`.
  `audiod_protocol.h` and `elevation_protocol.h` are public, because public
  headers are built on them. So is `virgl_protocol.h`, which is the payload of
  the public `GPU_IOC_3D_SUBMIT` and which `sdk/nativetest` uses.
- **Same binaries.** All 14 `sdk/` examples, and seven combinations of
  `--gui`, `--audio`, `--sse` and `--native`, link to byte-identical images
  against the sysroot and with the runtime compiled from source.
- `--heap-mib` no longer recompiles `posix.c`. The bootstrap arena
  (`sx_heap_bootstrap`, `sx_heap_bootstrap_size`) is weak, and the program
  links strong definitions generated for it. The weak 256 KiB stays in its
  `.bss` unused, because static images are not linked with `--gc-sections`.
- The build id comes from the program's own git checkout, not the
  repository's, and `gen_sxe_resources.py` takes the icon catalogue as
  `--icon-root`.
- `sdk-sysroot-test` compiles each of the 72 sysroot headers on its own,
  seeing only the sysroot, and fails on any file read from outside it. A
  copy of the sysroot builds `hello`, `gfxhello --gui`, `nativetest --native`
  and a `--heap-mib` program.
- The `midi` group is not in the sysroot: `build-user.sh` has no flag that
  links it, so `savanxp/midi.h` stays a header without a library, as it
  already was.
- **Found while testing:** `sys/time.h` and `sys/select.h` both defined
  `struct timeval`, so a program that included both did not compile.
  It was fixed right after: `sys/time.h` now takes the type from
  `sys/select.h`. `sdk-sysroot-test` also compiles all the headers together
  now, which catches that kind of conflict.

### Phase 3 — SUN on equal terms

- `libsxn.a` carries `sxn_runtime.c`, `sxngui.c`, `sxnfiledlg.c` and
  `mime_icon.c`.
- `build-user.sh --native --gui` links SUN's frame, not the POSIX one.
- An example under `sdk/` builds a window with `sxngui.h` from outside the
  tree, and `nativetest` asserts what it uses.
- **Acceptance:** `smoke native-test`, `smoke sxgui-smoke`, plus the new
  example in a scenario.

### Phase 4 — dynamic linking through the SDK

- `build-user.sh --pie` links PIE against `ldso.o` and `dlfcn.o` from the
  sysroot. It adds `--export-dynamic` and the `/lib/ld.so.0.4`
  interpreter, and takes `--needs libfoo.so.0.4` for `DT_NEEDED`.
- `doomgeneric`, `ccleste` and FFmpeg drop their hand-written copies and
  use the SDK's flags or the toolchain file.
- Static stays the default until the shared-library work says otherwise.
  Changing the default would be its own minor version.
- **Acceptance:** each port builds and runs its scenario, and the
  `ld*test` programs pass.

### Phase 5 — toolchain integration

- A CMake toolchain file and a `sxcc` wrapper in `share/sdk/`, so an
  autotools or CMake project configures against the sysroot without a
  bespoke script.
- FFmpeg's `env.sh` becomes the first consumer.

## Not in this roadmap

- Building the in-tree programs against the sysroot. CMake keeps building
  them from source; the allowlist test keeps the two honest.
- Renaming or removing API to tidy it up. 2.0 records the breaks that
  already happened and adds no new ones.
- Win32 and Linux subsystem headers, a C++ runtime, and distribution
  outside a release ISO.
