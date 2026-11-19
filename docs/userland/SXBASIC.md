# SxBASIC — the Savan BASIC language

> **Status: console programs run; forms do not yet.** Phase 1 of
> [SXBASIC_ROADMAP.md](../roadmaps/open/SXBASIC_ROADMAP.md) runs a console
> project with `/system/sxbasic.sxe /path/to/project.sbp [arguments]`; a
> project with forms is refused until phase 2. This is phase 0's text: the language, its
> project files and its forms, written before the compiler so the compiler has
> something to be checked against. Where an implementation phase finds a rule
> here that cannot work, the rule changes here first. Three sample projects
> written by hand against it live in
> [`system/sxbasic/samples/`](../../system/sxbasic/samples/).

SxBASIC is the name in code, files and documents; **Savan BASIC** is the name
a user reads. The language borrows the shape of Visual Basic (`Dim`, `Sub`,
`If ... End If`, forms with events named `Control_Event`), but it is its own
dialect: where VB had a trap (implicit `Variant`, `ByRef` by default, `On
Error GoTo`) this dialect picks the safer rule, and VB source does not compile
unchanged.

## A first look

```vb
Private Sub GreetButton_Click()
    Dim name As String = Trim(NameBox.Text)
    If name = "" Then
        MsgBox "Type a name first.", mbWarning, "Greeter"
        Exit Sub
    End If
    ResultLabel.Text = "Hello, " & name & "!"
End Sub
```

The user draws a form with a text box `NameBox`, a button `GreetButton` and a
label `ResultLabel`; double-clicking the button opens this procedure, and it
runs every time the button is clicked.

## Projects and files

A program is a **project**: one project file naming the forms and modules
that make it up. Every file is text, so projects diff and merge.

| File | Extension | Holds |
|---|---|---|
| Project | `.sbp` | the program's name, kind, start, resources, and its list of files |
| Form | `.sbf` | a window: its controls and their properties, then the form's code |
| Module | `.bas` | code shared by the whole project |

`.bas` is the extension every BASIC has used for source, so any editor
outside SavanXP already colours it. `.sbp` and `.sbf` are the project's own.
The `.sx` prefix stays with the system's formats (`.sxe`, `.sxres`, `.sxlnk`).
A made executable is a `.sxe`, as every SUN program is.

All three are UTF-8, with LF line ends; CR LF is read too and written back as
LF. File names inside a project are relative to the project file.

### The project file

```text
SxBASIC Project 1

Name = "Converter"
Description = "Two-form sample of the Savan BASIC reference"
Version = "1.0"
Kind = Windows
Startup = MainForm
Form = "MainForm.sbf"
Form = "AddForm.sbf"
Module = "Conversions.bas"
```

The first line names the format and its version; a reader refuses a version
it does not know. Then one `Key = value` per line, in any order, with blank
lines and `'` comments allowed. Values are written as the language writes
them: strings in quotes, numbers as numbers, and the bare words of `Kind` and
`Startup`.

