# Shared libraries: where this is going

The goal is a loader that maps `libSxGFX`, `libSxGUI` and `libFFmpeg` once,
instead of the 70 of 82 programs in `/bin` carrying their own copy. The engine
is described in [`SECTIONS.md`](SECTIONS.md); this file records the decisions and
the order, so the work is not reconstructed from commit messages.

This is SDK v2 work. Breaking the existing SDK surface is acceptable and expected.

## What the measurements said

Measured, not estimated:

- `/bin` is 25.5 MB across 82 binaries, 311 KB average. The floor for any C
  program is 427 KB (`false`, `true`, `cat`, `ls`).
- `gfx2d.o` — the SXGFX rasterizer, 2,589 lines — is linked into **70 of the 82**
  binaries. `posix.o` likewise. `sxgui.o` into 11.
- `aboutapp` is 487 KB of which 461 KB is the shared runtime: 8 parts runtime to
  one part application.
- Resident code is roughly 170 KB of `.text`+`.rodata` per process, so a desktop
  session duplicates ~1.4 MB.
- `mediaplayer-ffmpeg` is 7 MB: all of FFmpeg inside one binary, and the only
  consumer.

So the case is the runtime duplication, and it is real.

## Decisions taken

**Libraries land in `/lib`, not `/bin`.** `/bin` is what Program Manager scans for
programs, so a library there would show up as an application. `/disk/lib`.

**Backing is read-only; writable segments are private copies.** No page another
process has mapped is ever written, which keeps copy-on-write out of the first
milestone. Each process pays for its own data segment, which is what every
`DT_NEEDED` library already costs on Linux.

**The identity cache is keyed on `inode_id` + offset + length.** `inode_id` alone
treated "the whole file" and "one slice of it" as the same section, and a loader
asking for one `PT_LOAD` got the entire image mapped behind that segment.

**No content-generation counter yet.** `write_file` rewrites an existing inode,
so `inode_id` survives an in-place overwrite and a cache keyed on it alone would
serve the old image. That is unreachable today — there is no install syscall,
Add/Remove Programs can only uninstall, and the build rewrites `disk.img` from the
host with no kernel running to be stale. The moment an install syscall lands, the
key needs a generation bumped by `write_file`, `truncate_file`, `unlink_file`,
`rename_path` and create, **plus** the test that proves it. Five hooks and a test
is not worth writing before the mutation path exists.

**Naming follows Linux, and the version is the system's.** Libraries are
`libfoo.so.0.4`, and the version tracks the OS rather than being independent per
library — one ABI number for the whole system means there is exactly one number to
get right. `.dll` was considered and dropped: nothing about the mechanism requires
Windows naming, and the file extension buys no functionality (SxFS does not care
about it).

**Resolution is (a) now, (b) later.** Today `DT_NEEDED` would hold the exact
filename, so upgrading a library means relinking every program against it —
which reproduces the static-link problem one level down. The better scheme is
`DT_SONAME` plus resolving it by scanning `/lib` for the highest
`libfoo.so.0.*`, which reimplements the symlink Linux uses at load time. It waits
for an install path, because until there is one there is nothing to upgrade.

Note that a major version in the SONAME is a promise, not a fact: today SXGFX's
headers *are* the SDK's public surface and nobody verifies what may change. The
`VERSION` tag in `.sxmeta` is the honest one.

## What is built

Verified by `./build.sh smoke smoke`, `--smp 4`, and the Doom persistence check.

| | |
| --- | --- |
| Section views can be executable; W^X preserved | done |
| Section view and section budgets raised, and proven | done |
| `section_open`, `section_open_range`, `map_view_at` | done |
| One file-backed section shared across processes by inode | done |
| `libmath.so.0.4` built as PIC, staged in `/lib` | done |
| `ldso`: place `PT_LOAD`s, relocate, resolve by name | done |
| `PT_INTERP` delivered to `crt0` | done |
| `ET_DYN` accepted by the kernel with a load bias | done |
| First PIE executable with `.dynsym` (`pietest`) | done |
| Two `PT_LOAD`s may share a page, union of permissions | done |
| `ldso` holds an array of libraries, not one | done |
| One load bias shared by every segment of a library | done |
| Relocations resolve by name across the loaded set | done |
| `DT_NEEDED` walked from `/lib`, each dependency loaded once | done |
| The executable in the symbol scope, with its `.dynsym` | done |
| `-fstack-protector-strong` back on for libraries | done |
| `ldso` relocates the executable, `R_X86_64_RELATIVE` and all | done |
| A program linking `libmath.so.0.4` and calling `sqrt` (`libtest`) | done |
| A desktop program on the library (`calc`, `sqrt` from `/lib`) | done |
| `libsxgui.so.0.4`, resolving `gfx_*` against the executable | done |
| `ldso_symbol_is_shared()`: "did this come from a `.so`?" | done |
| SxGUI drawing correctly from a library, pixel-compared | done |
| `crt0` runs the loader before `main`, via a weak hook | done |

