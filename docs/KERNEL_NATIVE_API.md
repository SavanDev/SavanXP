# Kernel native API (Sx)

Status: done (2026-10-16, slice 13). The layering stands: a native
kernel API with POSIX as a translation subsystem on top and a Win32-like
native userland beside it. See [`SYSTEM_LAYERING.md`](SYSTEM_LAYERING.md)
for the layering and [`NATIVE_APPS.md`](NATIVE_APPS.md) for writing
native programs.

## Naming: Sx, not Nt

The shape is NT-inspired (subsystems talk to one native surface instead of
reaching into each other's internals) but the names are this system's own:
`sx::current`, `sx::copy_from_user`, `sx::negative_error`. Errors stay
`-errno` (`savanxp_error_code`) for now; a status type can come with the
object/process factoring, not before.

## Invariants

- No POSIX-visible behavior change per slice. Numbers, structs, errors and
  scheduling stay identical; only the call path moves.
- The native dispatcher only uses the `sx::` surface
  ([`sx.hpp`](../include/kernel/sx.hpp): current process, user-memory copy,
  errors) plus `display::*` and `console::*`. It never reaches into
  `process.cpp` internals.
- `kernel/process.cpp` no longer implements user-memory copy itself: the
  single implementation lives in `kernel/sx.cpp`, and the `process::`
  wrappers exist only so drivers and the translation layer keep compiling
  untouched while they converge onto `sx::`.

## Done

- Slice 1: `kernel/native_dispatch.cpp` owns `dispatch_native_syscall`.
- Slice 2: `include/kernel/sx.hpp` + `kernel/sx.cpp` own user-memory copy,
  current-process identity and errors; the native dispatcher speaks `sx::`
  only, and `process::` keeps thin wrappers so drivers compile untouched.
- Slice 3: `posix_dispatch.cpp` owns `dispatch_posix_syscall` against the
  published `process::` translation boundary.
- Slice 4: standalone native ABI (`SXN_SYS_EXIT`, fallback cut) plus the
  first guest test proving it.
- Slice 5: `sx::` sync primitives plus native spawn/wait.
- Slice 6: `sx::` sections/views/waits plus the handle model.
- Slice 7: userland surface `sxn.h` plus native event/section/wait
  syscalls.
- Slice 8: native IPC ring plus the windowed-client design.
- Slice 9: first native window inside real `windowd`.
- Slice 10: native input ring plus toolkit seed.
- Slice 11: widgets on `sxnwidgets.h`, scripted interaction.
- Slice 12: real PS/2 keys plus launcher integration.
- Slice 13 (this change): POSIX bodies out of the kernel, guide, close
  (below).

## Slice 13 (this change): POSIX translation lives in its subsystem

- `subsystems/posix/kernel/posix_translation.cpp` owns the 33 POSIX-only
  bodies (fd table, fs paths, sockets, pipes, timers, semaphores,
  wait-many, sleep, fork, kill, poll): same signatures, same behavior,
  `sx::negative_error` instead of the old file-local one.
  `kernel/process.cpp` keeps lifecycle, the scheduler and the cores both
  dispatchers share (spawn, waitpid, events, sections, single waits).
- `include/kernel/process_detail.hpp` is the kernel-internal sharing the
  move needed (handle lookup/allocation, pipe pool, poll evaluation,
  waits, slot management): not subsystem API, no stability contract.
- `docs/NATIVE_APPS.md` is the native-app guide (headers, fixed fds,
  manifests, proof strategy, worked examples), listed in `docs/README`.
- What stays shared on purpose: the object cores both dispatchers call
  (events, sections, single waits) remain `process::` cores in the
  kernel; factoring them into `sx::` is future work, not leftover debt.

## Slice 12: real PS/2 keys, native apps in the menu

- `windowd --hw-native-test` (scenario `native-hw-smoke`): launches
  `nativewidgets`, focuses its field with one synthetic click, prints
  `NATIVE HW READY`, and from then on drains `/dev/input0` only through
  the production path (`wm_handle_key` to the active client, never a
  direct `route_key`). The host taps `h`, `i`, `Esc` over QMP; the typed
  square going green in the framebuffer proves PS/2 -> driver -> ring ->
  native client, and Esc must close the form (`NATIVE HW PASS`).
- Only the focus click stays synthetic: the pointer already has its own
  driver proof (`pointer-smoke`) and shares the same ring records the
  keys just proved. Pointer motion through `handle_pointer_event` into
  the ring is exercised daily by every POSIX app; no new claim there.
- Launcher: `sdk/nativedemo` and `sdk/nativewidgets` ship `.sxres` with
  `category=Accessories` (`subsystem=native` stays informational, the
  loader trusts EI_OSABI). The scan lists what declares a category, so
  both appear in the menu like any port; `progman --selftest` asserts
  they are listed and keeps rejecting category-less busybox applets.
- `progman-smoke` prepares both native builds before asserting; the
  QMP callback lives in `tools/qmp_native_hw.py` next to the other
  host drivers.

## Slice 11: native widgets, synthetic interaction

- `include/sxn/sxnwidgets.h`: Label, Button and single-line Field over
  `sxnwm.h` with a hand 5x7 font (small enough to delete when a real
  toolkit binds system fonts). Hit-testing, focus, field editing and
  press visuals; no toolkit claims beyond one form.
- `sdk/nativewidgets` is that form: button activations count, typed text
  lands in the field, two state squares turn green, Esc composes the last
  frame and exits 10. Layout margins are shared with the selftest.
- The selftest clicks, types `hi` and sends Esc through the real
  `route_pointer`/`route_key` router paths and samples the squares from
  the framebuffer. Pointer records ride the same ring the key did.
- Still synthetic: the records are fabricated in-test, not read from
  hardware. QMP-driven proof is slice 12 above.

## Slice 10: native input ring, `sxnwm.h` seed

- Transport by binary: `windowd` sniffs EI_OSABI at launch (8 bytes, magic
  plus OSABI) and gives native clients an input ring section on fd 7
  instead of the pipe. No negotiation, no version bump: the v4 structs are
  untouched, the pipe is still created, and POSIX paths never branch.
- `route_event` writes the same 32-byte record to the ring (SPSC, head
  published after the record, drop when full like the pipe) and signals
  the existing wake event. Reset, re-check header AND ring, then wait.
- `include/sxn/sxnwm.h`: attach, full-surface submit, composed wait and
  ring drain over a context struct. Extracted from the demo, used by it:
  substrate first, widgets with a second client.
- `sdk/nativedemo` answers a key with green and exits 10; the selftest
  injects a real `route_key` record and samples green from the
  framebuffer. Pointer records ride the same ring and are ignored.

## Slice 9: first native window, output-only

- `sdk/nativedemo` maps fd 3, paints a marker, publishes one full-surface
  batch and waits for composed, all through `sxn.h`. It never touches fd 4:
  the input pipe stays unread, so input-via-ring is still ahead.
- No `windowd` production change was needed: fork plus dup2 plus exec
  already serves a native binary, and composition never knew the client's
  subsystem. The new `windowd_native_client_selftest` launches it,
  composes it and samples the marker from the framebuffer.
- The v4 structs stay single-source (`savanxp/wm_protocol.h` plus
  `syscall.h`): the native side shares them, only the transport differs.
  (Input arrived in slice 10 above.)

## Slice 8: native IPC proven, windowed client designed

- `sdk/nativetest` now runs a 50-round ping-pong ring over the shared
  section with two manual-reset events: strict alternation, each side
  resets what it consumed, abort on first failure so a broken channel
  fails in seconds. Same shape as a WM submit/retire channel, no new
  syscalls: the slice-7 substrate sufficed.
- Windowed-client design (not built): rendezvous by inheritance. The
  launcher, which already speaks the WM protocol, creates the surface
  plus the wake event and spawns the native child with fixed handle
  numbers (today's `dup2` pattern, via the cloned table); `windowd`
  learns the surface through the launcher's existing channel, as owned
  windows already do. What `windowd` still needs for that: accept a
  pre-created surface for a new client, and signal an event instead of
  the event pipe. That plus the first windowed client is slice 9, with
  visual proof.

## Slice 7: userland surface + native sync/memory

- `SXN_SYS_EVENT_CREATE/SET/RESET`, `SXN_SYS_WAIT_ONE`,
  `SXN_SYS_SECTION_CREATE/MAP_VIEW/UNMAP_VIEW` (`0x1005`-`0x100b`), thin
  over the published `process::` cores. Handles come from the shared
  per-process table, so a spawned child inherits them.
- `include/sxn/sxn.h`: header-only typed wrappers for all 14 SXN
  syscalls, no tool or link change needed. `sdk/nativetest` speaks
  through it (one raw trap kept as a wire cross-check) and now proves
  cross-process sync plus shared memory: event plus section inherited by
  the child, marker answered, event waited, reset plus zero-timeout poll,
  unmap.
- Native spawn clones the caller's handle table (`clone_handles_for_spawn`;
  POSIX spawn still takes stdio only). Found by the test itself: without
  it the child maps EBADF. Per-handle opt-in stays future work next to
  FD_CLOEXEC.
- Deliberately not there: a toolkit, and WM/compositor attachment. A
  windowed native app needs native IPC first (sections plus waits over
  shared rings); that is slice 8, with a real client as proof.

## Slice 6: `sx::` sections/views/waits, handle model decided

- Neutral `sx::` section constructors, view map/unmap (with the W^X and
  mask policy) and wait satisfaction/consumption. The `process::`
  translation keeps flag translation, file checks, fd lookup/allocation
  and clock/parking orchestration. Same errors, same wakes.
- `begin_object_wait` is now a thin orchestrator over
  `sx::try_consume_objects` (>=0 consumed, `-errno` on misuse, `-EAGAIN`
  when nothing is signaled yet).

## Handle model (decided)

One shared integer handle table per process (`Process::handles`, entries of
`{object, granted_access, flags}`), used by POSIX fds today and by native
handles tomorrow. Granted access is enforced at use time; no second table:
two tables would need cross-table lifetime sync for zero isolation gain,
since isolation here comes from per-process tables plus the checks, not
from the integer format. What stays POSIX-flavored and is not renamed:
the 0/1/2 stdio convention, sequential allocation, path/VFS resolution,
signals and `poll`. A native handle table, if it ever differs, arrives as
a second view over the same entries — not as a second mechanism.

## Slice 5: `sx::` sync + native lifecycle

- Neutral `sx::` constructors and signals for events, timers and
  semaphores; the `process::` translation keeps flag translation, access
  masks and handle allocation. Same errors, same wakes.
- `SXN_SYS_SPAWN` (`0x1003`, same register convention as the POSIX spawn)
  and `SXN_SYS_WAITPID` (`0x1004`, pid or -1 plus status address) backed by
  the published `process::` machinery.
- `process::waitpid_child` is the shared core behind both WAITPID paths:
  the POSIX case kept its semantics by calling it, verified by the existing
  wait coverage plus the guest self-spawn.
- `sdk/nativetest` spawns itself with `argv[1] == "child"` (exits 42) and
  checks ECHILD, reaped pid and status on top of the slice-4 checks.

## Verification

```bash
python3 tests/host/smoke_runner_layout_test.py --root .
./build.sh build
./build.sh smoke smoke --smp 4
./build.sh smoke native-test
./build.sh smoke windowd-smoke
./build.sh smoke native-hw-smoke
./build.sh smoke progman-smoke
```
