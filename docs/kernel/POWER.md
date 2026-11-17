# Power-off and reboot

How the machine goes down: what the kernel does once something asks for
power-off or a reset, who can ask, and what is still missing next to a
Windows NT shutdown.

The code is `power::shutdown()` and `power::reboot()` in
[`kernel/power.cpp`](../../kernel/power.cpp); the hardware half is
`acpi::shutdown()` and `acpi::reboot()` in [`kernel/acpi.cpp`](../../kernel/acpi.cpp).

## Who asks

| request | path |
| --- | --- |
| Start menu's Shut Down dialog, Program Manager, Task Manager | `POWER_IOC_SHUTDOWN` / `POWER_IOC_REBOOT` on `/dev/power` (`power_shutdown()`, `power_reboot()` in libc) |
| the ACPI power button (QEMU's `system_powerdown`) | the SCI handler, `sci_interrupt()` in `acpi.cpp` |

All of them end in the same `power::shutdown()` or `power::reboot()`.
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

## What it does not do yet

Compared with Windows NT, this is only the last stage (`NtShutdownSystem`):

- **Applications are not asked.** There is no `WM_QUERYENDSESSION` or
  `WM_ENDSESSION`. Data a program holds in memory is lost without a prompt.
- **Processes are not told or stopped.** Nothing reaches services or
  background programs before the power goes.
- **There is no "safe to turn off" screen** when ACPI and the fallback ports
  all fail. The machine halts with the last frame on screen.

### Interaction with `init`

`init` plays no part in the sequence above: the kernel does all of it inside
the request, and no process runs again afterwards. The day applications and
processes are told first, `init` has to know the session is ending. Today it
restarts `sessiond`, `audiod` and `elevationd` when they exit
([`init.c`](../../subsystems/posix/userland/init.c)), so stopping them before
power-off would make `init` start them again. Ending the session needs a
shutdown state that `init` checks before restarting anything, or it needs
`init` to drive the shutdown itself.