## crt0 runs the interpreter

A program that links a library must be operable before `main`, and the kernel
maps the main image without relocating it. `crt0` calls `sx_start_dynamic()` on
the way to `main`; that checks a weak `sx_run_interpreter` and calls it if the
program defined one. `ldso.c` defines it, so linking the loader is what opts a
program in. The other programs do not define it and pay nothing but a null test.

The check has to be in C. Asking in assembly whether a weak symbol is defined
means `movq symbol(%rip), %rax`, which *reads memory at the symbol's address* —
and for an undefined weak symbol that address is zero, so the process takes a
page fault on address 0 before reaching `main`. The linker gives an undefined
weak symbol a GOT entry holding zero, and that is what the C test reads.

This does not make the model Linux-shaped: the kernel still maps the main image
and the program still does its own linking. What it removes is the rule that
every program has to remember to call the loader first, which had no teeth
because nothing checked it.

`ldso_start()` is idempotent on purpose. `R_X86_64_RELATIVE` *adds* the bias, so
running the relocations twice would leave the image's pointers off by
`kUserBase`.

## The executable has to be relocated too

The kernel maps the main image and hands it to the process, but it does not touch
the `GOT`. A program that links a library therefore starts life with `DT_NEEDED`
in its dynamic table and an empty slot for every call into one. `ldso_start()`
is that missing step: walk the *executable's* `DT_NEEDED`, then apply the
executable's own relocations. Same order as for a library — chain first,
relocations after — because a call into a dependency cannot be resolved before
that dependency is mapped.

`R_X86_64_RELATIVE` is the other half. It carries no symbol: the value stored in
the image is already a link-time address and only needs the image's bias added.
Skipping it leaves a relocated image with pointers to address zero, which is not
a crash but a program that quietly reads the wrong thing.

Today `ldso_start()` is called from `main`, so a program must not touch a library
symbol before it. `crt0` doing it is the step after this one, and it is the
point where the model stops being hybrid.

## One runtime unit list

`savanxp_user_runtime`, `savanxp_user_runtime_pic` and `savanxp_user_runtime_pic_nomath`
used to have different source lists, and the PIC one was a superset: it carried
`sxgui`, `sxe` and `audio`. A working `STATIC` program had to list those units by
hand, and the moment it moved to `PIE` the same units became duplicate symbols.
Changing profile was not a one-line change but a source edit, which is the
opposite of what a profile is for.

One list now feeds all three, and the 14 programs that listed runtime units
stopped listing them. Extra units cost nothing in a program that does not use
them: the compile options carry `-ffunction-sections`/`-fdata-sections` and the
`STATIC` profile passes `--gc-sections`, so the unreferenced ones go at link
time.

## The build has to leave the symbol undefined

`libtest` links `LINK_PROFILE PIE WITHOUT_MATH`. `WITHOUT_MATH` drops `math.c`
from the runtime sources — without that, `sqrt` is defined inside the executable,
the linker never emits a `DT_NEEDED`, and the test passes against a private copy
of `math.c` with no library involved at all. Removing the unit is what makes the
test real.

## Finding the executable

The kernel maps the main image, so the loader has no descriptor and no section
to open for it. It finds the base the way an interpreter does when it has nowhere
to ask: take the address of one of its own functions — which is by definition in
the executable's image — and walk backwards a page at a time looking for the ELF
header. The walk is bounded on both ends, so an unreadable header cannot turn into
an unbounded scan.

`placed[]` is then filled in from the image's own geometry instead of by mapping
anything: the kernel put each `PT_LOAD` at `p_vaddr` plus one bias, and the bias
is `kUserBase` for an `ET_DYN`, zero for an `ET_EXEC`. That is the same rule as
`kernel/elf.cpp`, and it has to match it — otherwise every executable address
lands on the wrong page.

The executable takes **slot 0** and libraries start at slot 1. `resolve` already
walked slots from newest to oldest, so putting the executable first makes it the
**last** thing searched: the background of the scope, not its beginning.

An executable with no `.dynsym` — a non-PIE `ET_EXEC` — is a normal case, not a
load failure. Its slot keeps zero tables and `resolve` skips it. `ldtest` is
linked `LINK_PROFILE PIE` for exactly this reason: without `--export-dynamic` a
library has nothing to resolve against.

