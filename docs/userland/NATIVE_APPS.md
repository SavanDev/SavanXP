# Native apps (SUN)

How to write a program against SUN, the userland native API
([SUN.md](SUN.md)), which speaks the kernel's native API (Sx) directly
with no POSIX in between. POSIX remains the translated subsystem most
software uses; this document is the practical guide: headers, build,
manifest, launch and debugging. What SUN is, where it is going and
the closest Win32 counterpart of each call are in [SUN.md](SUN.md).

## What "native" means here

- The binary is stamped `EI_OSABI=0x53` and enters through the native
  dispatcher, which only implements the SXN contract
  (`include/abi/savanxp_native_abi.h`). POSIX numbers from a native
  process are `ENOSYS`; there is no fallback.
- The process still gets the 0/1/2 stdio convention and one shared
  integer handle table with granted access. `SXN_SYS_SPAWN` clones the
  caller's table into the child, `SXN_SYS_SPAWN_STDIO` gives it stdio only,
  and `SXN_SYS_SPAWN_EX` gives it exactly the handles a request names; the
  POSIX spawn takes stdio only.
- Errors are `-errno` (`savanxp_error_code`), same values as POSIX.
- `main` must never return: the stock `crt0` falls through to a POSIX
  EXIT that is `ENOSYS` for native. Every example ends in `sxn_exit`.

## The headers

The SUN headers, all header-only, no link or tool changes needed:

- `include/sxn/sxn.h` — typed wrappers for every SXN syscall: exit, log,
  spawn (also with chosen handles, `sxn_spawn_ex`), waitpid, terminate,
  threads (`sxn_thread_create`, `sxn_thread_exit`, `sxn_thread_exit_code`,
  `sxn_set_thread_base`, `sxn_wait_address`, `sxn_wake_address`),
  events, sections, views, waits, gfx, handle close and duplicate, pipe
  creation (`sxn_pipe_create`), pipe and device read/write, device open and
  control, ports, the clock.
- `include/sxn/sxnwm.h` — the windowing of SUN over a context struct:
  connect to `windowd` (`sxnwm_connect`), submit the whole surface or
  rectangles, wait for composed or idle, drain the input ring, ask for a
  size and follow resizes, see the close request, and publish the title and
  icon (`sxnwm_set_title`, `sxnwm_set_presentation`).
- `include/sxn/sxnwidgets.h` — widget seed, not a toolkit: Label,
  Button and single-line Field with a hand-drawn 5x7 font, hit-testing,
  focus and field editing. Enough for one form; a real toolkit binds
  the system fonts later.

## Connecting a window

A native client connects to `windowd` like any other (WM protocol v5,
`savanxp/wm_protocol.h`): `sxnwm_connect` opens the port `windowd`, writes
the hello asking for `SAVANXP_WM_HELLO_INPUT_RING`, reads the welcome and
receives four handles from the event pipe -- surface section, wake event,
submit event, input ring -- mapping the two sections. The event pipe stays
open but is never read: input records are the same 32-byte
`savanxp_wm_event`s, on the ring, which drops when full like the pipe. The
wait pattern is: reset the wake event, re-check the header AND the ring,
then wait. The read of the welcome blocks (there is no native poll yet).

## In-tree native programs

A program built by the tree is native when `cmake/UserPrograms.cmake`
registers it with `NATIVE`; it has to be `STATIC`. The platform services are
native too, but they are not programs of the POSIX subsystem: they live in
`system/<service>/` and `cmake/SystemServices.cmake` registers them with
`savanxp_system_service`. It links the same runtime as
everyone else -- the parts that make no system call (`memset`, string
functions) work unchanged -- but any POSIX call is `ENOSYS`, and `main` must end
in `sxn_exit`. Four are:

- `compositord`: the GPU through `SXN_SYS_OPEN_DEVICE`/`_DEVICE_CONTROL`, the
  display memory a section it creates, and a port of its own that `windowd`
  connects to (`compositor_protocol.h`), read and written through
  `SXN_SYS_READ`/`_WRITE`.
- `sessiond`: spawn, wait, terminate, the clock, and probing ports.
- `console`: the Command Prompt, an application rather than a service
  ([CONSOLE.md](../desktop/CONSOLE.md)): a window through `sxnwm.h`, `/bin/sh`
  started with `sxn_spawn_ex` over pipes from `sxn_pipe_create`, and one
  `sxn_wait_many` over the window and the pipes. Its title and icon come from
  `console.sunres.h` ([SUN.md](SUN.md#resources-compiled-in)).
- `windowd`: written against the POSIX SDK and kept that way; `windowd_sys.h`
  renames its POSIX calls to SXN equivalents in `windowd_sys.c`. A large
  program can move to the native API that way, one renamed call at a time,
  as long as it needs no files.

Native programs have `malloc`, `free`, `realloc` and `calloc`: the allocator
every program shares, with its arenas mapped through SXN by the native
runtime (`system/runtime/sxn_runtime.c`, linked into every native target; see
[NATIVE_HEAP.md](NATIVE_HEAP.md)).

## Build, manifest, launch

```bash
./tools/build-user.sh --source sdk/myapp --name myapp --native
```

`--native` stamps the OSABI from the neutral ABI header; without it
the same sources build a POSIX binary instead. Ship a `.sxres`
manifest next to the sources so the launcher lists it — `category=`
is what puts it in the menu (`subsystem=native` is informational,
the loader trusts the stamp):

```ini
name=My app
description=What it does (no POSIX)
version=system
vendor=SavanXP
subsystem=native
category=Accessories
```

`progman --selftest` asserts the scan still lists the native menu
apps and still rejects category-less binaries; if your app vanishes
from the menu, the manifest is the first suspect.

## Debug and proof

- `sxn_log` prints to the kernel log (visible on the serial log in
  QEMU runs). Prefer phase logs (`form up`, `typed`, `esc`) over
  printf: there is no stdout convention to rely on.
- Exit codes are the verdict: `nativewidgets` exits 10 on success so
  the harness can tell a clean run from a timeout (1).
- Pixels are the proof for windowed clients: the `windowd` selftests
  sample the composed framebuffer. Match the layout margins in your
  app with the sampler or you will chase ghosts.
- Keyboard-only hardware proof lives in `native-hw-smoke`
  (`windowd --hw-native-test` + `tools/qmp_native_hw.py`): real PS/2
  keys through the production path into the form. Pointer records
  ride the same ring; the synthetic click in that test covers focus,
  not transport.

## Worked examples

- `sdk/nativetest` — no window: exit, log, spawn/waitpid, events,
  sections, views, waits, and a 50-round parent/child ring
  ping-pong. Scenario `native-test`.
- `sdk/nativedemo` — window answering a key with green.
  Scenario `windowd-smoke`.
- `sdk/nativewidgets` — the interactive form: counted button,
  editable field, two state squares, Esc composes the last frame.
  Scenarios `windowd-smoke` (scripted) and `native-hw-smoke` (real
  keys).

## Not there (deliberately)

Native owned dialogs, native fork, sockets and files, and toolkit
widgets beyond the seed. Per-handle inheritance is `sxn_spawn_ex`, by
naming the handles; there is no `FD_CLOEXEC`-style flag on a handle. See
`KERNEL_NATIVE_API.md` and [SUN.md](SUN.md) for the standing list.
