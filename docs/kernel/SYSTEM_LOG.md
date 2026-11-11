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

Not logged: the text console's echo of what is typed (`klog::ScopedMute` in
`tty.cpp`). It is input, not a message.

The kernel side is `kernel/klog.cpp`. `console::write_char` and
`console::serial_write` hand it each character; text is the kernel's unless a
`klog::ScopedSource` names a process for the length of a write. Like the
console, it runs under the kernel lock and takes no lock of its own.

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

The ring holds `klog::kCapacity` records (2048, 512 KiB in the kernel image);
the oldest are overwritten. It lives in RAM: it does not survive a reboot.

## Reading: `SXN_SYS_LOG_READ`

`sxn_log_read(sequence, records, capacity)` (`include/sxn/sxn.h`) copies the
complete lines from the first one at or after `sequence` that the caller may
read, oldest first, and returns how many. 0 means nothing newer. A reader
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