## What the canary proves

`-fstack-protector-strong` was off for libraries because the canary makes every
object reference `__stack_chk_guard` and `__stack_chk_fail`, which live in the
executable. With the executable in scope, it is back on.

Resolving the symbol is not the same as the protection working: if the `GLOB_DAT`
relocation pointed anywhere at all, and both halves of the comparison happened to
match, the check would pass while detecting nothing. So `ldtest` reads the guard
through the address the resolver returned and checks it is non-zero and that its
two halves differ — `crt0` seeds it from two registers precisely so that an
eight-byte stack smash cannot reach it.

## One load bias per library

Every `PT_LOAD` of an `ET_DYN` has to be reachable at `bias + p_vaddr`, because
that is the arithmetic the linker assumed for every `%rip`-relative reference it
emitted. Two things follow, and both were wrong at first:

- **The segments must share one bias.** Placing each one wherever the kernel
  offers leaves a `PLT` in the text segment computing a `GOT` address in the data
  segment using the text segment's anchor. The two anchors differ by each
  segment's offset inside its page, and the jump lands in unrelated code.

- **A segment's bytes must start at `p_vaddr`, not at the start of its mapping.**
  `map_view_at` takes a page-aligned address and puts the section's first byte
  there, so the loader asks for the range from the start of the segment's *page*
  (`p_offset - (p_vaddr & 4095)`) rather than from `p_offset`. `p_offset` and
  `p_vaddr` share the same remainder modulo the page size, so the start is never
  negative. Mapping from `p_offset` instead puts the content that far too low, and
  the error lands inside the image: a variable shows up where another one was,
  and the code runs.

The two bugs hid each other. Reading a table out of a segment worked while the
address arithmetic was wrong, because the same off-by-offset appeared on both
sides of the comparison.

## Relocation order

`DT_NEEDED` is walked *before* the GOT is filled, not after. A `JUMP_SLOT` that
points into a dependency can only be resolved once that dependency is mapped, and
walking the chain afterwards fills it against a world that does not exist yet.
Loading a dependency moves the working-library pointer, so `ldso` saves it and
restores it on the way back; otherwise the caller's relocations would be applied
to whichever library was loaded last.

`resolve` skips `SHN_UNDEF` entries. An undefined symbol with the name being
looked up is the question, not the answer, and its `st_value` of zero would
translate to the base of the image — a call that lands on the ELF header.

## R_X86_64_RELATIVE read the wrong field

`calc` was the first real program to be flipped to `LINK_PROFILE PIE`, and it
died on the first write to stdout with `cr2 = 0x40000c` — a write to the load
bias, landing in the read-only first page of its own image. `stdout` is
`FILE* stdout = &g_stdout_file`, and it held exactly `0x400000`.

The cause is that `R_X86_64_RELATIVE` was applied as `*addr += bias`. That is
the usual shape of the relocation, and it works only for linkers that also
*store* the link-time value in the field. `lld` does not: it leaves the slot at
**zero** and puts the link-time address in `r_addend`. Adding the bias to a zero
yields `base + 0`, which is a pointer to the start of the image.

For `libtest` the numbers are exact:

| | |
| --- | --- |
| `stdout` slot | `0x44bb8` |
| relocation | `R_X86_64_RELATIVE`, addend `0x45058` |
| bytes at that offset in the file | `0` |
| value at runtime | `0x400000` |

The authoritative value is `r_addend`, not the memory. The fix is to store
`r_addend + bias`.

It went unnoticed because nothing reached it. Every PIE program in the tree
called `sqrt` and nothing else — a call goes through a `JUMP_SLOT`, resolved by
name, which was never affected. A relocated global *pointer* was never touched
by any of them, and that is the whole class of bug `apply_table_in` had. Two
other things had to be fixed before this one was even reachable: `crt0` was
destroying `argc`/`argv`, and the kernel was writing the interpreter path over
the last argument string.

`libtest` now checks a relocated global pointer, because a test that only calls
`sqrt` cannot see this.

## The PIE profile used to declare an interpreter that does not exist

`lld`, given `-pie` without `-static`, writes `/lib64/ld-linux-x86-64.so.2` into
every `ET_DYN` by default — a path from the host that builds, and one SavanXP has
no file for. The profile now declares `/disk/lib/ld.so.0.4`.

## The kernel wrote the interpreter path over an argv string

It went *above* the `argv` pointer array, reasoning that the space where `envp`
lives in Linux is unused here. It is not unused: the argv *strings* sit
immediately above the array. With `calc --selftest` the array landed at
`0x6fffffffc8`, the `"--selftest"` string at `0x6fffffffe0`, and the 28-byte
interpreter path began at exactly `0x6fffffffe0`. `argv` looked perfect — right
pointers, right count — and the program read `argv[1]` as
`/lib64/ld-linux-x86-64.so.2`.

