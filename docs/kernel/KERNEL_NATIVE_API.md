# Kernel native API (Sx)

Status: done (2026-10-16, slice 13), isolation campaign complete (slices
A-E). The layering stands: a native kernel API with POSIX as a translation
subsystem on top and a Win32-like native userland beside it. The kernel now
includes no userland header, keeps no POSIX per-process state, and returns its
own status space. See [`SYSTEM_LAYERING.md`](../userland/SYSTEM_LAYERING.md) for the
layering and [`NATIVE_APPS.md`](../userland/NATIVE_APPS.md) for writing native programs.

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
  ([`sx.hpp`](../../include/kernel/sx.hpp): current process, user-memory copy,
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
- Slice 13: POSIX bodies out of the kernel, guide, close.
- Isolation A: the kernel stops including the POSIX SDK.
- Isolation B: the current directory left `Process`.
- Isolation C: poll parking left `Process`.
- Isolation D: POSIX signals left `Process`.
- Isolation E: the error space belongs to the kernel. The
  campaign is complete: the kernel includes no userland header, keeps no
  POSIX per-process state, and returns its own status space.
- Slice 14: handles, devices and the clock for services.
- Slice 15: ports, handle passing, POSIX on the native floor.
- Slice 16: `SXN_SYS_TERMINATE`, a parent ending its child.
- Slice 17: `SXN_SYS_WAIT_MANY`, and streams as wait targets.
- Slice 18: `SXN_SYS_PROCESS_QUERY`.
- Slice 19: `SXN_SYS_SPAWN_STDIO`, `SXN_SYS_SET_NONBLOCKING`.

## Isolation: making the kernel indifferent to userland

The goal is not a second userland API. It is that deleting POSIX (or any
userland) leaves the kernel untouched. Measured against that standard,
slice 13 was necessary but not sufficient: the kernel still reached into
the POSIX SDK header for every data struct, and it still kept POSIX state
in `Process`.

| Slice | Removes | Risk |
|-------|---------|------|
| **A** (done) | The POSIX SDK include from `kernel/` and `include/kernel/` | mechanical |
| **B** (done) | `cwd` from `Process` (POSIX path resolution state) | medium |
| **C** (done) | poll parking from `Process` | high |
| **D** (done) | POSIX signals from the process state | medium |
| **E** (done) | errno from the kernel's return path | semantic |
| **C** | poll parking from `Process` (`poll_entries`, `WaitReason::poll`) | medium |
| **D** | signals from the process state (`pending_signals`, `SignalFlags`) | medium |
| **E** | errno from the return path (`savanxp_error_code`) | semantic |

### Isolation A: the kernel speaks a neutral contract, not the POSIX SDK

`include/abi/savanxp_abi.h` holds the wire contract the kernel and every
userland layer share: the data structs (gpu, framebuffer, input, audio,
net, sections, WM) and the vocabularies the kernel genuinely owns
(open access, poll bits, signal numbers, socket kinds, process state).
The SDK's `savanxp/syscall.h` keeps the POSIX vocabulary and includes it:
system-call numbers, mode bits, seek, fcntl, socket options.

`kernel/` and `include/kernel/` no longer include `savanxp/*`; only the
POSIX layer (`subsystems/posix/kernel/`) does, which is where POSIX
belongs. A layout guard asserts both halves, so the SDK header cannot
creep back in through a driver.

Two facts this exposed, worth recording because they are easy to
mis-measure: the kernel used **no** POSIX *enum type names* but did use
16 POSIX *enumerators* (`SAVANXP_PROC_*` in state reporting,
`SAVANXP_SOCK_*`/`IPPROTO_*` in the network stack, `SAVANXP_SO_*` socket
options, one syscall number in a comment). Measuring the enum names would
have said "zero coupling" and been wrong; those enumerators are what
slices B-E have to remove.

### Isolation B: the current directory left the kernel

`Process` had `char cwd[256]`: the kernel knew where each process stood in
the filesystem. That is path-resolution state, so it belongs to the layer
that resolves paths.

The kernel now reserves, copies and frees **subsystem-private per-process
state** without ever reading it: each subsystem registers the size it needs
(`subsystem::register_process_state_size`), the kernel allocates in
`ensure_subsystem_state` and frees in `release_process_resources`. POSIX
declares `posix::ProcessState` (the cwd) in
`subsystems/posix/kernel/posix_process_state.hpp`; native registers zero,
which is what makes "no POSIX left in the kernel" measurable instead of
hopeful.

Two rules that are easy to get wrong and are why this is not mechanical:

- **Same size, keep the bytes.** `exec` does not change the cwd, so a
  POSIX-to-POSIX exec must preserve the state, not zero it. A size change
  (native-to-POSIX) does get fresh zeroed state, which POSIX reads as `/`.
- **`exec` re-checks the size.** The exec path sets `subsystem_id` from the
  new image's OSABI, so it must call `ensure_subsystem_state` too. Without
  that, a native process that execs a POSIX binary silently keeps a null
  state and lands in `/`. The new `cwdtest` exists because of this.

`cwdtest` is the assertion that was missing: nothing tested `chdir`/`getcwd`
before this slice, and it is exactly the inheritance (`fork`) and
preservation (`exec`) behaviour that moving state breaks quietly. It checks
the initial cwd, chdir/getcwd round-trip, relative resolution against the
cwd, the two failure paths, fork inheritance and exec preservation. Verified
to fail with the preservation rule deliberately broken, so it has teeth.

- `subsystems/posix/kernel/posix_translation.cpp` owns the 33 POSIX-only
  bodies (fd table, fs paths, sockets, pipes, timers, semaphores,
  wait-many, sleep, fork, kill, poll): same signatures, same behavior,
  `sx::negative_error` instead of the old file-local one.
  `kernel/process.cpp` keeps lifecycle, the scheduler and the cores both
  dispatchers share (spawn, waitpid, events, sections, single waits).
- `include/kernel/process_detail.hpp` is the kernel-internal sharing the
  move needed (handle lookup/allocation, pipe pool, poll evaluation,
  waits, slot management): not subsystem API, no stability contract.
- `docs/userland/NATIVE_APPS.md` is the native-app guide (headers, fixed fds,
  manifests, proof strategy, worked examples), listed in `docs/README`.
- What stays shared on purpose: the object cores both dispatchers call
  (events, sections, single waits) remain `process::` cores in the
  kernel; factoring them into `sx::` is future work, not leftover debt.

### Isolation C: the kernel stopped parking processes for poll()

This was the hardest of the set: poll was the last POSIX mechanism the kernel
*executed*, not just stored.

`Process` carried `poll_user_fds`, `poll_count` and a 64-entry
`poll_entries` copy (512 bytes per process, 32 KiB of kernel BSS), plus a
`WaitReason::poll`. The timer tick walked the table looking for those,
re-evaluated each entry against the handle table, committed the revents to
user memory and woke whoever became ready. Four separate wakeup paths did the
same, which is what keeps compositor latency honest.

What the kernel has now is the neutral statement "this process is parked with
a deadline" (`wake_deadline_ms`, `State::blocked_wait`). What it *is* waiting
for belongs to the layer that parked it:

- `subsystem::register_wait_recheck` — the owning subsystem gets the parked
  process and decides (`subsystem::recheck_parked_process`).
- POSIX's answer is `posix::recheck_parked_poll`, which evaluates its own
  request, which now lives in `posix::ProcessState` next to the cwd.
- `WaitReason::poll`, the three fields and both evaluators are gone from the
  kernel; `poll_open_file` survives only as a published helper POSIX calls.

The copy-in-kernel rule survives the move unchanged and is why the state is
still an array: a tick-driven re-evaluation cannot switch CR3 a thousand times
a second per parked process. User memory is touched exactly once, at commit.

Two measurements worth keeping:

- 32 KiB of unconditional kernel BSS is now paid only by POSIX processes.
- **Negative test:** unregistering the wait hook makes `polltest` hang
  (timeout, not a clean failure). That is the shape a broken poll wakeup has,
  and it is why `windowd-smoke` — a compositor that waits for every frame by
  poll — is the real proof here, not the unit-level smoke.

### Isolation D: the kernel has no signals

Measuring first paid off here: `pending_signals` and `last_signal` were
**written and never read**. No path in the kernel consulted them, there is no
`sigaction` and no signal frame, so this kernel has no signal delivery at all.
The two fields plus a `SignalFlags` enum plus `signal_flag_for` existed only to
name POSIX signals inside the kernel.

What actually happens on a signal is smaller and stays:

- `kill(pid, n)` in the POSIX translation: SIGINT/SIGKILL/SIGTERM terminate the
  process with `128+n`; SIGCHLD is a no-op that returns success.
- The notification that a child exited, for a parent that is waiting, is
  `waitpid` — which the native subsystem uses too (`SXN_SYS_WAITPID`), so the
  wakeup it needs (`wake_waiting_parent`) is neutral and stays in the kernel.

The vocabulary moved to `subsystems/posix/kernel/posix_signals.hpp` with the
POSIX mapping beside it. `sigtest` is the check that the behaviour is intact:
SIGKILL on init denied with EACCES, SIGTERM to a child observed as `128+15`.

Worth recording: this is a smaller slice than B or C, and the reason is a
measurement, not a shortcut. "Move the signal state to POSIX" would have been
the wrong plan; the right one was "find out what the signal state is for",
and the answer was: nothing.

### Isolation E: the error space belongs to the kernel

The last one, and the only semantic change of the set. The kernel returned
`-errno` everywhere, named `savanxp_error_code`, and spelled every constant
`SAVANXP_E*`: 345 of them across the kernel.

Now `include/abi/savanxp_abi.h` declares `enum sx_status` as the canonical
space, and POSIX is downstream of it:

- the kernel writes `sx_status` and `SX_EINVAL` and never the POSIX names;
- `typedef enum sx_status savanxp_error_code` plus `#define SAVANXP_EINVAL
  SX_EINVAL` keep every existing userland source compiling untouched.

The numbers did not move, on purpose. Renaming the space while keeping the
values is what makes the change *observable as a no-op* -- any drift in an
error becomes a smoke failure instead of something nobody can attribute. A
userland with a different error vocabulary now maps `sx_status` in its own
translation, which is the separable part that was missing before.

Two things stayed on purpose, and the guard says so:

- `SAVANXP_EDITION` and `SAVANXP_EVENT_*` are not errors; a mechanical rename
  of the `SAVANXP_E` prefix would have taken them, and the revert pass had to
  put them back. Renaming by prefix is exactly as unsafe as it sounds.
- The constant names are shared wire vocabulary. Only the *ownership*
  inverted; renaming 345 call sites to a name nobody else uses would be
  cosmetics, not isolation.

## Slice 19: spawning with stdio only, non-blocking streams

- `SXN_SYS_SPAWN_STDIO` (`0x101f`): `SXN_SYS_SPAWN` whose child gets only the
  caller's stdio, as the POSIX spawn does. A service starting a program it has
  nothing to give must not clone its table into it: a client's event pipe
  write end living on in another process would keep that client from ever
  seeing the service close it. `sessiond` uses it.
- `SXN_SYS_SET_NONBLOCKING` (`0x1020`): rdi = a pipe or device handle, rsi =
  1 or 0. The native read and write already honoured the stream's
  non-blocking mode; this sets it, for every handle to the stream, as POSIX
  `fcntl(O_NONBLOCK)` does. A non-blocking pipe write of a record that fits
  goes whole or not at all.

Both are what a native `windowd` needs: it starts `compositord` and its
selftest's applications, and it never waits on a client's pipe.
`nativetest` covers both, and its `check` now logs the line that failed.

## Slice 18: querying a process

- `SXN_SYS_PROCESS_QUERY` (`0x101e`): rdi = pid, rsi = `struct
  sxn_process_query` -- pid, parent, state (`SXN_PROCESS_RUNNING`, or
  `SXN_PROCESS_ENDED` while waiting for its parent's `waitpid`, with its exit
  code), open handle count and image name. `-ENOENT` for a pid that names
  nothing, a reaped one included.
- Any process may ask about any other: it is what a task list shows, and what
  POSIX `proc_info` already exposes. Native only.

`windowd` needs it to run native: whether a client is still alive, a client's
parent for the shell rule, the name that labels a window it did not start,
and its selftest's handle counts. `nativetest` sees its child running, then
ended with the status it was given, then gone.

## Slice 17: waiting on several handles, streams included

- `SXN_SYS_WAIT_MANY` (`0x101d`): rdi = `int32_t` handles, rsi = count (1..16),
  rdx = timeout. Wait-any: returns the index of the first handle signaled, or
  `-ETIMEDOUT`. The same `begin_object_wait` core as POSIX `wait_many`.
- **Streams are wait targets** (`sx::is_wait_target`), for WAIT_ONE, WAIT_MANY
  and POSIX `wait_one`/`wait_many` alike: a pipe, device or socket opened for
  reading is signaled while a read would not block -- data, or end of stream.
  Level-triggered: waiting consumes nothing, reading does. A write end is
  `-EINVAL`.
- A stream has no signal of its own to wake a parked wait. The kernel's
  recheck of parked processes, already run on every pipe write, every object
  signal and every tick, now completes object waits too -- the path the POSIX
  `poll` already took. So a pipe write wakes the waiter at once; a device
  (keyboard, mouse) within a tick.

This is what a service needs to run its loop on the native API: `windowd`
waits on the keyboard, the mouse, the session's submit event, its port and
the connections that still owe their hello. `nativetest` covers the index,
timeouts, a readable and an ended pipe, the level-triggered rule, the
refusals, and a parked wait woken by another process's write.

## Slice 16: ending a child

- `SXN_SYS_TERMINATE` (`0x101c`): rdi = pid, rsi = exit status. Ends a child of
  the caller through the same `terminate_process` the POSIX `kill` uses, and
  the parent's `waitpid` reports the status it chose. Only the caller's own
  children: having created a process is the right to end it, the closest thing
  to NT's handle with terminate access while there are no process handles. A
  child that already ended is not an error; anything else's pid is `-ECHILD`.
- Native only, like the other lifecycle calls: POSIX has `kill`, with its own
  rules. The user is the session manager, `sessiond`, ending what is left of a
  session (docs/desktop/DESKTOP_ARCHITECTURE.md#the-session-manager-sessiond).

`nativetest` spawns a child that blocks forever, ends it, checks the status
`waitpid` reports, and that the reaped pid and a process that is not its child
are refused.

## Slice 15: ports, handle passing, and POSIX on the native floor

- `SXN_SYS_PORT_CREATE`, `_CONNECT`, `_ACCEPT` (`0x1017`-`0x1019`): a named
  rendezvous point (`object::PortObject`, 16 in the system, names up to 31
  bytes), CSRSS's `\Windows\ApiPort` in shape. Connecting creates two pipes
  (`process::detail::create_pipe_pair`, a kernel core now, not a subsystem's):
  the client gets the write end of one and the read end of the other; the other
  two wait in the port with the client's pid until the service accepts. The
  port is waitable and signaled while connections are pending (waiting does not
  consume; accepting does). Closing the last handle frees the name and drops
  what was never accepted, which the client sees as end of stream.
- `SXN_SYS_PIPE_SEND_HANDLE`, `_RECEIVE_HANDLE` (`0x101a`, `0x101b`): the same
  handle queue as the POSIX `pipe_send_handle`, for native processes.
- **POSIX processes reach the native object calls.** The POSIX dispatcher
  forwards every number in `native_call_open_to_all_subsystems` (handles,
  events, sections, waits, pipe and device I/O, ports, the clock) to the native
  dispatcher, as a Win32 process calls ntdll. The lifecycle calls (EXIT, SPAWN,
  WAITPID) and the standalone ones (INFO, LOG, GFX) stay native-only: a
  subsystem defines those in its own terms. This is what lets an ordinary
  application find windowd by port. The isolation is unchanged: the kernel
  still knows nothing of POSIX.
- Port names are first come, first served; nothing stops a process from taking
  a service's name before the service starts. The session manager (`sessiond`) starting
  services first is what keeps that from happening.

`nativetest` covers ports between two native processes (connect by name, a
message each way, an event handed over the reply pipe); `handletest` does the
same from a POSIX process with `fork`, which also proves the forwarding.

## Slice 14: handles, devices and the clock for services

The calls a platform service needs to run without POSIX
([DESKTOP_ARCHITECTURE.md](../desktop/DESKTOP_ARCHITECTURE.md), phase 3):

- `SXN_SYS_CLOSE`, `SXN_SYS_DUPLICATE` (`0x100c`, `0x100d`): on the shared
  handle table, granted access carried over.
- `SXN_SYS_READ`, `SXN_SYS_WRITE` (`0x100e`, `0x100f`): pipes and devices
  only. They sit on the kernel's own cores (`try_pipe_read`,
  `pipe_write_chunk`, `device::read/write`), not on the POSIX translation, and
  park the process as `blocked_read`/`blocked_write` like the POSIX side, so
  the same wakeups serve both. A pipe write up to the pipe's capacity is whole
  or not at all.
- `SXN_SYS_OPEN_DEVICE`, `SXN_SYS_DEVICE_CONTROL` (`0x1014`, `0x1015`): a
  device by bare name (`gpu0`, `input0`, `serial`; letters and digits only, so
  the call cannot reach outside `/dev`), then the same control requests the
  POSIX side sends through `ioctl`. The `SXN_SYS_GFX_*` block stays as the
  whole-screen path for a simple program; a service driving the hardware uses
  the device.
- `SXN_SYS_CLOCK` (`0x1016`): monotonic nanoseconds.

`nativetest` covers all of them except pipe I/O, which a native process cannot
create pipes for yet; native `compositord` exercises that path.

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
