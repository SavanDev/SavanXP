# SxBASIC roadmap

> **Status: open, phases 0 to 5 done.** SxBASIC is a BASIC language and a visual
> development environment for it, in the spirit of Visual Basic: draw a form,
> double-click a button, write what happens on click, press Run, and make an
> executable. It runs on SavanXP only and makes programs for SavanXP only, as
> Visual Basic ran on Windows and made Windows programs. The facts below were
> checked on 2026-11-18 at `b6e022a4`.

## The name

One name for the language and the environment, as Visual Basic had. **SxBASIC**
in code, files and documents (`libsxbasic.so`, `SXBASIC.md`), after SxFS and
SxGL; **Savan BASIC** where a user reads it (the Start menu, the title bar,
the About box). "Visual" stays out of it: next to "BASIC" it is Microsoft's
mark. The files are `.sbp` (project), `.sbf` (form) and `.bas` (module):
`.bas` because every BASIC uses it for source and editors elsewhere already
colour it, and not `.sx*`, which stays with the system's own formats (`.sxe`,
`.sxres`, `.sxlnk`). Bare `.sb` was passed over: Microsoft Small Basic and
Scratch 1.x already use it, and one extension cannot name three kinds of file.

## What it is, and what it is not

It is three things, the same three Visual Basic was:

1. **A language and its runtime**: a BASIC dialect with an event model
   (`Sub OkButton_Click()`), forms and controls as objects with properties.
2. **A visual designer**: a toolbox, a form to draw controls on, a property
   grid, a code editor, a project explorer.
3. **Make SXE**: an executable that runs from the desktop like any other
   program, with its own icon and title.

It is not a clone of Visual Basic. The dialect borrows the shape of VB
(`Dim`, `Sub`, `If ... Then ... End If`, `Form.Show`, events named
`Control_Event`) but compatibility with VB6 source, `.frm` files or the VB
runtime is not a goal, and neither is its look: SxBASIC is drawn with SXGUI.

## Decisions

### It runs on SavanXP only

A version for Windows and Linux would need a third application frame for
SXGUI on the host, three stubs for Make SXE, and three runtimes kept in step.
None of that serves the goal. SxBASIC is a SUN program, and so is every
program it makes (rule 6 of `AGENTS.md`).

### It lives in this repository, not in a port

