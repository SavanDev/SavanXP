# Log Viewer

`/system/logview.sxe`, in the Start menu under System. A SUN program that
shows the [system log](../kernel/SYSTEM_LOG.md): every line the kernel and the
programs sent to the serial port. Its reason to exist is real hardware, where
the serial log has nowhere to go.

## The window

- **Categories**, on the left, each with its count:
  - *All lines*.
  - *Problems*: the lines that read like an error or a warning (below).
  - *Programs*, then one row per program name: `windowd`, `compositord`,
    `nightwood`. Runs of the same program share a row; the Source column
    keeps the pid apart.
  - *Kernel*, then one row per subsystem. A kernel line names its subsystem
    the way every driver writes it, `xhci: port 3 ...`: a prefix of letters,
    digits, `-`, `_` and `.` followed by `: `. Lines without one go to
    *(other)*.
- **The list**: time in seconds since boot, source, message. An error carries
  a red mark and a warning a yellow one.
- **Find**: keeps the lines whose message or source contains the text,
  ignoring case, inside the chosen category.
- **The panel below** shows the selected line in full, wrapped, with its time,
  source and sequence number.
- **Follow new lines** keeps the newest line in view; picking an older line
  turns it off. The log is read every half second.
- **Copy** (Ctrl+C, Edit > Copy Line) puts the selected line on the clipboard
  as `[time] source: message`.
- **Clear** empties the view; the kernel's ring is untouched, and lines that
  arrive later show up as usual.
- **File > Save Log...** (Ctrl+S) writes the lines shown, in `--print`'s
  format, to a file picked in the common Save As dialog
  ([FILE_DIALOG.md](../desktop/FILE_DIALOG.md)): the category and Find chosen
  decide what is saved. It starts in `/users/user` with `system-log.txt`,
  lists `*.txt` and `*.log`, and adds `.txt` to a name without an extension.
  The status bar says how many lines were saved and where. A USB stick
  mounted from Disk Management is the way to take the log off a machine.

The program keeps its own copy of up to 4096 lines, so while it stays open it
remembers more than the kernel's ring of 2048. What the ring overwrote before
the program started is gone.

A user sees the kernel's lines, the system's services and their own
programs, never another user's (see
[SYSTEM_LOG.md](../kernel/SYSTEM_LOG.md#reading-sxn_sys_log_read)).

## Problems

The log has no levels, so the classification goes by words, case-insensitive,
at the start of a word: `panic`, `fatal`, `error`, `fail`, `fault`, `corrupt`
make an error; `warn`, `timeout`, `timed out`, `unsupported`, `retry`,
`retrying`, `lost` make a warning. A word followed by `=` is a counter's name
and does not count: `fault=0` in a statistics line is not a fault. It is a
heuristic. A message that should stand out is best written with one of those
words; a counter is best written as `name=value`.

## Without a window

```sh
logview --print                # everything
logview --print problems       # all, problems, kernel, programs
logview --print xhci           # one subsystem or program
```

Writes `[time] source message` lines to stdout and exits; it works from the
text console, where there is no desktop. `logview --selftest` checks the
parsing and the reading against the live log, saves the whole log to
`/users/logview-selftest.txt` and reads it back, and runs the file dialog's
own checks (`sxn_file_dialog_selftest`); `./build.sh smoke
logview-smoke` runs it, and `./tools/shoot.sh --scenario logview` looks at the
window, and saves through the dialog twice: once under the suggested name,
then over it, to see the replace question.
