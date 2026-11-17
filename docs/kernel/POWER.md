# Power-off and reboot

How the machine goes down: how the session is ended first, what the kernel
does once the power is asked for, who can ask, and what is still missing next
to a Windows NT shutdown.

The session half is `init`'s shutdown service
([`init.c`](../../subsystems/posix/userland/init.c)), `sessiond` and
`windowd`. The kernel half is `power::shutdown()` and `power::reboot()` in
[`kernel/power.cpp`](../../kernel/power.cpp), and the hardware half is
`acpi::shutdown()` and `acpi::reboot()` in [`kernel/acpi.cpp`](../../kernel/acpi.cpp).

## Who asks

| request | path |
| --- | --- |
| Start menu's Shut Down dialog, Program Manager, Task Manager | `init`'s `shutdown` port (`power_shutdown()`, `power_reboot()` in POSIX; `sxn_request_shutdown()` in SUN), which [ends the session](#ending-the-session) and then opens `/dev/power` |
| the ACPI power button (QEMU's `system_powerdown`) | the SCI handler, `sci_interrupt()` in `acpi.cpp`, straight to the kernel |
| anything where `init` does not serve its port (the rescue system, a smoke run) | `power_shutdown()` and `sxn_request_shutdown()` fall back to `/dev/power` |

Any process may ask, as any process may open `/dev/power`. The kernel side of
all of them ends in the same `power::shutdown()` or `power::reboot()`.
**Rule: nothing calls `acpi::shutdown()` or `acpi::reboot()` directly.** Those
two only cut the power, and anything that skips `power::` loses whatever the
writeback queue still holds.

## The sequence

1. **`fs::sync_all()`**: every mounted volume makes its metadata durable and
   marks itself clean, the way an unmount would. On SxFS that is both
   superblocks with `CLEAN` and the journal header cleared, so the next boot
   finds no journal to replay and host tools (`sxfs-cli`, `fscheck`) see a
   clean image.
2. **`block::shutdown_all()`**: drains the writeback queue
   ([`BLOCK_WRITEBACK.md`](BLOCK_WRITEBACK.md)), then runs every block
   driver's `shutdown` hook (NVMe's shutdown notification,
   [`NVME.md`](NVME.md)).
3. **`acpi::shutdown()`**: S5 through `PM1a/b_CNT`, then the QEMU, Bochs and
   VirtualBox ports, then `hlt` forever. **`acpi::reboot()`**: the FADT reset
   register, port `0xCF9`, the 8042 reset pulse, then `hlt` forever.

The serial log shows `power: shutdown: syncing volumes` (or `reboot`).
A step that fails is logged and the next still runs:
`power: a volume did not sync; ...` or `power: the writeback queue did not drain; ...`.
The machine still goes down, because a failed disk will not recover by staying
on, and the volume replays its journal at the next mount: it loses the last
commits, never their order.

## Why it may run from an interrupt

The power button calls `power::shutdown()` from the SCI handler. That is safe
because an external IRQ takes the big kernel lock
([`smp.hpp`](../../include/kernel/smp.hpp)), and every filesystem mutation runs
inside a system call that holds that lock from start to finish. SxFS and the
block layer never drop it partway through (`smp::wait_for_interrupt()` is used
only by the network, the TTY and the PC speaker). So when the handler runs, no
volume is halfway through a mutation, its mutation lock is free, and the
storage drivers complete by polling, without needing interrupts.
**Keep it that way.** A filesystem or block path that sleeps with the lock
released would make this handler deadlock on the mutation lock.

## The test

`./build.sh smoke power-smoke` presses the power button with writes still in
the queue, and then boots the machine again:

1. `nativetest power` (SUN, [`sdk/nativetest`](../../sdk/nativetest/main.c))
   writes a 512 KiB payload it never flushes, logs `NATIVE POWER READY`, and
   keeps rewriting a second file so commits are always in flight.
2. [`tools/power_smoke.py`](../../tools/power_smoke.py) sends QMP
   `system_powerdown`. The smoke machine runs with `-no-shutdown`, so QEMU
   stops instead of exiting. With the machine stopped, the script checks the
   disk copy: the serial log shows the `power::shutdown()` line and no failure,
   both superblocks carry the same sequence and `CLEAN`, and the journal has no
   pending transaction. Then it resets the machine.
3. On the second boot the payload exists, `nativetest power` reads back every
   byte, and the scenario ends with `NATIVE POWER PASS`.

Against the old power button, which called `acpi::shutdown()` directly, the
volume checks alone failed on every run, in three different ways: a pending
journal, superblocks without `CLEAN`, or a payload that did not read back after
the reboot. A change that breaks this sequence should fail this scenario.

## Ending the session

`init` owns the shutdown, as winlogon does in Windows. A program asks on
`init`'s port (`SAVANXP_SHUTDOWN_PORT_NAME`, the request and replies in
`savanxp_abi.h`), and `init` ends the session in order before it touches
`/dev/power`:

