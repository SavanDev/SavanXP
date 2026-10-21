# Command Prompt (`/bin/console`)

> Status: **built 2026-10-21.** The Command Prompt is the first application
> written against SUN ([SUN.md](../userland/SUN.md)), in `system/console`. It
> replaces the POSIX `shellapp`, whose window could not take a key while a
> command ran.

The Command Prompt looks and behaves like the console of Windows 2000: light
grey on black, an 80-column screen buffer with scrollback, the line editing of
`doskey`, and Ctrl+C. Like Windows, the window and the interpreter are two
programs: the window is the console host, the interpreter is `/bin/sh`.

## The split

```
desktop ── spawn ──▶ console            SUN, native: window, screen buffer, keys
                       │
                       └─ sxn_spawn_ex ─▶ /bin/sh --console 3    POSIX
                                            ├─ foreground job    reported on fd 3
                                            └─ `start`ed apps     never reported
```

| | Windows | SavanXP |
|---|---|---|
| Console host | `conhost.exe` (`csrss` before XP) | `console` (SUN) |
| Interpreter | `cmd.exe` | `sh` (POSIX) |
| Standard handles | `STARTUPINFO.hStd*` | `struct sxn_spawn_request` |
| Console API | `SetConsoleTitle`, `GenerateConsoleCtrlEvent`, ... | the control channel |

The console host never parses a command and the interpreter never draws a
pixel. That is the property `shellapp` lacked: it ran the command inside its
own event loop, so the window froze until the command ended, and a program that
read its standard input got nothing.

## Handles

`console` starts `/bin/sh --console 3` with `sxn_spawn_ex` and these handles:

| In `sh` | What | Direction |
|---|---|---|
| 0 | input pipe, read end | console → sh: the lines the user enters |
| 1 | output pipe, write end | sh and its children → console |
| 2 | the same output pipe | stderr shares stdout, so their order is kept |
| 3 | control pipe, write end | sh → console: the control channel |

Every child `sh` runs inherits 0, 1 and 2, which is what makes a program's
output appear in the window and its reads see the lines the user types. Fd 3
does not cross: the POSIX spawn hands a child only its stdio.

`console` waits on three things with one `sxn_wait_many`: the window's wake
event (input from `windowd`), the output pipe and the control pipe. Nothing
blocks the window.

## Input

The console is in line mode, as `ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT` is by
default on Windows: the console edits the line, echoes it into the screen
buffer, and writes it to the input pipe with a `\n` when the user presses
Enter. The editing keys are those of Windows 2000:

| Key | Effect |
|---|---|
| Left / Right, Home / End | move the caret in the line |
| Backspace / Delete | erase before / under the caret |
| Insert | toggle overwrite; the cursor becomes a block while it is on |
| Esc | clear the line |
| Up / Down | previous / next line in the history (50 lines) |
| F3 | the rest of the previous line |
| Page Up / Page Down, wheel | scroll the buffer; typing returns to the bottom |
| Ctrl+C | end the foreground job (below) |

There is no raw mode yet: a program cannot read single keys. That needs a
console mode request on the control channel and stays for the next slice.

## The control channel

Fd 3 of `sh` carries text lines, one request each:

| Line | Meaning | Windows analogue |
|---|---|---|
| `fg <pid> [<pid>...]` | a foreground job started: these processes | the processes attached to the console |
| `fg` | the foreground job ended | — |
| `cls` | clear the screen buffer | `cls` via `FillConsoleOutputCharacter` |

The console drains the output pipe before it acts on a control line, so a
`cls` never overtakes output written before it.

## Output

Bytes from the output pipe are UTF-8, drawn with the console face. `\n`,
`\r`, `\b` and `\t` (to the next multiple of 8) do what they do on a
terminal, and a line wraps at column 80 when the next character arrives, so a
line of exactly 80 characters followed by `\n` leaves no blank line, as in
Windows. There are no colours yet. Escape sequences are skipped rather than
printed (`ESC [ ... final`, `ESC ] ... BEL`), and `ESC [ 2 J` clears the
buffer, so a program written for a VT terminal does not leave garbage.

