# System log

The kernel keeps the last lines it sent to the serial port, so a program in
the system can show them. Under QEMU the serial port goes to a terminal or a
file; on real hardware there is usually no cable, and without this the log
would be lost. [Log Viewer](../userland/LOG_VIEWER.md) is the program that
reads it.

## What goes in

Every character that leaves through the console's serial path, as lines:

| Writer | Path | Source of the line |
|---|---|---|
| the kernel and its drivers | `console::printf`/`write` | `kernel`, pid 0 |
| a program writing to the text console (stdout/stderr on the tty) | POSIX `write` on a tty handle | the program |
| a program writing to `/dev/serial` (`windowd`, `compositord` stats) | `serial_dev` | the program |
| a native program's `sxn_log` | `SXN_SYS_LOG` | the program |
| a POSIX program's `syslog(3)` (`<syslog.h>`) | `SXN_SYS_LOG`, open to every subsystem | the program |

`SXN_SYS_LOG` prefixes its line with the caller's subsystem and pid:
`native[4]: ...`, `posix[7]: ...`. `syslog` adds the `openlog` ident and, for a
warning or worse, the priority as a word; the log keeps no facility or priority
of its own. The `posix-syslog` scenario closes standard output and error and
reads its line back from the serial port.

Not logged: the text console's echo of what is typed (`klog::ScopedMute` in
`tty.cpp`). It is input, not a message.

The kernel side is `kernel/klog.cpp`. `console::write_char` and
`console::serial_write` hand it each character; text is the kernel's unless a
`klog::ScopedSource` names a process for the length of a write. Like the
console, it runs under the kernel lock and takes no lock of its own.

## The serial port

The serial port does not read the log: it needs every byte in order, a prompt
without its line break and binary written to `/dev/serial` included, and the
log keeps only complete, cleaned lines. Instead `kernel/console.cpp` queues
the bytes it sends to COM1 and to QEMU's debug console (port `0xe9`), and the
BSP's tick feeds them to the UART (`console::serial_tick()`):

- The queue holds 4 KiB. Writers and the tick both run under the kernel lock,
  which is all that keeps it consistent; it has no lock of its own.
- Each tick, if COM1's transmitter is empty, it sends what the UART's FIFO
  takes without waiting, up to 8 bytes: a 16550A takes 16, an 8250 or 16450
  one (read from IIR when the port is set up). Under KVM each byte costs
  about 100 us of port exits, so the cap keeps the tick's hold under 1 ms;
  8 bytes at 1000 Hz is 8 KB/s, twice what 38400 baud carries.
- A writer that finds the queue full sends its oldest byte at once, waiting
  for the port as every write did before the queue. Nothing is dropped; a
  burst larger than the queue costs what it always cost.
- Until the first tick, nothing would drain the queue, so boot output is
  written synchronously.
- A panic, a fatal exception and power-off call
  `console::serial_flush_synchronous()`: the queue goes out, and every later
  character is written as it comes, because the machine may stop before
  another tick.

Why: waiting for the port at every character, with the kernel lock held and
interrupts off, made each stats line a hold of several milliseconds, and the
tick's own reports of 15-40 ms the longest holds of an idle desktop
([SMP_ROADMAP.md](../roadmaps/open/SMP_ROADMAP.md), slice 5.1).

**Trap.** A core that hangs with the kernel lock held also stops the tick, so
up to 4 KiB of the last output can stay in the queue and never reach the
port. A panic flushes it; a silent hang does not. When the serial log of a
hang ends too early, that is where the rest is.

## Lines and records

A record is one line, without its line break, in a `struct sxn_log_record`
(`include/abi/savanxp_native_abi.h`), 256 bytes:

- `sequence`: 1, 2, 3... for the life of the system. Never reused.
- `time_ns`: the monotonic clock when the line's first character arrived; 0
  for the lines before the clock was calibrated, early in boot.
- `pid` and `source`: 0 and `kernel`, or the writer's pid and its process
  name at the time it wrote (`nightwood.sxe`, `windowd`). The name is copied,
  so the line keeps it after the process ends.
- `text`: at most 199 characters, NUL-terminated. Tabs and other control
  bytes become spaces; a backspace removes the last character of the line
  being built, as on the terminal.
- `flags`: `SXN_LOG_CONTINUED` when the text continues the previous record of
  the same writer, because the line was longer than a record;
  `SXN_LOG_UNTERMINATED` when a line was committed without its line break
  (below).

Empty lines are dropped.

Writers interleave: a driver logs while a program is halfway through a line.
Each writer builds its line in a slot of its own, and the line enters the ring
only at its line break, so lines are never mixed. There are eight slots, the
first one the kernel's. When a ninth writer appears, the writer heard from
least recently gives up its slot, and its unfinished text is committed with
`SXN_LOG_UNTERMINATED`. A line still being written is not visible; a program
that prints a prompt and waits leaves it pending until its next line break.

The log holds `klog::kCapacity` records (2048, 512 KiB in the kernel image).
The first `SXN_LOG_BOOT_KEPT` (512) are the boot and are **never
overwritten**: the processor, the PCI devices and what every driver found stay
readable however long the machine has been up, which is what a run on new
hardware is read for. The other 1536 are a ring for every later line, the
oldest overwritten first, so the periodic `timer-stats:` and `smp-stats:`
lines cannot push the boot out. A reader that asks for an overwritten sequence
past the boot lines starts at the oldest one the ring still holds. The log
lives in RAM: it does not survive a reboot.

## Reading: `SXN_SYS_LOG_READ`

`sxn_log_read(sequence, records, capacity)` (`include/sxn/sxn.h`) copies the
complete lines from the first one at or after `sequence` that the caller may
read, oldest first, and returns how many. Between the boot lines and the ring
there may be a gap of overwritten sequences. 0 means nothing newer. A reader
follows the log by passing the last sequence it saw plus one; a sequence
already overwritten starts at the oldest record. The call passes the record
size, and the kernel writes at most that much per record, so the struct can
grow at the end. Native only.

Who sees what:

- the kernel's lines and the lines of programs running as the system (uid 0,
  every platform service) are open to everyone: they are the state of the
  machine, and the person at it needs them when something fails;
- a program running as a user is seen by that user and by the system, not by
  another user.

The writer's uid decides, as it was when the line was written. So a gap in the
sequences a reader sees is either another user's lines or lines that were
overwritten; the call does not tell them apart.

`nativetest` checks the contract: a line it logs comes back with its pid and
name and after kernel lines, sequences grow, past the end is 0, bad sizes and
pointers are `-EINVAL`, and a child running as user 1001 sees its own line and
the system's while one running as 1002 does not see 1001's.