1. **`init` accepts.** The first reply (stage `ACCEPTED`) comes at once:
   status 0, or `-EBUSY` while another shutdown is under way. From then on
   `init`'s supervisor restarts nothing that exits. A second thread serves
   the port, because the first stays in `waitpid`. It keeps answering new
   requests (`-EBUSY`) while it waits for the session.
2. **`sessiond` ends the session.** `init` asks on `sessiond`'s port
   (`SAVANXP_SESSION_PORT_NAME`, which serves only its parent). `sessiond`
   stops restarting the desktop and connects to `windowd` with
   `SAVANXP_WM_HELLO_END_SESSION`, which `windowd` grants only to its own
   parent.
3. **`windowd` asks every application window to close**, exactly as its X
   does, and keeps the shell. A window that connects meanwhile is asked too.
   Each program answers as it answers the X:
   - by default the runtime ends it;
   - a program that answers itself (`on_close` in the `sxgui_app` and
     `sxngui_app` frames) asks "save changes?" (Notepad does), then quits;
   - if the user answers Cancel, the program turns the close down
     (`gfx_decline_close`, `sxnwm_decline_close`, the
     `close_declined` word in the surface header);
   - a window that does not close becomes "not responding", and the shell
     offers End Now. Nothing ends it behind the user's back, so a shutdown
     waits for the user as long as it takes, as Windows does by default.
4. **The answer travels back.** When no application window is left, `windowd`
   answers `sessiond` with 0. `sessiond` ends the desktop, `windowd` and
   `compositord`, answers `init` and exits. If a program turned its close down,
   `windowd` withdraws the other requests and answers `-ECANCELED`. The
   session goes on, and `init` answers its requester the same way.
5. **`init` stops the services** (`audiod`, `elevationd`, a fallback shell),
   sends the second reply (stage `DONE`, status 0), and gives the requester
   up to two seconds to hang up. Then it opens `/dev/power`, and the kernel
   does [the sequence](#the-sequence) below.

A requester that only wants to start the shutdown closes after the first reply
(`power_shutdown()` and `sxn_request_shutdown()` do), so the desktop is never
blocked while the session ends.

The Win32 names, for orientation: the request is `ExitWindowsEx`, the close
request is `WM_QUERYENDSESSION` and `WM_ENDSESSION` folded into `WM_CLOSE`,
and a declined close is a `WM_QUERYENDSESSION` that returned `FALSE`.

### Testing it

`./build.sh smoke session-end-smoke` runs the real session (`init`'s
`nativesessionend` spec) and starts `nativetest session-end` in it. A window of
its own (`session-end-client`) turns down the first close and takes the
second. The first shutdown request has to come back `-ECANCELED` with the
window alive. The second has to end the session with the window gone, a
request sent meanwhile has to get `-EBUSY`, and the machine powers off after
`NATIVE SESSION END PASS`.

`./tools/shoot.sh --scenario notepadclose` checks Notepad's side on screen:
Alt+F4 asks, Esc keeps the window and its text, a second Alt+F4 asks again,
and Discard closes it.

## What it does not do yet

- **The power button skips the session.** It goes straight from the SCI
  handler to `power::shutdown()`: the volumes are synced, but applications are
  not asked. Making it ask needs a way for the kernel to tell `init`.
- **Programs without a window are not told.** Services and background
  programs are ended, not asked: nothing like `SERVICE_CONTROL_SHUTDOWN`
  reaches them.
- **No "SavanXP is shutting down" screen**, and no "safe to turn off" screen
  when ACPI and the fallback ports all fail. The machine halts with the last
  frame on screen.
