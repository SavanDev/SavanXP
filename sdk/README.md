# SDK

This directory contains C examples and external application sources for
SavanXP. Applications are built for the public POSIX SDK and can be installed
into `build/disk.img` without rebuilding the initramfs.

The canonical public SDK surface is [`subsystems/posix/sdk/v1`](../subsystems/posix/sdk/v1).

## Basic workflow

From the repository root:

```bash
./build.sh build
./tools/build-user.sh --source sdk/hello --name hello
./build.sh run
```

Inside SavanXP:

```text
which hello
hello
```

The builder accepts either a single `.c`/`.S` file or a directory containing
sources and local headers. A directory source is compiled recursively, with
`include/` added to the include path when present.

## Included examples

- `sdk/hello/main.c`: simple stdout output
- `sdk/errdemo/main.c`: simple stderr output
- `sdk/fsdemo/main.c`: create/write/read under `/disk/tmp`
- `sdk/pathops/main.c`: `mkdir`/`rename`/`truncate`/`unlink`/`rmdir`
- `sdk/procpeek/main.c`: simple process snapshots
- `sdk/spawnwait/main.c`: `spawn` and `waitpid`
- `sdk/statusdemo/main.c`: errors, process states, and SDK metadata
- `sdk/gfxhello/main.c`: a fullscreen application using `gfx_*`
- `sdk/udptest/main.c`: local IPv4 UDP self-test
- `sdk/tcpget/main.c`: minimal HTTP client over IPv4 TCP
- `sdk/multifile/`: an external application with several source files
- `sdk/template/main.c`: minimal starting point for a new application

Applications normally include:

```c
#include "savanxp/libc.h"
```

They can also use `savanxp/gfx2d.h` and its 32-bit bitmap, painter, and
rectangle primitives.

## Build options

Install under an explicit path below `/disk`:

```bash
./tools/build-user.sh --source sdk/fsdemo --name fsdemo \
  --destination /disk/bin/fsdemo
```

Build the ELF without installing it:

```bash
./tools/build-user.sh --source sdk/procpeek --name procpeek --no-install
```

Build a multi-file application:

```bash
./tools/build-user.sh --source sdk/multifile --name multifile
```

Create an application from the public template:

```bash
./tools/new-user-app.sh --name myapp
./tools/build-user.sh --source sdk/myapp --name myapp
```

Create a port-like application under another repository directory:

```bash
./tools/new-user-app.sh --name myport --destination-root ports
```

Applications that need the SXGUI runtime, PCM audio, or SSE/SSE2 opt in with
`--gui`, `--audio`, and `--sse` respectively. The runtime is not linked into
every external binary by default.

## Build, install, and run

`tools/run-user.sh` builds, installs, and boots one application:

```bash
./tools/run-user.sh --source sdk/errdemo --name errdemo
```

The official Doom and FFmpeg integrations live under [`ports/`](../ports/README.md)
and are built independently. They use the same candidate-based SxFS installer
and must not be copied into this directory.