When output arrives while a line is being edited, the echo steps aside, the
output lands where the line was being typed, and the line is echoed again
after it.

## Ctrl+C

The kernel delivers no signals (Isolation D in
[KERNEL_NATIVE_API.md](../kernel/KERNEL_NATIVE_API.md)). Ctrl+C is therefore a
termination, decided by the console:

- With a foreground job, the console ends each of its processes with
  `sxn_terminate(pid, 130)`. They are descendants of the console, not
  children, which `SXN_SYS_TERMINATE` allows since slice 22. `sh` sees its
  `waitpid` return and prints the next prompt.
- With no job, the console abandons the line being edited, prints `^C` and
  enters an empty line, so `sh` prints a fresh prompt.

The shell itself is never ended, as `cmd.exe` ignores `CTRL_C_EVENT`. A program
started with `start` is never reported, so Ctrl+C does not reach it either,
which is how a GUI program started from `cmd` behaves.

## `sh` in console mode

`--console <fd>` changes `sh` in four ways and nothing else:

- The prompt is `<cwd>>` (`/disk>`), the default `$P$G` of `cmd`.
- `start <program> [args]` starts a program without waiting for it.
- `;` separates commands on one line (`start notepad; exit`).
- The foreground job and `cls` go to the control channel; `sh` reaps the
  programs it `start`ed once they end (it finds them with `proc_info`).

`sh` also stops on end of input, which it never could: reading the kernel tty,
end of input did not exist.

## Look

- Colours: attribute `07`, light grey (192,192,192) on black, as the default
  of the Windows 2000 console.
- Font: the console face (GNU UniFont 8x16, [UI_FONTS.md](UI_FONTS.md)),
  standing in for the 8x12 raster *Terminal*.
- Screen buffer 80 x 300, window 80 x 25, a vertical scroll bar on the right:
  the Windows 2000 defaults.
- Cursor: the small underscore that blinks every 500 ms; a block in overwrite
  mode.
- The title bar says `Command Prompt`, and `Command Prompt - <program>` while
  a foreground job runs, as Windows names the running program. The name comes
  from `sxn_process_query`; the title from `sxnwm_set_title`, the icon and
  accent from the resources compiled in.

## Opening it

- Start menu: Accessories → Command Prompt (Program Manager lists it too).
- **Ctrl+Alt+T** anywhere: Windows lets any Start menu shortcut carry a
  Ctrl+Alt key, and this is the one the Command Prompt ships with. AltGr does
  not trigger it, so no layout loses the character on AltGr+T.

## In the smokes

Before the console, the visual harness opened an app by clicking the Start
menu, finding *Program Manager* in it by its text, and walking Program
Manager's icon grid with the arrow keys, counting positions in a list sorted by
name. It broke whenever an app was added or renamed.

Now `Session.run(command)` in `tools/shoot_session.py` presses Ctrl+Alt+T,
waits for the console, types `start <program>; exit` and Enter. The console
closes, the program opens with the focus, and no other window stays behind.
`taskbar-smoke` does the same from `tools/taskbar_smoke.py`. Typing goes
through `Qmp.type_line`, which knows where the guest's default ES layout puts
`;`, `/` and the rest. `shoot.sh --scenario console` exercises the console
itself: a builtin, a long listing, the history, scrolling and Ctrl+C.
The program's parent is a shell that already exited, so it is re-parented to
`init` like any orphan; nothing in `windowd` depends on who started a normal
window ([DESKTOP_ARCHITECTURE.md](DESKTOP_ARCHITECTURE.md#who-is-the-shell-decided-by-parentage)
only concerns the shell role).

`progman-smoke` still exercises Program Manager itself; it is no longer the
path every other scenario walks through.
