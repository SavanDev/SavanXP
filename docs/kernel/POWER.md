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
| the ACPI power button (QEMU's `system_powerdown`) | the SCI handler, `sci_interrupt()` in `acpi.cpp`, tells `init`, which [ends the session](#the-power-button) as for the Start menu |
| anything where `init` does not serve its port (the rescue system, a smoke run) | `power_shutdown()` and `sxn_request_shutdown()` fall back to `/dev/power`, and the power button powers off at once |

Any process may ask, as any process may open `/dev/power`. The kernel side of
all of them ends in the same `power::shutdown()` or `power::reboot()`.
**Rule: nothing calls `acpi::shutdown()` or `acpi::reboot()` directly.** Those
two only cut the power, and anything that skips `power::` loses whatever the
writeback queue still holds.

## The sequence

0. **The screen**: "SavanXP is shutting down..." or "SavanXP is
   restarting..." ([below](#the-screen)).
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
   VirtualBox ports. **`acpi::reboot()`**: the FADT reset register, port
   `0xCF9`, the 8042 reset pulse. Each waits about a second and returns if
   the machine is still on.
4. **Still on**: the log says `power: the power-off did not happen; it is now
   safe to turn off the computer` (or `the restart`), the screen says "It is
   now safe to turn off your computer.", and the CPU halts for good. The disks
   were synced at step 1, so cutting the power by hand loses nothing.

The serial log shows `power: shutdown: syncing volumes` (or `reboot`).
A step that fails is logged and the next still runs:
`power: a volume did not sync; ...` or `power: the writeback queue did not drain; ...`.
The machine still goes down, because a failed disk will not recover by staying
on, and the volume replays its journal at the next mount: it loses the last
commits, never their order.

## The screen

The kernel draws it, not the session, so every path shows it: the Start menu
through `init`, the power button, and a program that opens `/dev/power` with no
session at all. `power::shutdown()` and `power::reboot()` draw it before they
sync anything:

1. **`display::emergency_console()`**, the operation a panic uses
   (FB4, [`FRAMEBUFFER_ROADMAP.md`](../roadmaps/open/FRAMEBUFFER_ROADMAP.md)).
   On DISPI the desktop may have left its second buffer on screen, so this
   makes the buffer the console writes into the one shown. On virtio-gpu the
   console's buffer is already the one shown, so there is nothing to do.
2. **The console stops drawing** on the framebuffer. The `power:` lines that
   follow go to the serial port only.
3. **`boot_screen::show_power()`** paints the boot splash's logo, name,
   edition and version on black, with the message where the boot bar runs. It
   is drawn once, in the geometry the console has now, because the desktop may
   have changed the mode. Then it is flushed when the display needs that.

`power-button-smoke` takes a screenshot of the machine once it has stopped and
checks it is this screen: black, with the name and the message, not the
desktop's last frame. That check fails if the screen is not drawn. The "safe to
turn off" screen has no scenario, because QEMU always powers off. It was
checked once by hand, with S5 and the ports skipped.

## Why it may run from an interrupt

The power button calls `power::shutdown()` from the SCI handler when nobody
watches it ([below](#the-power-button)). That is safe
because an external IRQ takes the big kernel lock
([`smp.hpp`](../../include/kernel/smp.hpp)), and every filesystem mutation runs
inside a system call that holds that lock from start to finish. SxFS never
drops it partway through (`smp::wait_for_interrupt()` is used only by the
network, the TTY and the PC speaker). So when the handler runs, no volume is
halfway through a mutation, its mutation lock is free, and the storage drivers
complete by polling, without needing interrupts.

The block layer drops it for one thing only: an idle application processor
waits for one writeback command with the lock released and interrupts off
(SMP slice 5.2, [BLOCK_WRITEBACK.md](BLOCK_WRITEBACK.md#draining)). The
queue is consistent the whole time; only the device is busy.
`block::shutdown_all()` first settles that command, spinning under the lock
until the device answers. That wait ends on its own, because the core running
the command needs nothing else to finish it: no lock, and no interrupt, since
its own are off. An SCI routed to that core waits for the command to end.
**Keep it that way.** A filesystem or block path that sleeps with the lock
released, and needs something from a core holding it to wake up, would make
this handler deadlock.

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
5. **`init` stops the services.** It [asks them](#services) to stop and
   waits for them, ends a fallback shell, then sends the second reply (stage `DONE`, status 0), and gives the requester
   up to two seconds to hang up. Then it opens `/dev/power`, and the kernel
   does [the sequence](#the-sequence) below.

A requester that only wants to start the shutdown closes after the first reply
(`power_shutdown()` and `sxn_request_shutdown()` do), so the desktop is never
blocked while the session ends.

The Win32 names, for orientation: the request is `ExitWindowsEx`, the close
request is `WM_QUERYENDSESSION` and `WM_ENDSESSION` folded into `WM_CLOSE`,
and a declined close is a `WM_QUERYENDSESSION` that returned `FALSE`.

### Services

`init` is also the service control manager. It starts `audiod` and
`elevationd` with `--service` (`SAVANXP_SERVICE_ARGUMENT`), and the read end
of a pipe as their standard input (`SAVANXP_SERVICE_CONTROL_HANDLE`). It keeps
the write end. Standard input is used because a POSIX spawn, which `init`
uses, passes a child its stdio and nothing else.

At shutdown, `init` writes one `struct savanxp_service_control` with
`SAVANXP_SERVICE_CONTROL_SHUTDOWN`, Windows' `SERVICE_CONTROL_SHUTDOWN`, to
every service (`init: asking audiod to stop`). Then it gives them five seconds
together (`INIT_SERVICE_STOP_MS`), shorter than Windows'
`WaitToKillServiceTimeout`. A service that is still running after that is
ended (`init: audiod did not stop within 5 s; ending it`). End of file on the
channel means `init` is gone, and a service stops then too.

- **`audiod`** saves what its 250 ms debounce still holds, waits for a save in
  flight, and exits (`audiod: stopping for the shutdown`,
  `audiod: sound preferences saved`). Before this, a volume changed just
  before the shutdown was lost.
- **`elevationd`** has nothing to save; it exits.

A new service that holds state reads the channel the same way, from POSIX
(`read` on fd 0, made non-blocking) or from SUN (`sxn_read` and
`sxn_wait_many` on handle 0).

`power-button-smoke` checks it on the second press. Both services have to be
asked and stop on their own before the session is declared over. The window
that closes sets the master volume to 37 as it goes, before `audiod`'s
debounce has run, and the next boot has to find 37. With the services killed
as before, the volume was lost in two runs of two.

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

## The power button

A press is a shutdown request like any other, as Windows treats it: the
session ends first and an application may still cancel it. The SCI handler
calls `power::button_pressed()`, which does one of two things:

- **Someone watches the button.** `init`'s shutdown thread opens `/dev/power`
  for reading and calls `POWER_IOC_WATCH_BUTTON`, which only a system process
  may use. A press is then a `struct savanxp_power_event` waiting on that
  descriptor (serial: `power: power button pressed; telling pid N`). `init`
  waits for it beside its port and ends the session as for any request, with
  nobody to answer (`init: shutdown requested by the power button`). If an
  application cancels, the machine stays up, as it does for the Start menu. A
  press during a shutdown already under way changes nothing.
- **Nobody watches, or the last press is still unread.** The kernel calls
  `power::shutdown()` at once. That covers the rescue system, smoke runs that
  do not start the session (`power-smoke`), and an `init` that has stopped
  answering. A button that does nothing is worse than an abrupt power-off, so
  pressing twice always powers off unless `init` took the first press.

`init` registers only once its thread runs. A press that comes earlier finds
no watcher and powers off directly, so no press is ever left unread.

`./build.sh smoke power-button-smoke` presses the button twice during the real
session, with the `session-end-smoke` window that turns down its first close
([`tools/power_button_smoke.py`](../../tools/power_button_smoke.py)):

1. The first press has to reach `init`, be cancelled by the window, and leave
   the machine running.
2. The second has to close the window, which writes a mark it never flushes,
   then end the session and go through `power::shutdown()` with the volume
   clean.
3. On the next boot `nativetest power-button` finds the mark.

Against a kernel that powers off on the first press, it fails at that press.

## What it does not do yet

- **Only init's services are asked.** A background program with no window
  that is not one of them (a process started from a shell, a fallback shell)
  is ended, not told.
- **The screen comes only when `/dev/power` is opened.** Between the end of
  the session and that moment, `init` stops the services and gives the
  requester up to two seconds to hang up. Meanwhile the desktop's last frame
  stays on screen. Windows shows its screen while the session logs off.
