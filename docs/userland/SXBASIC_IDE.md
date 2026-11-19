# Savan BASIC, the environment

`/system/sxbide.sxe` is the development environment of SxBASIC
([SXBASIC.md](SXBASIC.md)): open a project, edit its files, run it, find
its errors at their line and make its executable. It is a SUN program drawn
with SXGUI, listed in All Programs under Development as **Savan BASIC**, and
it opens a `.sbp` from Files. This is phase 4 of the
[roadmap](../roadmaps/open/SXBASIC_ROADMAP.md): forms are edited as text.
Drawing them is phase 5, and the debugger is phase 6.

```text
sxbide [project.sbp]
sxbide --selftest <script>
```

## The window

One window, the panes inside it, as Visual Basic's docked layout:

- **Menu bar and toolbar.** File (New Project, Open Project, Save, Save All,
  Save Project As, Exit), Edit (Undo, Redo, Cut, Copy, Paste, Select All),
  Run (Start, End, Make SXE). The toolbar repeats Open, Save All, Start, End
  and Make SXE. F5 starts the program, Ctrl+S saves the file shown, and
  Ctrl+O opens a project.
- **Project.** The project file, then its forms and modules in the order
  the project lists them. A file with unsaved changes shows ` *` after its
  name. Choosing one shows it in the editor.
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
- **Status bar.** The caret's line and column, and the last message.

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

- Start runs `sxbasic <project.sbp>`.
- Make SXE runs `sxbasic --make <project.sbp> <folder>/<Name>.sxe`, next to
  the project and named after its `Name`.

The child's standard output and error go to one pipe, which the
environment reads on its tick without blocking. A program that loops or
crashes therefore does not take the environment with it. A Windows program
opens its own window, since `sxngui_app` gives each process one main window
([roadmap](../roadmaps/open/SXBASIC_ROADMAP.md#run-starts-a-separate-process)).
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
| `expect-files <names...>`, `expect-current <name>` | the explorer |
| `expect-property <key> <value>`, `expect-line <n> <text>` | the grid and the editor |
| `expect-exit <code>`, `expect-output-file <path>` | the child's exit code and everything it printed |
| `expect-error <file> <line>` | the error's file shown, the caret and the mark on its line |
| `expect-exists <path>` | a file that was written |

`system/sxbasic/selftests/ide.txt` is the scenario `sxbide-smoke` runs. It
opens the console sample, saves it as `/tmp/idetest/hello/`, and changes
its `Version` through the grid. It then runs the sample against its
expected output and breaks a line to find the compile error there. Last, it
undoes the break and makes `Hello.sxe`. `tools/shoot.sh --scenario sxbide`
shows the window on the greeter sample and starts it.

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
- **Names keep the case they were typed in.** The editor does not yet
  rewrite a name to the case of its declaration, as the
  [reference](SXBASIC.md#lexical-rules) says the environment does.
