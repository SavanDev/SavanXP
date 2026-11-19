# Savan BASIC, the environment

`/system/sxbide.sxe` is the development environment of SxBASIC
([SXBASIC.md](SXBASIC.md)): open a project, edit its files, run it, find
its errors at their line and make its executable. It is a SUN program drawn
with SXGUI, listed in All Programs under Development as **Savan BASIC**, and
it opens a `.sbp` from Files. It is phases 4 to 6 of the
[roadmap](../roadmaps/done/SXBASIC_ROADMAP.md): a form is drawn in the
designer or edited as text, both being the same text, and a program runs
under the [debugger](#the-debugger).

```text
sxbide [project.sbp]
sxbide --selftest <script>
```

## The window

One window, the panes inside it, as Visual Basic's docked layout:

- **Menu bar and toolbar.** File (New Project, Open Project, Save, Save All,
  Save Project As, Exit), Edit (Undo, Redo, Cut, Copy, Paste, Select All),
  View (Code, F7; Object, Shift+F7), Run (Start or Continue, Break, End,
  the steps, Toggle Breakpoint, Add and Delete Watch, Make SXE). The
  toolbar repeats Open, Save All, Start, Break, End and Make SXE. F5 starts
  the program, Ctrl+S saves the file shown, and Ctrl+O opens a project.
- **Project.** The project file, then its forms and modules in the order
  the project lists them. A file with unsaved changes shows ` *` after its
  name. Choosing one shows it: a form in the
  [designer](#the-form-designer), the rest in the editor.
- **Properties.** The property grid (`SXGUI_PROPGRID`) on the block the
  caret is in. See [the property grid](#the-property-grid-on-text).
- **The code editor.** One `SXGUI_TEXTEDIT` per file, each with its own
  undo history, line numbers, BASIC's colours (keywords navy, comments
  green) and the indent kept on Enter. The line of the last error carries
  the current-line mark.
- **Output.** What the last Start or Make SXE printed, and the
  environment's own lines (opened, saved, ended with exit code n).
  Double-clicking or pressing Enter on a line that names a file and a line
  goes there.
- **Locals and Watch.** Beside Output: while the program is stopped, the
  variables of the procedure it stopped in, and the watched names with
  their values.
- **Status bar.** The caret's line and column, and the last message.

## The form designer

A form opens in the designer; View > Code (F7) shows its text, View >
Object (Shift+F7) the designer again. The designer replaces the editor in
its pane:

- **The toolbox** on its left: Pointer, then every control a form holds
  (the kinds and their defaults come from `libsxbasic`'s tables, the ones
  the compiler checks against).
- **The form** on grey, with its caption, its client area at its `Width`
  and `Height`, a dot every 8 pixels, and its controls painted by SXGUI as
  they will look at run time but inert (`sxgui_paint_widgets`). A control
  with `Visible = False` still shows. Timers have no place on a form and
  sit in a tray under it.

With a control chosen in the toolbox, dragging on the form draws it; a
click places it at its default size. It is named after its kind with the
first free number (`Button1`), takes its name as its `Text` where it has
one, and the toolbox goes back to Pointer. A Timer is placed by clicking
anywhere, with `Interval = 1000`.

With the Pointer, a click selects the control under it (the form where
there is none) and dragging moves it; dragging one of the selected
control's eight handles resizes it, and the selected form resizes from its
right, bottom and bottom-right handles. Positions and sizes snap to the
8-pixel grid; a move keeps the control's offset within the cell only when
it is not moved at all. Delete removes the selected control, Esc puts the
toolbox back on the Pointer, Ctrl+Z and Ctrl+Y undo and redo, while no
other pane has the keyboard.

Double-clicking a control opens the code at its default event: `Click`
for a button, a check box, a radio button, a list and a combo box,
`Change` for a text box and the bars, `Tick` for a timer, `Form_Load` for
the form. When the form's code has no such `Sub`, one is added at its end
with the caret inside.

**The text is the model.** The designer reads the form's block again for
every paint and keeps nothing else. Selecting a control puts the caret on
its `Begin` line, which is what puts the [property
grid](#the-property-grid-on-text) on it; going back to the designer
selects the block the caret is in. What a drag changes, the designer writes
into the control's `Left`, `Top`, `Width` and `Height` lines (adding the
ones the block left out) as one replacement of the block: one step of the
file's undo. A new control is a block added before the form's `End`, a
deleted one is its block removed.

## The property grid on text

In this version the grid reads the text. It does not hold a model of the
form. It shows the `Key = value` lines of one block:

- **In the project file**, every key of the project.
- **In a form file**, the innermost `Begin ... End` around the caret: the
  form, a control or a menu. Lines of the blocks nested inside it are left
  out. With the caret in the form's code below the block, the grid shows
  the form's own block.
- **In a module**, nothing.

A row's kind comes from what the line holds:

| The line holds | The row is |
|---|---|
| `Kind` | a list: `Console`, `Windows` |
| `True` or `False` | true/false |
| a number written `&H...` | a colour, with its swatch |
| anything else | text |

A string value shows without its quotes, and the grid puts them back,
doubling any `"`, when it writes the value. Writing a value replaces that
line's value in the editor with `sxgui_textedit_replace`, so the change is
one step of the file's undo. The grid follows the caret and the text on the
next tick (10 per second), but not while a value is being edited.

Only the properties written in the file are listed. A property left out
takes its default, and adding one means typing its line.

## Start, End and Make SXE

Start and Make SXE save every changed file. Then they start the runner,
`/system/sxbasic.sxe`, as a child process:

- Start runs `sxbasic --debug <project.sbp>`, under the
  [debugger](#the-debugger).
- Make SXE runs `sxbasic --make <project.sbp> <folder>/<Name>.sxe`, next to
  the project and named after its `Name`.

The child's standard output and error go to one pipe, which the
environment reads on its tick without blocking. A program that loops or
crashes therefore does not take the environment with it. A Windows program
opens its own window, since `sxngui_app` gives each process one main window
([roadmap](../roadmaps/done/SXBASIC_ROADMAP.md#run-starts-a-separate-process)).
When the pipe ends, the environment waits for the child and prints its exit
code. End terminates the child with `sxn_terminate`.

While a child runs, Start and Make SXE are disabled and End is enabled.

### Errors at their line

The runner prints a compile error as `file:line:column: message` and a
runtime error as `Runtime error n: description (file, line n)`
([SXBASIC.md](SXBASIC.md#errors)). The first such line of a run is the
run's error. When the child ends, the environment does three things:

- it opens that file;
- it puts the caret at the start of that line;
- it marks the line in the gutter.

A file is matched by the name the project gives it, or by its last path
component.

## The debugger

Every Start runs under the debugger, as Visual Basic's Run did; with no
breakpoint set, it never stops and costs a check per statement.

- **Breakpoints.** F9 toggles one on the caret's line, and so does a click
  on the gutter's mark column; it shows as a red dot. A breakpoint on a line
  with no statement never stops. Breakpoints are kept by line number: lines
  added or removed above one do not move it.
- **Running and stopping.** F5 starts, or continues from a stop. F8 (Step
  Into) stops at the next statement, in a procedure it calls too; Shift+F8
  (Step Over) at the next one of this procedure or a caller;
  Ctrl+Shift+F8 (Step Out) at the next one of a caller. F8 and Shift+F8
  with nothing running start the program stopped at its first statement.
  Break stops a running program before its next statement (a Windows
  program between events, or a console one between slices of 20000
  instructions).
- **A stop.** The file opens at the statement about to run, which carries
  the yellow arrow; the status bar says why and where. Locals lists every
  variable of that procedure, strings in quotes and arrays as their item
  count. Watch lists the watched names: Shift+F9 adds the name at the caret
  (or the selection), Delete in the Watch pane or Run > Delete Watch removes
  one. A watched name is looked up as the procedure sees it: its own
  variables, then its module's, then the project's `Public` ones, else it
  shows `<not in scope>`.
- **A runtime error nothing catches** stops the program at the statement
  that raised it, with its number and description in the status bar and the
  variables as they were; Continue then ends the program.

### The channel

The runner is the one that stops. `sxbasic --debug` gets two pipes more:
on its handle 3 it reads what the IDE says, on handle 4 it writes what it
reports, one line each, fields separated by tabs:

| From the IDE | Meaning |
|---|---|
| `break <line> <file>`, `clear <line> <file>` | set or clear a breakpoint, any time |
| `watch <name>`, `unwatch <name>` | add or remove a watched name, any time; at a stop the report comes again |
| `pause` | stop before the next statement |
| `go`, `step`, `over`, `out` | start (the first command after the settings), or go on from a stop |

| From the runner, at a stop | Meaning |
|---|---|
| `stop <reason> <file> <line> <procedure>` | breakpoint, step, pause or error |
| `error <number> <description>` | for an error stop |
| `local <name> <value>`, `watch <name> <value>` | one per variable, one per watched name |
| `end` | the report is whole; the runner waits for a command |

The VM does the stopping (`sxb_vm_set_debug` and the rest in
`system/sxbasic/core/sxbasic.h`): the compiler marks the code word where
each statement starts and records every variable's name and slot, and the
interpreter calls the debugger's `stop` before a marked word when a
breakpoint, a step or a pause asks for it. Neither the marks nor the names
are in a made executable's image, so a made program cannot be debugged.

## Files

- **Save** writes the file beside itself, as `<name>.saving`, and renames
  it over the old one with `sxn_rename_path`. A crash while saving leaves
  the old file whole.
- **New Project** takes a path from the Save dialog, say
  `/home/me/MyApp.sbp`. It makes the folder `/home/me/MyApp/`
  (`sxn_make_directory`) and writes `MyApp.sbp` and a `MainForm.sbf` with
  one button and its `Click` handler. It refuses a folder that already holds
  a project of that name.
- **Save Project As** writes every file of the project into the new path's
  folder, making the folders that are missing, and copies the project's
  `Icon` PNGs there. The project file takes the new name. The environment
  goes on from the new copy.

A command that would drop unsaved changes (New Project, Open Project, Exit,
and closing the window) does not run the first time. It says so in the
status bar, and runs if it is chosen again right away. The toolkit has no
Yes/No/Cancel box of its own yet.

## The selftest

`sxbide --selftest <script>` builds the same window in a surface of its
own, with no window from windowd. It then runs a script of commands against
the same code the menus call. Start and Make SXE wait for the child.

| Command | Does or checks |
|---|---|
| `open <path>`, `save`, `save-as <path>` | Open Project, Save All, Save Project As |
| `select <name>`, `goto <line>`, `insert <text>` (`\|` is a line end), `undo` | the explorer and the editor |
| `set-property <key> <value>` | a value kept in the grid |
| `run`, `make` | Start or Make SXE, to the child's end |
| `run-script <script>` | Start with the runner's own `--selftest <script>`, for a Windows program |
| `new <path>` | New Project |
| `view code\|object`, `tool <kind>`, `key Delete\|Escape` | the views, the toolbox, the designer's keys |
| `drag <x0> <y0> <x1> <y1>`, `click <x> <y>`, `dblclick <x> <y>` | pointer events in the designer, at points of the form's client area |
| `expect-view code\|object`, `expect-selected <name>` | the view shown and the designer's selection |
| `expect-control <name> <x> <y> <w> <h>`, `expect-no-control <name>` | a control's rectangle as the text has it, or its absence |
| `expect-files <names...>`, `expect-current <name>` | the explorer |
| `expect-property <key> <value>`, `expect-line <n> <text>` | the grid and the editor |
| `expect-exit <code>`, `expect-output-file <path>` | the child's exit code and everything it printed |
| `expect-error <file> <line>` | the error's file shown, the caret and the mark on its line |
| `expect-exists <path>` | a file that was written |
| `breakpoint <file> <line>` | toggles a breakpoint |
| `start`, `continue`, `step-into`, `step-over`, `step-out` | the debugger's commands, waiting for the next stop or the end |
| `add-watch <name>` | a watched name |
| `expect-stop <file> <line>`, `expect-stop-error <number>` | stopped there, shown and marked; stopped at that runtime error |
| `expect-local <name> <value>`, `expect-watch <name> <value>` | a row of the Locals or the Watch pane |

`system/sxbasic/selftests/ide.txt` is the scenario `sxbide-smoke` runs. It
opens the console sample, saves it as `/tmp/idetest/hello/`, and changes
its `Version` through the grid. It then runs the sample against its
expected output and breaks a line to find the compile error there. Last, it
undoes the break and makes `Hello.sxe`.
`system/sxbasic/selftests/ide-design.txt`, run by `sxbide-design-smoke`,
is the designer's: a new project, its form made bigger, a label and a
button drawn, the button's `Text` set in the grid, the button moved and
resized, a check box drawn and deleted, and a double-click writing the
button's `Click` handler. It then runs the form with
`ide-design-run.txt`, which clicks the button and checks what the handler
wrote. `system/sxbasic/selftests/ide-debug.txt`, run by
`sxbide-debug-smoke`, is the debugger's. It stops the console sample at a
breakpoint and reads two variables and a watch. It steps over two
statements, into `FizzBuzz` (where the watch is out of scope) and out of it,
and runs to the end against the expected output. Then it breaks a line
with a division by zero nothing catches, which stops the program there
before it ends. A failed check also goes to the system log, which reaches
the serial log the scenario reads. `tools/shoot.sh --scenario sxbide`
shows the window on the greeter sample and starts it; `--scenario
sxbide-debug` steps into the console sample and checks the Locals pane.

## Limits of this version

- **Project size.** At most 16 files per project, and 8 icons copied by
  Save Project As.
- **The property grid.**
  - It lists only the properties written in the file.
  - It has no scrollbar and no drop-down
    ([known gaps](../desktop/PLATFORM_GRAPHICS.md#known-gaps-in-sxgui-c)).
- **No file management.** No Add File, Remove File or Rename from the
  explorer: edit the project file's `Form =` and `Module =` lines, then open
  the project again.
- **Console programs.**
  - A console program's input comes from the environment's own standard
    input, which is empty when the desktop started it.
  - Its output reaches the Output pane when it prints a line end.
- **Output pane.** It keeps the last 256 lines.
- **The designer.**
  - Menus are edited as text; the designer does not show the form's menu
    bar.
  - One control is selected at a time; there is no copy and paste of
    controls, and no moving them with the arrow keys.
  - The toolbox names its controls instead of drawing them.
  - A `GroupBox` does not carry the controls drawn on it when it moves: it
    does not contain them ([SXBASIC.md](SXBASIC.md)).
- **The debugger.**
  - Breakpoints do not follow the lines they are on when lines are added or
    removed above them.
  - A watch is a variable's name, not an expression, and a control's
    property cannot be watched.
  - Only the procedure the program stopped in shows its variables; there
    is no call stack to choose another.
  - While stopped, a Windows program's window does not repaint.
  - There is no Set Next Statement, and no editing and going on: code
    changed while stopped runs at the next Start.
- **Names keep the case they were typed in.** The editor does not yet
  rewrite a name to the case of its declaration, as the
  [reference](SXBASIC.md#lexical-rules) says the environment does.