The path is now reserved *between* the strings and the array, by lowering
`user_sp` before the array is built. This was already reachable: `interptest` has
a `PT_INTERP` and never collided only because whether the path lands on the
lowest string depends on the lengths involved.

## calc dropped its own decimal engine

`calc` had a 16-significant-digit decimal arithmetic of its own — a 16-digit
`int64` mantissa plus an exponent of ten, with `wide_divmod` and a Newton
`isqrt` in 128-bit integers. The file said why it existed: the in-tree userland
was built `-mno-sse`, so `double` did not compile. That reason is gone, and
`math.c` is a shared library now, so the engine was roughly 340 lines to maintain
for a `sqrt` that lives in `/lib`.

The engine is a `double` and the operations come from `libmath.so.0.4`. `calc`
declares `DT_NEEDED libmath.so.0.4`, `sqrt` is undefined in the binary, and
there is no local copy:

```
$ nm build/linux/calc | grep -w sqrt
                 U sqrt
$ objdump -R build/linux/calc | grep sqrt
0000000000048910 R_X86_64_JUMP_SLOT  sqrt
```

Two things needed deciding rather than assuming.

**How many digits to show.** The engine had 16. A `double` has 53 mantissa bits,
about 15.95 decimal digits, so the sixteenth is not always true —
`9999999999999999` has no exact representation and rounds to `10000000000000000`.
Showing 16 would advertise a precision the value does not have, and it shows up
in the last digit. `CALC_DIGITS` is now 15, and the entry buffer follows it
because both share the constant.

**Not to leak binary noise.** `0.1 + 0.2` is `0.30000000000000004` in binary.
`%.17g` would show that, which is exactly the defect the decimal engine existed to
avoid. `calc_format` rounds to `CALC_DIGITS` for display and keeps the full
precision internally, so the screen reads `0.3` while the number does not.

It also stopped using `%g`, for two reasons: `%g`'s threshold puts `1e-5` where a
calculator shows `0.00001`, and it zero-pads the exponent, so `1.25e-07` grows a
phantom zero. `calc_format` picks the notation itself and trims the mantissa —
moving the exponent suffix rather than writing a NUL over the `e`, which would
have eaten it.

### What a user sees change

| | before | after | why |
| --- | --- | --- | --- |
| `0.1+0.2` | `0.3` | `0.3` | display rounding, unchanged |
| `2/3` | `0.6666666666666667` | `0.666666666666667` | 15 significant digits |
| `1/3 × 3` | `0.9999999999999999` | `1` | the hardware result is exact |
| `√2` | `1.414213562373095` | `1.4142135623731` | 15 significant digits |
| typed digits | 16 | 15 | a `double` cannot carry 16 |
| `9999999999999999 × 9` | `8.999999999999999e+16` | `8.99999999999999e+15` | the 16th digit is gone at entry |

The last row is a real loss, not cosmetics: the decimal engine multiplied a
16-digit integer exactly. Every desktop calculator has it too — they are all
IEEE-754 — and it is the price of the arithmetic being shared rather than
maintained here.

`calc_parse` filters the text before handing it to `strtod`, because `strtod`
stops at the first character it cannot use and `"1,234.5"` — a pasted number
with a thousands separator — would read as `1`. The old parser ignored
non-digits anywhere; that leniency is part of the behaviour, so it survived.

## SxGUI as a library

`libsxgui.so.0.4` is `sxgui.c` plus `sxgui_app.c` — 4553 lines that every SxGUI
program used to carry. It has **no `DT_NEEDED`** of its own, and that is the
design decision worth explaining.

Its undefined symbols are exactly `gfx_*`, `sxchrome_*`, `clipboard_*`, libc and
the stack canary. `gfx_*` is SxGFX, the layer underneath, which is *not* a
library yet. But the executable is already in the symbol scope, so `libsxgui`
leaves `gfx_*` undefined and the program resolves it out of its own binary. That
keeps SxGFX in the runtime, which is what lets the GUI migration happen without
reaching the deepest layer at the same time.

The cost is that **every program using SxGUI has to be PIE**, because a non-PIE
`ET_EXEC` has no `.dynsym` and therefore exports nothing for the library to
resolve against. That makes this the natural place where "migrate the programs to
PIE" stops being optional housekeeping.

### What is verified, and what is not