| Key | Value | Notes |
|---|---|---|
| `Name` | string, required | The program's name: the `.sxe` file name and the `NAME` the launcher shows. Letters, digits, spaces, `-` and `_` |
| `Kind` | `Console` or `Windows`, required | A console program runs in the Command Prompt and has no forms; a Windows program has forms and no console |
| `Startup` | `Main` or a form name, required | `Main` runs `Sub Main` of a module; a form name loads that form and shows it |
| `Form` | string, repeated | A `.sbf` file. A `Console` project has none |
| `Module` | string, repeated | A `.bas` file |
| `Description`, `Vendor`, `Copyright` | string | The `.sxmeta` tags of the same name ([SXE_FORMAT.md](SXE_FORMAT.md#v1-tags)) |
| `Version` | string | `1.2.3`, up to four numbers; becomes `VERSION` and `VERSION_STRING` |
| `Accent` | integer | `&HRRGGBB`, the caption accent (`ACCENT`) |
| `Category` | string | Asks for a place in All Programs (`CATEGORY`) |
| `Icon` | string, repeated | A PNG, one per size, as `icon_file` in a `.sxres`; the same rules pick and scale them |

An unknown key is an error, as it is in a `.sxres`: a typo has to fail, not
leave the program without its icon.

### The form file

```text
SxBASIC Form 1

Begin Form AddForm
    Text = "Add Reading"
    Width = 264
    Height = 110
    Begin TextBox CelsiusBox
        Left = 72
        Top = 8
        Width = 184
        Height = 24
    End
    Begin Button OkButton
        Text = "OK"
        Left = 80
        Top = 76
        Width = 84
        Height = 26
        Default = True
    End
End

Public Accepted As Boolean

Private Sub OkButton_Click()
    ...
End Sub
```

After the version line comes one `Begin Form <name>` block: the form's
properties, then one `Begin <ControlType> <name>` block per control, each
closed by `End`. Menus nest one level of `MenuItem` blocks inside a `Menu`
block. Property values are literals only. A property left out takes its
default. The order of the control blocks is the Tab order.

Everything after the form block is the form's code, as in a module. The
designer rewrites the block and leaves the code as it was.

The form's name is the one in `Begin Form`, not the file name. Controls are
named with identifiers, unique within the form, and a control's name cannot
also be a variable or a procedure of that form.

### The module file

Code only, with no header. The module's name is the file name without
`.bas`, and has to be an identifier.

## Lexical rules

- **Case does not matter.** `dim`, `Dim` and `DIM` are the same word, and so
  are `total` and `Total`. The environment rewrites every use of a name to the
  case of its declaration, as Visual Basic did.
- **One statement per line.** A line ending in a space and `_` continues on
  the next. There is no `:` separator.
- **Comments** start with `'` or with `Rem` as the first word of a statement,
  and run to the end of the line.
- **Identifiers**: a letter, then letters, digits and `_`, at most 63
  characters, ASCII only.
- **Literals**: integers (`42`, `&HFF`), doubles (`3.5`, `.5`, `1e-3`),
  strings in double quotes with `""` for a quote inside (`"say ""hi"""`),
  `True` and `False`.

Reserved words, which cannot name anything:

```text
And As Boolean ByRef ByVal Case Catch Const Dim Do Double Each Else ElseIf
End Exit False For Function If In Integer Is Loop Me Mod Next Not Or Preserve
Print Private Public ReDim Rem Return Select Step String Sub Then To True Try
Unload Until While Xor
```

## Types and values

| Type | Holds | Default |
|---|---|---|
| `Integer` | a signed 64-bit integer | `0` |
| `Double` | an IEEE 754 double | `0` |
| `String` | text, UTF-8 | `""` |
| `Boolean` | `True` or `False` | `False` |
| `T()` | an array of any of the above | empty, `UBound` is `-1` |

There is no `Variant`: every variable, parameter and function has a type,
written with `As`. There is one integer type, not VB's `Integer`, `Long` and
`Byte`; an integer result outside 64 bits is runtime error 6.

**Strings** are values: assigning one copies it, and nothing changes a
string in place. Length and positions count characters (code points), not
bytes, so `Len("Año")` is 3. Comparison is by code point, which is also the
order of the UTF-8 bytes. `UCase` and `LCase` change ASCII letters only.

**Conversions** that cannot lose anything happen on their own: an `Integer`
where a `Double` is expected, and anything joined with `&`. Everything else
is written out:

| From → to | How | Rule |
|---|---|---|
| `Double` → `Integer` | `CInt(x)` | Round to nearest, halves away from zero |
| | `Int(x)`, `Fix(x)` | Down, and toward zero; both return `Double`, so `CInt(Int(x))` |
| `String` → number | `CInt(s)`, `CDbl(s)` | Runtime error 13 if `s` is not a number; `IsNumeric(s)` asks first |
| anything → `String` | `CStr(x)`, `&` | See below |
| `String` → `Boolean` | `CBool(s)` | `"True"` or `"False"`, any case; error 13 otherwise |

An `If` or a loop condition has to be a `Boolean`: `If n Then` is a compile
error, `If n <> 0 Then` is how it is said.

**Numbers as text** are always written with `.` as the decimal point,
whatever the language of the system. An `Integer` prints its digits. A
`Double` prints at most 15 significant digits, without trailing zeros and
with no `.0` when it is whole: `CStr(3.5)` is `"3.5"`, `CStr(4.0)` is `"4"`,
`CStr(1 / 3)` is `"0.333333333333333"`. Very large and very small values use
an exponent: `"1E+20"`.

**Arrays** have one dimension and start at 0. `Dim a(9) As Integer` has ten
elements, `a(0)` to `a(9)`, as in VB. `Dim a() As String` starts empty;
`ReDim a(n)` gives it `n + 1` fresh elements, `ReDim Preserve a(n)` keeps the
ones that fit. Assigning an array copies it. An index outside the array is
runtime error 9.

## Declarations and scope

```vb
Dim count As Integer
Dim name As String = "nobody"
Dim width As Integer, height As Integer
Dim readings(99) As Double
Const Limit = 15
```

Every name in a `Dim` takes its own `As`; `Dim a, b As Integer` is an error,
not VB's silent `Variant`. An initializer is optional. A `Const` takes a
literal or an expression of other constants, and its type is the literal's.

| Declared | Visible | Lives |
|---|---|---|
| In a procedure | the rest of the procedure | one call; fresh on every call |
| At the top of a module, `Dim` or `Private` | that module | the whole run |
| At the top of a module, `Public` | the whole project | the whole run |
| At the top of a form, `Dim` or `Private` | that form | while the form is loaded |
| At the top of a form, `Public` | the project, as `FormName.name` | while the form is loaded |

Procedures follow the same rule, with one difference: a procedure in a
module is `Public` unless it says `Private`, and a procedure in a form is
`Private` unless it says `Public`. A form shares code through its `Public`
members or, better, through a module.

Two modules cannot both declare the same `Public` name. A local name can
hide a module's; the module's is then reached as `ModuleName.name`.

## Expressions

From tightest to loosest:

| Operators | Meaning |
|---|---|
| `( )`, `.`, calls, indexes | grouping, members |
| `^` | power, always `Double` |
| unary `-` | negation |
| `*`, `/` | multiply; `/` always gives a `Double` |
| `\`, `Mod` | integer division and remainder; `Integer` only, the remainder takes the sign of the dividend |
| `+`, `-` | add, subtract; numbers only |
| `&` | join as text |
| `=`, `<>`, `<`, `>`, `<=`, `>=` | compare, giving a `Boolean` |
| `Not` | negation, `Boolean` only |
| `And` | `Boolean` only; the right side is not evaluated when the left is `False` |
| `Or`, `Xor` | `Boolean` only; `Or` skips the right side when the left is `True` |

`+` does not join strings: `"a" + "b"` is a compile error that points at
`&`. An operation on an `Integer` and a `Double` works on doubles. Division
by zero, `/` included, is runtime error 11, not an infinity.

`=` compares inside an expression and assigns as a statement, as in every
BASIC.

## Statements

```vb
total = total + 1                      ' assignment
ReadingsList.AddItem "21.5 C"          ' a call: arguments with or without
ReadingsList.AddItem("21.5 C")         '   parentheses
```

A function can be called as a statement, and its result is dropped.

**If**

```vb
If score >= 9 Then
    grade = "A"
ElseIf score >= 7 Then
    grade = "B"
Else
    grade = "F"
End If

If done Then Exit Sub                  ' one statement, no Else
```

**Select Case** works on `Integer`, `Double` and `String`. The first `Case`
that matches runs, and there is no fall-through:

```vb
Select Case score
    Case 9 To 10
        grade = "A"
    Case 7, 8
        grade = "B"
    Case Is >= 5
        grade = "C"
    Case Else
        grade = "F"
End Select
```

**For**, with the variable declared beforehand. The bounds and step are
evaluated once; a step of 0 is runtime error 5. `Next` may repeat the
variable's name.

```vb
For i = 1 To 10 Step 2
    ...
Next i

For Each word In words                 ' over an array, in order
    ...
Next
```

`For Each` too wants its variable declared, with the array's element type.

**Do**, the one loop besides `For`:

```vb
Do While count < 10        Do Until done        Do
    ...                        ...                  ...
Loop                       Loop                 Loop Until done
```

`Exit For`, `Exit Do`, `Exit Sub` and `Exit Function` leave the innermost
loop of their kind or the procedure.

**Print** writes any value to standard output, as `CStr` would write it:
`Print a & b` ends the line, `Print a;` does not. A console program's output goes to its console; a Windows
program's goes to the environment's output pane when it runs from the
environment, and nowhere otherwise.

## Procedures

```vb
Sub Swap(ByRef a As Integer, ByRef b As Integer)
    Dim t As Integer = a
    a = b
    b = t
End Sub

Function Average(values() As Double) As Double
    Dim total As Double
    Dim v As Double
    For Each v In values
        total = total + v
    Next
    Return total / (UBound(values) + 1)
End Function
```

- Parameters are `ByVal` unless they say `ByRef`: the reverse of VB6, whose
  default changed caller variables nobody meant to change.
- Arrays are always passed by reference; `ByVal` on an array is an error.
- An argument for a `ByRef` parameter or an array has to be a variable of
  exactly the parameter's type, not an element or an expression.
- `Return value` leaves a `Function` with its result. A function that reaches
  `End Function` returns its type's default, as does `Exit Function`.
  `Return` without a value leaves a `Sub`.
- A function returns an array with `As T()`.
- Procedures may call themselves.

There are no optional parameters and no overloads in this version.

## Errors

**Compile errors** name the file, the line and the column, and say what was
expected: `MainForm.sbf:42:17: "+" does not join strings; use "&"`. A
project with a compile error does not run.

**Runtime errors** carry a number and a description. Not caught, one ends the
program: a Windows program shows a message box naming the error and the file
and line it happened on, a console program prints the same line, and the
environment opens the editor at that line.

```vb
Try
    text = ReadAllText(path)
Catch
    MsgBox "Could not read " & path & ": " & Err.Description, mbError
End Try
```

An error inside `Try` jumps to `Catch`, where `Err.Number` and
`Err.Description` say what happened. One not caught in a procedure goes on to
its caller's `Try`, and so on up. `Err.Raise number, description` raises one,
for a program's own errors (numbers from 1000) or to pass a caught one on.

There is no `On Error`: no `GoTo`, no `Resume Next`.

| Number | Description | Raised by |
|---|---|---|
| 5 | Invalid argument | a value a procedure does not accept: `Mid(s, 0)`, a `For` step of 0 |
| 6 | Overflow | an integer outside 64 bits, `CInt` of a double out of range |
| 7 | Out of memory | |
| 9 | Index out of range | an array index, `List(i)` of a list box |
| 11 | Division by zero | `/`, `\`, `Mod` |
| 13 | Type mismatch | `CInt`, `CDbl`, `CBool` of text that is not one |
| 28 | Out of stack space | recursion about 4000 calls deep |
| 53 | File not found | the file procedures |
| 57 | Input/output error | the file procedures |
| 70 | Permission denied | the file procedures |
| 75 | Path not valid | a path that is not absolute, or names a directory |
| 380 | Invalid property value | a control property out of its range |

The numbers are the ones BASIC programmers already know where one exists.

## Starting and ending

- `Startup = Main` runs `Sub Main()` of the one module that has it. A console
  program always starts this way.
- `Startup = FormName` loads that form and shows it.
- A console program ends when `Sub Main` returns. A Windows program ends
  when its last form unloads.
- `End` ends the program at once, without running any `Form_Unload`.

## Forms

A form is a window and its controls. Each form exists once in a running
program and is reached by its name from anywhere: `AddForm.ShowModal`,
`MainForm.ReadingsList.ListCount`. `Me` is the form whose code is running.

A form **loads** the first time something uses it: `Show`, `ShowModal`, or
reading or writing one of its controls or `Public` members. Loading creates
its controls with the properties of its file, then runs `Form_Load`. `Unload
FormName` closes its window, runs `Form_Unload`, and drops its controls and
variables; the next use loads it again, fresh.

| Member | Kind | |
|---|---|---|
| `Text` | property | the title bar |
| `Width`, `Height` | property | the client area, below the menu bar if there is one |
| `Resizable` | property, file only | whether the user can resize the window; `False` by default |
| `Show` | method | shows the form and returns at once |
| `ShowModal` | method | shows the form as a dialog of the form that called it, and returns when it is hidden or unloaded |
| `Hide` | method | hides it, still loaded |
| `Form_Load` | event | after loading |
| `Form_Unload(ByRef Cancel As Boolean)` | event | before unloading; `Cancel = True` keeps it open |
| `Form_Resize` | event | after the user resizes it |

The first form shown is the program's main window; every other form is a
window owned by it. At most four of those can be open at once
(`SAVANXP_WM_MAX_OWNED_WINDOWS`, see
[SYSTEM_LIMITS_ROADMAP.md](../roadmaps/open/SYSTEM_LIMITS_ROADMAP.md)).

### Events

An event runs the procedure named `ControlName_EventName` (or
`Form_EventName`) in the form's code, if there is one. A procedure whose name
starts with a control's name and `_` has to name an event that control has,
so a misspelt handler is a compile error instead of a button that silently
does nothing.

Events run one at a time. A long loop inside one freezes the window until it
returns, unless it calls `DoEvents`, which lets the window repaint and handle
input. Events can then run inside the loop, so the loop has to expect that.

### Controls

Every control has `Left`, `Top`, `Width`, `Height` (pixels, from the top left
of the form's client area), `Visible` and `Enabled` (`True` by default), and
the method `SetFocus`. Colours and fonts are the system's; there are no
`ForeColor`, `BackColor` or `Font` properties.

| Control | Properties | Methods | Events | SXGUI widget |
|---|---|---|---|---|
| `Label` | `Text` | | | `SXGUI_LABEL` |
| `Button` | `Text`; `Default`, `Cancel` (file only) | | `Click` | `SXGUI_BUTTON` |
| `CheckBox` | `Text`, `Value` (Boolean) | | `Click` | `SXGUI_CHECKBOX` |
| `RadioButton` | `Text`, `Value` (Boolean), `Group` (file only) | | `Click` | `SXGUI_RADIO` |
| `TextBox` | `Text`; `MultiLine` (file only) | | `Change` | `SXGUI_TEXTFIELD`, or `SXGUI_TEXTEDIT` when `MultiLine` |
| `ListBox` | `ListCount`, `ListIndex`, `List(i)` | `AddItem`, `RemoveItem`, `Clear` | `Click`, `DblClick` | `SXGUI_LISTBOX` |
| `ComboBox` | `ListCount`, `ListIndex`, `List(i)`, `Text` (read only) | `AddItem`, `RemoveItem`, `Clear` | `Click` | `SXGUI_COMBOBOX` |
| `GroupBox` | `Text` | | | `SXGUI_GROUPBOX` |
| `ScrollBar` | `Min`, `Max`, `Value`, `LargeChange`; `Horizontal` (file only) | | `Change` | `SXGUI_SCROLLBAR` |
| `TrackBar` | `Min`, `Max`, `Value` | | `Change` | `SXGUI_TRACKBAR` |
| `ProgressBar` | `Min`, `Max`, `Value` | | | `SXGUI_PROGRESS` |
| `PictureBox` | `Picture` (an absolute path) | | `Click` | none yet: phase 2 adds it |
| `Timer` | `Interval` (milliseconds), `Enabled` | | `Tick` | none: `on_tick` of the application frame |

Notes on the table:

- "File only" properties are set in the form file and fixed at run time.
- `Default` and `Cancel` mark the buttons Enter and Esc press, in a form
  shown with `ShowModal`.
- A `GroupBox` only draws a frame; it does not contain the controls inside
  it, unlike VB's `Frame`. Radio buttons exclude each other by `Group`, a
  number, not by the frame they sit in.
- `ComboBox` is a drop-down list to pick from; it does not take typed text.
- `ListIndex` is the selected row, `-1` for none; setting it selects.
  `RemoveItem i` and `List(i)` with `i` outside the list are error 9.
- A `Timer` is invisible. Its `Tick` runs no faster than the application
  frame's tick; phase 2 states the resolution.
- `Value` of a scroll bar, track bar or progress bar outside `Min`..`Max` is
  error 380.

### Menus

```text
Begin Menu FileMenu
    Text = "File"
    Begin MenuItem FileExit
        Text = "Exit"
    End
End
```

A form has at most one menu bar: its `Menu` blocks, in order. Each holds
`MenuItem` blocks, one level deep, as SXGUI's menus are. An item has `Text`
(`"-"` makes a separator), `Enabled` and `Checked`, and its `Click` event runs
`ItemName_Click`. Menus and items are named like controls and live in the
same names.

## Built-in procedures

| Group | Procedures |
|---|---|
| Text | `Len(s)`, `Left(s, n)`, `Right(s, n)`, `Mid(s, start[, length])`, `InStr(s, find[, start])` (0 when not found), `UCase(s)`, `LCase(s)`, `Trim(s)`, `LTrim(s)`, `RTrim(s)`, `Replace(s, find, with)`, `Split(s, separator)`, `Join(array, separator)`, `Space(n)`, `Chr(code)`, `Asc(s)` |
| Conversion | `CInt(x)`, `CDbl(x)`, `CStr(x)`, `CBool(x)`, `IsNumeric(s)` |
| Math | `Abs(x)`, `Sgn(x)`, `Sqr(x)`, `Int(x)`, `Fix(x)`, `Round(x[, digits])` (halves away from zero), `Sin`, `Cos`, `Tan`, `Atn`, `Exp`, `Log`, `Rnd()` (from 0 up to but not including 1), `Randomize [seed]` |
| Arrays | `UBound(array)` |
| Console | `Print`, `ReadLine()` (a line of standard input, without its end) |
| Dialogs | `MsgBox(text[, style[, title]])`, `InputBox(prompt[, title[, default]])` (`""` when cancelled); Windows programs only, a compile error in a console program |
| Files | `ReadAllText(path)`, `WriteAllText(path, text)`, `FileExists(path)`, `AppPath()` (the program's directory) |
| Program | `Command()` (the arguments, as one string), `Seconds()` (since the program started, a `Double`), `DoEvents`, `End` |

Positions in text start at 1. Paths are absolute, as SUN's are;
`AppPath() & "/notes.txt"` builds one next to the program.

**Constants**: `NewLine` (a line end) and `Tab`; for `MsgBox`, the buttons
`mbOk` (the default), `mbOkCancel`, `mbYesNo`, `mbYesNoCancel`, the icons
`mbInformation`, `mbWarning`, `mbError`, `mbQuestion`, added to the buttons
(`mbYesNo + mbQuestion`), and the results `mbResultOk`, `mbResultCancel`,
`mbResultYes`, `mbResultNo`.

## Not in this version

Left out on purpose, each a decision to revisit with a program that needs it:

- `Variant`, `Object`, user-defined types (`Type ... End Type`), classes.
- `GoTo`, `GoSub`, labels, `On Error`, `While ... Wend`, `Call`, the `:`
  separator: one way to do each thing.
- Arrays of more than one dimension, optional parameters, overloads.
- Bit operators on integers.
- Dates and times beyond `Seconds()`, until local time exists
  ([TIMEZONE_ROADMAP.md](../roadmaps/open/TIMEZONE_ROADMAP.md)).
- Keyboard and mouse events on controls and forms (`KeyPress`, `MouseMove`),
  control arrays, more than one copy of a form, submenus, tab controls.
- Fonts and colours per control.

## Notes for the implementation

- **The virtual machine must be able to suspend a running procedure and
  resume it later.** `ShowModal` returns to its caller only when the dialog
  hides, while the application frame keeps running events; `DoEvents` runs
  events in the middle of a procedure; the debugger stops at a breakpoint.
  All three need the VM's call stack to be data the VM owns, not the C stack.
- The bytecode is not a format anyone else reads. What is stable is the
  source; the section a made executable carries (phase 3) is versioned with
  the runtime that reads it.
- Names are case-insensitive, so the compiler compares them folded to one
  case and the environment keeps the declared spelling for display.

## Samples

Each one is a whole project, written before there was anything to run it:

| Project | Shows |
|---|---|
| [`hello`](../../system/sxbasic/samples/hello/) | a console program: loops, functions, `Select Case`, text, arrays, a caught error; `expected-output.txt` is what it prints, and phase 1's smoke scenario compares against it |
| [`greeter`](../../system/sxbasic/samples/greeter/) | one form: a text box, a button, a label, a message box |
| [`converter`](../../system/sxbasic/samples/converter/) | two forms and a module: a menu, a list box, a dialog shown with `ShowModal` that hands back its result through `Public` members |
