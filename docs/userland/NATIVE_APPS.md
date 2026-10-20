# Native apps (Sx)

How to write a program that speaks the kernel's native API directly,
with no POSIX in between. POSIX remains the translated subsystem most
software uses; this document is for the standalone sidecar: demos,
tests, and future services that want the direct connection.

## What "native" means here

- The binary is stamped `EI_OSABI=0x53` and enters through the native
  dispatcher, which only implements the SXN contract
  (`include/abi/savanxp_native_abi.h`). POSIX numbers from a native
  process are `ENOSYS`; there is no fallback.
- The process still gets the 0/1/2 stdio convention and one shared
  integer handle table with granted access. `SXN_SYS_SPAWN` clones the
  caller's table into the child; the POSIX spawn takes stdio only.
- Errors are `-errno` (`savanxp_error_code`), same values as POSIX.
- `main` must never return: the stock `crt0` falls through to a POSIX
  EXIT that is `ENOSYS` for native. Every example ends in `sxn_exit`.

## The three headers

All header-only, no link or tool changes needed:

- `include/sxn/sxn.h` — typed wrappers for every SXN syscall: exit, log,
  spawn, waitpid, events, sections, views, waits, gfx, and handle close and
  duplicate, pipe and device read/write, device open and control, the clock.
- `include/sxn/sxnwm.h` — windowed-client substrate over a context
  struct: connect to `windowd` (`sxnwm_connect`), submit full-surface
  batches, wait for composed, drain the input ring.
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
registers it with `NATIVE`; it has to be `STATIC`. It links the same runtime as
everyone else -- the parts that make no system call (`memset`, string
functions) work unchanged -- but any POSIX call is `ENOSYS`, and `main` must end
in `sxn_exit`. Three are:

- `compositord`: the GPU through `SXN_SYS_OPEN_DEVICE`/`_DEVICE_CONTROL`, the
  display memory a section it creates, and a port of its own that `windowd`
  connects to (`compositor_protocol.h`), read and written through
  `SXN_SYS_READ`/`_WRITE`.
- `sessiond`: spawn, wait, terminate, the clock, and probing ports.
- `windowd`: written against the POSIX SDK and kept that way; `windowd_sys.h`
  renames its POSIX calls to SXN equivalents in `windowd_sys.c`. A large
  program can move to the native API that way, one renamed call at a time,
  as long as it needs no files and no heap.

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

Native owned dialogs, native fork, native pipe creation, sockets and files, toolkit
widgets beyond the seed, and per-handle spawn opt-in (next to
`FD_CLOEXEC`). See `KERNEL_NATIVE_API.md` for the standing list.