`calc` is migrated: it declares `DT_NEEDED libmath.so.0.4` and
`libsxgui.so.0.4`, and has no local copy of either. `calc-smoke` proves that the
library opens, relocates, resolves its own two translation units against each
other, and resolves its `gfx_*` and `sxchrome_*` references **against the
executable** — if any of those had failed, `apply_table_in` would have returned
failure and `sx_start_dynamic` would have said so.

It does not prove the library's code runs, because `calc`'s self-test is
arithmetic and returns before `sxgui_app_init`.

### The gap, and the visual scenarios

That gap was "no SxGUI consumer has a smoke that reaches drawing". The fix was
already in the tree and had been missed: `tools/shoot_session.py` drives real
apps over QMP and compares pixels, and `scenario_notepadwheel` types forty lines
into the editor, scrolls with the wheel, clicks the scrollbar and asserts that
the pixels moved.

`notepad` is a SxGUI consumer with 23 `sxgui_*` references, so migrating it turns
that scenario into the verification. It does:

```
./tools/shoot.sh --scenario notepadwheel --out-dir /tmp/shots
```

Six screenshots, four pixel comparisons, and `notepadwheel: OK`. Run once with
`notepad` as `ET_EXEC` and once with it on the library, and **all six PNGs are
byte-identical**. A renderer that behaved differently under a `.so` would move
the scroll area, and the comparison would fail.

So SxGUI under a shared library is verified at the pixel level, not just at the
"it loaded" level. That is the gate for this work: `smoke` and `calc-smoke` say
the loader is fine, `shoot.sh --scenario notepadwheel` says the toolkit draws.

`widgetsdemo` is migrated as well — twenty `sxgui_*` references, the broadest
user of SxGUI, and a gallery meant to be looked at by hand.

### The check that was not a check

The first version of the check compared the address `ldso_lookup` reports for
`sxgui_app_init` with the address `calc` calls. It passed — and it passed with
SxGUI back inside the binary too, because then the executable had its own copy
and both addresses still matched. A check that cannot fail is not a check.

`ldso_symbol_is_shared()` exists because of that. The executable is slot 0, so
"came from a shared library" is "the slot it was found in is not zero", and that
is a question a program can ask. `calc` now asserts the symbol is shared *and*
that one of its own functions is not, which is the negative control: without it,
the positive assertion would mean nothing.

## What blocks the rest, in order

1. **Migrate the remaining SxGUI programs.** `calc`, `notepad` and `widgetsdemo`
   are on the library. The other users of `sxgui_*` are `taskmgr`, `progman`,
   `appwiz`, `mines`, `filesapp`, `aboutapp` and `seltest`. Each has to become
   PIE at the same time, because the library resolves `gfx_*` against the
   executable and a non-PIE has no `.dynsym` to export. `kbdlayoutpopup`,
   `shellui` and `taskbar` include the header for types only and need nothing.

   Every migration should run `./tools/shoot.sh --scenario notepadwheel`
   afterwards, and the scenario that matches the app being moved if one exists.

2. **Wire the visual scenarios into `build.sh smoke`.** `notepadwheel` is the
   gate for this work and right now it only runs if someone remembers. It needs
   `windowd`, screenshots and several seconds, which is why `taskbar-smoke` is
   its own scenario rather than part of `smoke`. Making the SxGUI gate
   unskippable is worth a second host driver in `run_smoke.py`, reusing
   `shoot_session` instead of the separate `taskbar_smoke` client.

3. **Export control and versioning.** `DT_SONAME` is read but ignored; a `DT_NEEDED`
   name is looked up verbatim under `/lib`. Symbol visibility beyond global, and
   versioned names, are not implemented.

4. **`dlopen`, and unloading.** The library array is fixed at load time and there
   is no reference counting, so nothing can be removed once mapped. `kMaxLibraries`
   is 8.

5. **A second executable to lean on.** `calc` and `libtest` both pass, which is
   two. The profile has only been exercised by programs whose whole working set
   is a screen and a calculator; a window manager or a compositor under `ET_DYN`
   would exercise relocation far harder than either does.

6. **SxGFX as a library.** 6248 lines are still copied into every program, and
   once the SxGUI consumers are on PIE the pieces are in place to move it.

## Explicitly not worth doing yet

- **`mprotect` and RELRO.** `BIND_NOW` does not need either; lazy PLT binding does.
- **File demand paging.** A file-backed section is read whole. Sharing saves
  resident memory, not mapped memory, and `memory_bytes` counts mapped-present
  pages, so Task Manager shows the same per-process figure either way.
- **`dlopen`.** Loading by name is a different feature from `DT_NEEDED`, and the
  FFmpeg codec case needs `dlopen`, not this. `CHANGELOG.md` records music as
  deferred for exactly this reason.