Ports are third-party code with an upstream pin, patches and an overlay
([ports/README.md](../../../ports/README.md)). SxBASIC is first-party, and
it is going to drive the system it runs on: it is the consumer of the SXGUI
widgets and SUN calls it lacks (see [What is missing](#what-is-missing)), and
rule 6 asks for the consumer and the slice it needs in the same commit, with
its `nativetest` assertion. A separate repository would split every one of
those commits in two and leave the smoke scenarios without the program they
test. The runtime library also ships in the image, because the executables
SxBASIC makes need it on every SavanXP install, as Windows came to carry
`MSVBVM60.DLL`.

The SDK still matters: the core (below) is plain C that also builds for the
host, and nothing stops the IDE from being moved out later, because the SDK
2.0 sysroot carries everything a SUN program links.

### Bytecode and a virtual machine, not native code

There is no C compiler inside SavanXP, so the IDE cannot produce machine
code. The language compiles to bytecode and a virtual machine runs it, which
is what Visual Basic did before version 5 (P-code). Compiling to native code
through `sxcc` on the build host is left out ([Not in this
roadmap](#not-in-this-roadmap)).

### A call-free core, tested on the host

The compiler and the virtual machine make no system call, the rule SXGFX
already follows ([PLATFORM_GRAPHICS.md](../../desktop/PLATFORM_GRAPHICS.md)).
What they need from the system (printing, files, the clock, forms) comes
through a host interface of function pointers, as `struct sxgui_host` gives
the toolkit its clock and clipboard. The core then also builds for Linux, and
the bulk of its tests (parse this, run that, expect this output or this error
on this line) run in `./build.sh test` without booting QEMU.

### The runtime is a shared library, an executable is a stub plus bytecode

- `libsxbasic.so` carries the virtual machine and the forms runtime over
  SXGUI, in `/lib` next to `libsxgui.so`.
- A made executable is a small fixed stub, a SUN PIE program built once with
  the system and needing `libsxbasic.so`, with the compiled project added as
  a non-allocated ELF section, plus the `.sxmeta` and `.sxicon` sections every
  `.sxe` carries ([SXE_FORMAT.md](../../userland/SXE_FORMAT.md)).

Making an executable is therefore copying the stub and adding sections, not
linking. SUN PIE programs with `DT_NEEDED` libraries already exist:
`imgview.sxe` needs `libsxgui.so.0.4`, `libgfx2d.so.0.4` and
`libsximg.so.0.4`.

### Run starts a separate process

Run in the IDE starts the stub as a child process with the project's
bytecode, rather than running it inside the IDE. A program that loops or
crashes does not take the IDE with it, and each form gets the window of its
own process (`sxngui_app` is one main window per process). The debugger
(phase 6) talks to that process over a channel.

## What is missing

Checked against the tree on 2026-11-18.

| Area | Today | SxBASIC needs | Phase |
|---|---|---|---|
| SUN files | open, read, create, append, flush, query a path, list a directory, a 4 KiB whole-file write | `mkdir` for a new project folder, `rename` for a safe save, `unlink`, seek; all listed as missing in [SUN.md](../../userland/SUN.md#not-there-deliberately) and [VFS-A](VFS_ROADMAP.md#vfs-a--common-io-results-and-the-native-file-surface) | 4 |
| Code editor | `SXGUI_TEXTEDIT`: plain text, caller-owned buffer of fixed capacity, no wrap, no undo | a growable buffer, undo, line numbers, colouring by token, a gutter for breakpoints ([below](#the-code-editor-is-sxgui_textedit-not-a-new-widget)) | 4, 6 |
| Property grid | `SXGUI_LISTBOX` with columns | a two-column grid that edits a value in place (text, a list, a colour, true/false) | 4 |
| Designer surface | the app paints after the widgets (`on_paint`) and sees the pointer first (`on_pointer`) | controls drawn but inert, selection handles, drag to move and resize, a snap grid | 5 |
| Controls | label, button, checkbox, radio, listbox, combobox, text field, multi-line text, scrollbar, group box, progress, trackbar, tabs, icon view, menus | a picture control (an image in a rectangle) | 2 |
| Timer | `on_tick` in `sxngui_app` | a Timer control with its own interval | 2 |
| Resources of a made executable | SUN programs compile them in (`<program>.sunres.h`); `.sxmeta` and `.sxicon` are stamped by `llvm-objcopy` on the build host | the stub reads its title, accent and icon from its own `.sxe` at start, as POSIX programs do, and the IDE writes the sections itself, in C | 3 |

Each gap closes in the subsystem that lacks it, with SxBASIC as its
consumer, never with a workaround inside the IDE (rules 6 and 7).

### The code editor is `SXGUI_TEXTEDIT`, not a new widget

There is no `SXGUI_CODEEDIT`. What is hard in an editor (the caret, the
selection, the clipboard, scrolling, and the undo it does not have yet) is
what a code editor and Notepad share; a second widget would either copy it,
and the two copies would drift, or call into the first, and then it is not a
second widget. What a code editor adds is opt-in on `SXGUI_TEXTEDIT`:

- a growable buffer, as an alternative to the caller's fixed one;
- undo and redo;
- a gutter with line numbers, behind a flag;
- colour by span, asked of the program through a callback per visible line,
  so SXGUI never learns the language: SxBASIC colours BASIC, and any other
  program could colour something else;
- marks in the gutter (a breakpoint, the current line) set by the program;
- keeping the indent of the previous line on Enter, behind a flag.

Notepad gets the growable buffer and undo without asking for the rest. Its
fixed 32 KiB `NOTEPAD_DOCUMENT_CAPACITY` and its lack of undo are the same
gaps.

## Work that belongs to other roadmaps

Several gaps are pending phases or slices of other roadmaps that were waiting
for a consumer. SxBASIC is that consumer, so the slice lands with it: the
work, its assertion and an update to the other roadmap's status, in the same
commit. Only the slice SxBASIC uses lands, not the rest of the other phase.

| Roadmap | What SxBASIC takes from it | Phase here |
|---|---|---|
| [VFS_ROADMAP.md](VFS_ROADMAP.md#vfs-a--common-io-results-and-the-native-file-surface), VFS-A | the SUN path operations (mkdir, rename, unlink) that VFS-A keeps "for a New Folder button", and seek. The kernel already has the replacing rename a safe save needs (VFS-D); SUN only lacks the call | 4 |
| [SYSTEM_LIMITS_ROADMAP.md](SYSTEM_LIMITS_ROADMAP.md) | none at first: the IDE is one main window with its panes inside it, as Visual Basic's docked layout, so it fits in the 4 owned windows per process and the 12 application windows of the session. A running program with several forms is the first that could hit the 12, and that raise is planned there | 2, 5 |
| [IMAGE_VIEWER_ROADMAP.md](IMAGE_VIEWER_ROADMAP.md) | the picture control decodes with `libsximg`, for whatever formats it reads by then | 2 |
| [SXE_FORMAT.md](../../userland/SXE_FORMAT.md) | the project section of a made executable, and a writer for `.sxmeta` and `.sxicon` in C next to the Python generator | 3 |

## Phases

### Phase 0 — the language and the files, on paper (done)

A reference document for the dialect (`docs/userland/SXBASIC.md`, listed
in `docs/README.md`): types (integer, double, string, boolean, arrays,
control and form references), statements, `Sub` and `Function`, events, the
built-in procedures, and how a runtime error reports its line. Plus the
project files, all text so they diff and merge: the project (its forms, its
modules, the start form, title and icon), a form (controls, their properties,
then its code), a module.

Done when the document exists and a small program of each kind (console, one
form, two forms with a module) is written in it by hand.

**What landed.** [SXBASIC.md](../../userland/SXBASIC.md) and three projects
under `system/sxbasic/samples/`: `hello` (console, with the output it must
print), `greeter` (one form) and `converter` (two forms, a module, a menu, a
dialog). Decisions the reference makes that change the plan above:

- No `Variant`: every name is typed, one 64-bit `Integer`, `ByVal` by
  default, `Try`/`Catch` instead of `On Error`.
- A form exists once and loads on first use, as VB's default instances.
  Forms after the first are owned windows, so four can be open at once.
- `ShowModal` and `DoEvents` need a virtual machine that can suspend a
  procedure and resume it, which phase 1 builds in from the start; the
  debugger of phase 6 relies on the same thing.
- `GroupBox` does not contain controls; radio buttons group by number.

### Phase 1 — the core: compiler and virtual machine (done)

The call-free core: lexer, parser, compiler to bytecode, the virtual machine,
strings and arrays with their memory managed by the runtime, and runtime
errors that name the file and line. Host tests in CTest for the language,
case by case. A console runner in SavanXP, a SUN program, runs a project
from the command line, with a smoke scenario that runs the console program
from phase 0 and checks its output.

**What landed.** The core in `system/sxbasic/core/` (lexer, parser, a
compiler that types every expression, and a stack VM whose frames and Try
handlers are its own data), `/system/sxbasic.sxe` as the runner,
`sxbasic-test` (72 cases under CTest, the hello sample included) and
`smoke sxbasic-smoke`. Forms are refused with the line that names one.
The runner links statically (868 KiB). The runner's output does not reach the serial
log when init starts it, so the scenario's verdict is its exit status, which
is 0 only when the output matched.

### Phase 2 — forms at run time (done)

The forms runtime over SXGUI: a form file becomes a window of SXGUI widgets
built at start; properties read and written from code (`Label1.Caption =
"Hi"`); events dispatched to `Sub Control_Event`; `Form.Show`, `Unload`,
`MsgBox`, `InputBox`; the Timer control and the picture control. The
virtual machine and this runtime become `libsxbasic.so`, and the runner
becomes the stub. A selftest scenario opens a form, clicks a button through
the selftest path windowd already uses, and checks the text a handler wrote.

**What landed.** The compiler reads `.sbf` files and checks every property,
method and event against its control (`sxb_forms_def.c`); a misspelt handler
is a compile error. The VM invokes event procedures, and one that calls
`ShowModal`, `MsgBox` or `InputBox` waits on the VM's own stack while the
dialog's events run above it, then resumes. `system/sxbasic/forms.c` maps
forms onto SXGUI widgets with no system call; `sxbasic.sxe` puts it in a SUN
window, or with `--selftest` in a surface of its own driven by a script.
Checked by `sxbasic-test` (the VM against a fake forms runtime),
`sxbasic-greeter-smoke` and `sxbasic-converter-smoke`, and
`tools/shoot.sh --scenario sxbasic` in a real window. Departures from the plan:

- `libsxbasic.so` and the runner as the stub move to phase 3, which is what
  needs them.
- The picture control waits for a picture widget in SXGUI.
- The scenarios drive the program without a window instead of through
  windowd's selftest, which samples pixels of a known program; the shoot
  scenario covers the real window.
- A second form shows only with `ShowModal`, because the application frame
  has one main window and one dialog; the rest is in
  [SXBASIC.md](../../userland/SXBASIC.md#limits-of-this-version).

### Phase 3 — Make SXE (done)

The stub as a fixed system program; a writer in C that copies it and adds the
project section, `.sxmeta` and `.sxicon`, keeping them non-allocated (the
same check `gen_sxe_resources.py` makes); the stub reading its presentation
from its own file. A scenario makes an executable from inside SavanXP, starts
it and checks its window title, its icon in the launcher and its behaviour.
The format of the project section goes in `SXE_FORMAT.md`'s tag space or a
document of its own.

**What landed.** `libsxbasic.so` carries the compiler, the VM, the forms
runtime, the program image and the section writer; `sxbasic.sxe` is the
runner and the stub, and `sxbasic --make` writes a made executable, all
described in [SXBASIC_IMAGE.md](../../userland/SXBASIC_IMAGE.md), which is
also the project section's document. The project file's `Description`,
`Vendor`, `Version`, `Accent`, `Category` and `Icon` now reach `.sxmeta` and
`.sxicon`. The slice SUN lacked landed with it: `SXN_CREATE_EXECUTABLE`, since
SUN created every file 0600 and the kernel runs only what is marked runnable.
Checked by `sxbasic-test` and `sxbasic-make-smoke`. Departures from the plan:

- The scenario checks the window's accent and icon pixels as the stub read
  them from its own file, not the launcher's: the launcher lists a program by
  its `CATEGORY` from the directories it scans, and `/tmp` is not one.
- Icons other than 16x16 and 32x32 scale only by whole factors; the smooth
  scaling of the build's generator is not in the C writer.
- The stub finds its own file through `argv[0]`, and is copied with its debug
  sections ([limits](../../userland/SXBASIC_IMAGE.md#limits-of-this-version)).

### Phase 4 — the IDE, code first (done)

The environment itself: a menu bar and toolbar, a project explorer, a code editor,
the property grid, forms edited as text for now, Run (phase 2's stub in a
child process, its errors shown at the line they came from), Make SXE. This
phase consumes the SUN file calls and the SXGUI editor and property grid
from [What is missing](#what-is-missing), each in its own commit with its
assertion. A selftest opens a sample project, runs it and makes its
executable.

**What landed.** Savan BASIC, `/system/sxbide.sxe`
([SXBASIC_IDE.md](../../userland/SXBASIC_IDE.md)), and before it SUN's `sxn_make_directory` and `sxn_rename_path`
(`nativetest`). On `SXGUI_TEXTEDIT`, all six editor options from
[above](#the-code-editor-is-sxgui_textedit-not-a-new-widget):
`sxgui_textedit_document` (a buffer the toolkit owns and grows, with undo
and redo by step), `edit_flags` (`SXGUI_TEXTEDIT_LINE_NUMBERS`,
`SXGUI_TEXTEDIT_AUTO_INDENT`), `edit_colorize` (colour by span, asked per
visible line) and `edit_marks` (breakpoint and current-line marks in the
gutter), checked by `sxgui-textedit-test` on keys and painted pixels. The
property grid, `SXGUI_PROPGRID` (`struct sxgui_property` rows: text, a list,
a colour, true/false), checked by `sxgui-propgrid-test`; its missing
scrollbar and drop-down are in
[PLATFORM_GRAPHICS.md](../../desktop/PLATFORM_GRAPHICS.md#known-gaps-in-sxgui-c).
With the environment came `sxgui_textedit_replace` and
`sxgui_textedit_set_caret`, for the grid writing a value back and for going
to an error's line. Checked by `sxbide-smoke` (the selftest the phase asks
for, on the console sample) and `tools/shoot.sh --scenario sxbide`.
Departures from the plan:

- The property grid reads the `Key = value` lines of the block around the
  caret instead of a model of the form, and lists only what is written.
- Run and Make SXE start the runner, `sxbasic.sxe`; the environment does
  not link `libsxbasic`.
- A command that would lose unsaved changes asks by running only when
  chosen twice; SXGUI has no Yes/No/Cancel box of its own.

### Phase 5 — the visual designer (done)

The form drawn instead of written: a toolbox, controls placed by drawing a
rectangle, selection handles, move and resize, snap to a grid, the property
grid following the selection, double-clicking a control to open its default
event in the code editor. A selftest places a button, sets its caption,
writes a handler and runs the form.

**What landed.** The designer in Savan BASIC
([SXBASIC_IDE.md](../../userland/SXBASIC_IDE.md#the-form-designer)), and
SXGUI's design surface it needed: `sxgui_paint_widgets` (widgets painted
with no context, so inert), `sxgui_paint_grab_handles`, `sxgui_grab_at`,
`sxgui_grab_resize`, `sxgui_paint_snap_grid` and `sxgui_snap`, checked by
`sxgui-design-test`. The phase's selftest is `sxbide-design-smoke`, which
also moves, resizes and deletes; `tools/shoot.sh --scenario sxbide` shows
the designer. Departures from the plan:

- The text stays the model: the designer reads the form's block on every
  paint and writes each change into it as one step of undo, so the code
  view, the property grid and the designer cannot disagree.
- Menus are still edited as text, and the toolbox lists names, not icons.

### Phase 6 — the debugger

Breakpoints from the editor's gutter, stop at a runtime error instead of
ending, step into and over, the current line marked, local variables and a
watch list. The virtual machine pauses and reports over a channel between the
IDE and the child process.

## Not in this roadmap

- Running on Windows or Linux, and making programs for them.
- Native code through `sxcc`.
- Compatibility with Visual Basic source, `.frm`/`.vbp` files or its runtime.
- Controls from outside the system (an OCX equivalent), databases, network
  controls. Each would be a roadmap of its own after phase 5.
