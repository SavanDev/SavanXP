# SxBASIC made executables and the program image

> Phase 3 of [SXBASIC_ROADMAP.md](../roadmaps/done/SXBASIC_ROADMAP.md): Make
> SXE. The language is in [SXBASIC.md](SXBASIC.md); the sections every `.sxe`
> carries are in [SXE_FORMAT.md](SXE_FORMAT.md).

A made executable is a copy of the stub, `/system/sxbasic.sxe`, with three
sections added after it is linked:

```
Converter.sxe
├─ the stub as built           → maps libsxbasic.so, libsxgui, libgfx2d, libsximg
├─ .sxbasic  (NOT alloc)       → the compiled project: the program image below
├─ .sxmeta   (NOT alloc)       → NAME, DESCRIPTION, VERSION, VENDOR, COPYRIGHT, ACCENT, CATEGORY
└─ .sxicon   (NOT alloc)       → 16x16 and 32x32, when the project names an Icon
```

Making one is copying and adding sections, not linking:

```
sxbasic --make /path/to/project.sbp /path/to/Program.sxe
```

## The pieces

- **libsxbasic.so** carries the whole of SxBASIC that makes no system call:
  the compiler, the virtual machine, the program image, the writer of these
  sections and the forms runtime over SXGUI. It asks the program for its C
  library (`snprintf`, `strtod`, the math functions), which lld exports from
  the program that links it, as it does for `libsximg`.
- **sxbasic.sxe** is the runner and the stub at once. At start it maps its own
  file, from `argv[0]`, and looks for `.sxbasic`. Found, it is a made
  executable: it loads the image, reads `ACCENT` from its `.sxmeta` and the
  16x16 icon from its `.sxicon` for its window, and runs the program with its
  arguments. Not found, it is the runner (`sxbasic <project.sbp>`).
- **The writer** (`system/sxbasic/core/sxb_exe.c`) is the C counterpart of
  the build's `llvm-objcopy --add-section` stamping. It keeps the stub's bytes
  as they are, then appends each section, a copy of the section name table
  with the new names, and a new section header table. A section the stub
  already has by that name (the build stamps `.sxmeta` on every program) is
  pointed at the new bytes instead of added twice, and the old bytes stay
  behind unused. The sections are written without `SHF_ALLOC`, and the writer
  reads the result back and refuses it if one is not, the same check
  `gen_sxe_resources.py` makes. Magics, tags and sizes come from
  `include/sxe/sxe_format.h`, as the Python generator takes them.
- **The file** is created with `SXN_CREATE_EXECUTABLE` (0700): SUN creates
  files 0600, and the kernel runs only what is marked runnable
  ([SUN.md](SUN.md)).

## The program image

The `.sxbasic` section holds `sxb_program_save`'s output, read back by
`sxb_program_load`. Numbers are little-endian and written byte by byte;
strings are a `uint32` length and their bytes.

| Part | Contents |
|---|---|
| header | `SXBI`, version `1`, then the number of properties and of events the controls were written with |
| program | name, Windows or console, the entry procedure, the number of globals, the startup form |
| code | the bytecode words, then each word's source line and module |
| constants | type, integer, double bits, and for a string its bytes |
| procedures | name, module, parameters, locals, function or not, entry |
| modules | file name, first global and count (first is -1 for none) |
| forms | name, the module holding its code, the form's own control, then each control: name, kind, menu, text, every property value and every event's handler |

The version moves on any change to that layout, and a reader built with
other property or event tables refuses the image too: an executable made by
another SxBASIC says so instead of misreading its forms. The loader checks
every length against what is left and every index the VM and the forms runtime
look things up by (procedures, modules, globals, controls, handlers). The
bytecode itself is trusted, as an executable's own `.text` is: the made file
is a program, and running it is trusting it.

The project's `Description`, `Vendor`, `Version` and the rest are not in the
image; they go into `.sxmeta`, where the launcher and the window manager read
them.

## Icons

`Icon` names PNGs relative to the project, as `icon_file` does in a `.sxres`.
A PNG drawn at 16x16 or 32x32 is used for that size; a size with none is
scaled from the first PNG when it is square and a whole multiple or fraction
of it, pixel by pixel. Unlike the build's generator there is no smooth
scaling for other sizes: such an icon is refused with a message.

## Limits of this version

- The stub finds its own file through `argv[0]`, as POSIX programs do for
  their `.sxmeta`. The desktop and `sessiond` pass the full path; a shell that
  passes the name as typed leaves a made program unable to find itself, and it
  then behaves as the plain runner.
- The stub is copied with its debug sections, so a made executable is the
  size of `sxbasic.sxe` on disk (about 640 KiB, of which 75 KiB is code) plus
  its program.

## Checked by

- `sxbasic-test` (host): every language case runs through a saved and
  reloaded image; the converter's image reloads to the same bytes; images cut
  short or with a header byte changed are refused, under ASan; the sections
  added to the test's own executable read back, replace rather than repeat on
  a second make, and the result still runs.
- `sxbasic-make-smoke`: `sxbasic --make-selftest` makes
  `/tmp/Converter.sxe` inside SavanXP, starts it as a child with its selftest
  script, which checks the accent and icon pixels the stub read from its own
  file and then the converter's behaviour, and passes on how it ended.
- `nativetest`: `SXN_CREATE_EXECUTABLE` creates a 0700 file, and a bare flag
  with no disposition is refused.
