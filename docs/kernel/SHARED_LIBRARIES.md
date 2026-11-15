# Shared libraries: where this is going

The goal is a loader that maps `libSxGFX`, `libSxGUI` and `libFFmpeg` once,
instead of the 70 of 82 programs in `/bin` carrying their own copy. The engine
is described in [`SECTIONS.md`](./SECTIONS.md); this file records the decisions and
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
- the Media Player **was** 7 MB with all of FFmpeg inside one binary, and was the
  only consumer of it. It is now 496 KB and shares `libffmpeg.so.0.4` with nothing
  else, which is the point of the library: the duplication moved from "the runtime,
  once per program" to "the engine, once per image".

So the case is the runtime duplication, and it is real.

## Decisions taken

**Libraries land in `/lib`, not `/bin`.** `/bin` is what Program Manager scans for
programs, so a library there would show up as an application. `/lib`.

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

(b) arrived before the install path, on 2026-11-14, because the persistent image
already is one: external binaries in `/bin` outlive the system that built them.
See [DT_SONAME resolution](#dt_soname-resolution) for the rule that was chosen.

Note that a major version in the SONAME is a promise, not a fact: today SXGFX's
headers *are* the SDK's public surface and nobody verifies what may change. The
`VERSION` tag in `.sxmeta` is the honest one.

## What is built

Everything in this section is measured against the current tree, not against the
plan. The number in each row came out of `readelf`, `nm` or a script in `tools/`.

### The mechanism

| | |
| --- | --- |
| File-backed sections shared across processes by inode | done |
| Section views can be executable, W^X preserved | done |
| `section_open`, `section_open_range`, `map_view_at` | done |
| Section and section-view budgets raised, and proven | done |
| Two `PT_LOAD` segments may share a page, union of permissions | done |
| `ET_DYN` accepted by the kernel, with a load bias | done |
| Kernel delivers the interpreter path in `rcx`, the image base in `r8`, a fresh canary in `rdx` | done |
| `crt0` runs the loader before `main`, through a weak hook | done |
| `PT_INTERP` declared by the PIE profile | done |
| Loader places `PT_LOAD`s: text from the file, data as private copies | done |
| One load bias per library, shared by every segment | done |
| `R_X86_64_RELATIVE`, `R_X86_64_JUMP_SLOT`, `R_X86_64_GLOB_DAT` | done |
| The executable is in the symbol scope, and is relocated too | done |
| `DT_NEEDED` walked from `/lib`, each dependency loaded once | done |
| A two-library dependency chain, proven by `libchaintop` → `libchainbase` | done |
| A diamond in the dependency graph | handled, **not tested** |
| `ldso_lookup()`, `ldso_loaded()`, `ldso_symbol_is_shared()` | done |
| `<dlfcn.h>`: `dlopen`, `dlsym`, `dlclose`, `dlerror`, `dladdr`, per-handle scopes | done, [see below](#dlopen) |
| One loader for SUN and POSIX, `system/loader/ldso.c`; a SUN program maps a library | done, [see below](#one-loader-over-sxn) |
| `DT_NEEDED` resolution: the exact file, else the highest newer minor; the latest when a name gives no full version | done, [see below](#dt_soname-resolution) |
| 32 library slots, 24 program headers per image | done |

### The libraries

The system's libraries, measured 2026-11-14, after the toolkit split
([The toolkit, split](#the-toolkit-split-into-neutral-and-host)). The layering is
strictly downward: nothing depends on anything above it.

| library | size | `DT_NEEDED` | subsystem |
| --- | --- | --- | --- |
| `libmath.so.0.4` | 41 KB | — | neutral |
| `libsxmidi.so.0.4` | 51 KB | — | neutral |
| `libsximg.so.0.4` | 24 KB | — | neutral |
| `libgfx2d.so.0.4` | 132 KB | — | neutral |
| `libsxgui.so.0.4` | 154 KB | `libgfx2d` | neutral |
| `libsxgfx.so.0.4` | 68 KB | `libgfx2d` | POSIX |

`libffmpeg.so.0.4` is not in this table because the tree does not **build** it:
`ports/ffmpeg` links it and stages it at `/lib`, and the tree consumes it as an
imported CMake target, so it exists only in an image that has the port installed —
which is also when `mediaplayer` is built at all. It is two orders of magnitude
larger than anything above, and
[its section](#ffmpeg-as-one-library-and-what-it-cost-to-find-out) records what
loading it exposed.

A test image carries **thirty-three** files in `/lib`, and the number is worth
reading carefully because most of it is not the system: six interface libraries
(the table), `libffmpeg.so.0.4` from the port, and twenty-six that exist only to
be loaded by a test -- the chain, the diamond, `libneeded`, `libbroken`, the
`libdlmod_*` of `dltest`, `libweakref`, and the versions of `sonametest`. So
"thirty-three shared libraries" is not a claim about what the system can do. Six
is.

### The programs

116 executables in a test image's `/bin` and `/system`: 39 are `ET_DYN`, 30
declare a `DT_NEEDED`. By library: `libsxgui` 20, `libgfx2d` 20, `libsxgfx` 19,
`libmath` 5, `libsximg` 1, `libffmpeg` 1, and the test libraries. Six of the
`libsxgui` programs are SUN. `libsxmidi` has no in-tree consumer: the only
program that maps it is `/bin/doomgeneric`, built by `ports/doomgeneric`.

What each library asks of the program:

| library | asks the program for |
| --- | --- |
| `libmath` | nothing |
| `libsxmidi`, `libsximg` | memory functions |
| `libgfx2d` | `malloc` `free` `memcpy` `memset` |
| `libsxgui` | `memcpy` `memmove` `memset` `strlen`, and `sxgui_default_host`, weakly |
| `libsxgfx` | POSIX calls and libc |

All of those live in the runtime, not in the application, and only `libsxgfx`'s
are a single subsystem's.

### The guarantees, and what enforces each

| guarantee | enforced by |
| --- | --- |
| No executable carries a private copy of a library it maps | `tools/check_shared_libs.py`, at the end of every `./build.sh build`, exit 1 |
| No binary on the disk carries an `RPATH` or `RUNPATH` | `tools/check_shared_libs.py`; `SKIP_BUILD_RPATH` in `savanxp_program` and `savanxp_library` |
| The loader speaks SXN and calls no C library | `tools/check_shared_libs.py`: each compiled `system/loader/ldso.c` imports only the canary, `sx_image_base` and `sx_loader_report` |
| A SUN program maps a library through the same loader | `sun-libtest`: `sunlibtest.sxe` maps `libmath` and asserts `sqrt` came from it |
| A missing library resolves to its highest newer minor, never an older one, and the exact file wins; a name without a full version gets the latest | `sonametest`, `sonametest-newer` and `dltest` |
| The stack canary is in the executable, seeded by `crt0`, with two distinct halves | `ldtest`, on every run |
| The interpreter path reaches the loader | `interptest`, including that a library actually loaded |
| The toolkit draws the right glyphs in the right colour | `expect_text()` in `shoot_session`, from the font tables inside the built `libsxgfx` |
| Rebuilt programs reach the bootable image | the rootfs stamp depends on the binaries themselves |

`check_shared_libs` derives each library's symbols from the built artifacts rather
than a list of prefixes, so a new library is covered without editing it.

Every one of these was verified by breaking the thing it watches. Deleting
`SAVANXP_LIBRARY_REPLACES_libsxgfx` or `..._libgfx2d` gives 21 and 15 violations and
exit 1; painting the editor's rows or a taskbar label in the background colour fails
at 0 of 228 and 0 of 284 glyph pixels.

### What runs

`smoke`, `sxgui-smoke`, `taskbar-smoke`, `windowd-smoke`, `calc-smoke`, `--smp 4`,
`verify_doom_persistence.sh`, and 18 visual scenarios in `shoot_session`. For the
loader itself: `dltest`, `brokentest`, `missingtest`, `slottest`, `diamondtest`,
`needstest-missing`, `ldfailtest`, `ldstatictest`, `ldimagetest`, `sun-libtest`, `sonametest`,
`sonametest-newer`, `posix-loader-log`, `ffmpegload`. For the SUN programs that
map the toolkit: `taskmgr-smoke`, `logview-smoke`, `diskmgmt-smoke`,
`cursor-settings-smoke`, and the `taskmgrmenu`, `logview`, `sound`, `sxgldiag`
and `diskmgmt` visual scenarios.

### Limits that are part of the current design, not bugs

- **No lazy binding, deliberately.** The loader applies `DT_JMPREL` at load, like
  `BIND_NOW` and without reading it. It costs a little work per library at startup
  and buys the property that a broken dependency is a startup failure with a name
  rather than a crash at whichever call site happened to be first.
- **`--export-dynamic` is still required for POSIX PIE programs**, for `dlopen`:
  a library opened at run time may ask the program for anything. A SUN PIE
  program is linked without it, so `--gc-sections` works; lld still exports what
  the libraries on its link line ask for
  ([The toolkit, split](#the-toolkit-split-into-neutral-and-host)).
- **A non-PIE program cannot use a library.** An `ET_EXEC` has no `PT_DYNAMIC`, so
  it has nothing to declare a `DT_NEEDED` with. This is a property of the link
  model, not of the loader; a STATIC program that links the loader is told so at
  startup (step `-11`).
- **No run path.** The loader looks in `/lib` only and ignores `RPATH` and
  `RUNPATH`. The system's binaries carry neither: CMake gave every target that
  links a library the build directory as its run path, and since binaries are
  copied onto the disk as built, 38 of them shipped a path inside the
  developer's home directory. `SKIP_BUILD_RPATH` stops it, and
  `check_shared_libs` fails the build if one reappears.
- **A `DT_NEEDED` is resolved by name, not by `DT_SONAME`**: the exact file under
  `/lib`, else the highest newer minor of the same ABI
  ([DT_SONAME resolution](#dt_soname-resolution)). A library's own `DT_SONAME`
  is still not read; its file name is its identity.
- **Global visibility only.** No `STV_HIDDEN`, `STV_PROTECTED`, or `.symver`.
- **No unload.** `dlclose` drops a reference and leaves the image mapped; see
  [`dlopen`](#dlopen).
- **3534 dead symbols inside 16 binaries** — 2359 `sxgui`, 731 `sx_*`, 444 `gfx` —
  because the PIE profile does not pass `--gc-sections`. Not a sharing problem; the
  check above passes.
- **The two limits are hard and untested.** `kMaxLibraries` and
  `kMaxProgramHeaders` are enforced with an explicit failure that no test exercises.

## crt0 runs the interpreter

`crt0` calls `sx_start_dynamic()` before `main`, through a weak hook the program
only defines if it links the loader. That is the whole of the mechanism by which a
program's dependencies get resolved: nothing in an application has to remember to do
it, and nothing can forget.

The check that the hook is defined lives in C, not in assembly. Testing a weak symbol
means looking at its GOT entry, and in assembly that would be `movq symbol(%rip)` --
which *reads memory at that address*. An undefined weak symbol has address zero
there, so the process died reading page zero before reaching `main`.

The image's own base comes from the kernel in `r8`, recorded above in the table.

## The executable has to be relocated too

The kernel maps the main image and hands it to the process, but it does not touch
the `GOT`. A program that links a library therefore starts life with `DT_NEEDED` in
its dynamic table and an empty slot for every call into one. `ldso_start()` is that
missing step: walk the *executable's* `DT_NEEDED`, then apply the executable's own
relocations. Same order as for a library — chain first, relocations after — because
a call into a dependency cannot be resolved before that dependency is mapped.

`R_X86_64_RELATIVE` is the other half. It carries no symbol: the value stored in the
image is already a link-time address and only needs the image's bias added. Skipping
it leaves a relocated image with pointers to address zero, which is not a crash but a
program that quietly reads the wrong thing.

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
no file for. The profile now declares `/lib/ld.so.0.4`.

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

So SxGUI under a shared library is verified, and the screenshots being identical
across the migration is what says the rendering did not change.

What the scenario does **not** check is worth knowing too. Painting
`sxgui_draw_control_text` in the background colour, so the typed lines are
invisible, still passes `sxgui-smoke`: the editor panel is still white, and the
scroll comparison is satisfied by the scrollbar thumb moving rather than by the
text. So the gate says the toolkit loads, paints, and responds to scroll and
clicks — it does not read the glyphs. That is fine for what this work needed
(the migration changed nothing, and identical pixels show it) and it would not be
fine for a change to how SxGUI draws.

`widgetsdemo` is migrated as well — twenty `sxgui_*` references, the broadest
user of SxGUI, and a gallery meant to be looked at by hand.

## sxgui-smoke, so the gate cannot rot

For three commits the only way to run that scenario was to remember
`./tools/shoot.sh --scenario notepadwheel`. A gate nobody runs is not a gate, so
it is a scenario now:

```
./build.sh smoke sxgui-smoke
```

`shoot_session.run_scenario()` is the reusable entry point — `shoot.sh`'s `main()`
plus a QEMU already running. `run_smoke.py`'s `completion=host` path, which was
hardcoded to `run_taskbar_actions` and to printing `TASKBAR SMOKE PASS`, now takes
`--host-action taskbar|visual` and prints whatever success token the scenario
declares. `taskbar-smoke` is unchanged apart from the token coming from the
catalog instead of being spelled out.

Checked that the gate can fail: with `sxgui_paint_content` returning immediately,
`sxgui-smoke` fails with "no se encontro ningun panel blanco donde probar la
rueda".

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

## A private copy of a library is not a failure, which is the problem

The migration had a failure mode that nothing caught. A program that declares
`DT_NEEDED libsxgui.so.0.4` and *also* defines the toolkit's symbols links
cleanly, runs correctly, and passes every test: the executable's own copy
satisfies the references, the library is mapped and never called. The only
symptom is that nothing is shared, and that the binaries are bigger.

It happened while moving SxGFX. Adding `libsxgfx` as a `DT_NEEDED` of `libsxgui`
meant every program already on the toolkit had to declare it too, or keep its own
copy of 3953 lines. Ten of them did, until the numbers were printed. Then
`seltest` — which had never been in the way — turned out to have a page gap
between two `PT_LOAD` segments, and the loader's scan for the image header read
the unmapped page. Neither showed up as a test.

`tools/check_shared_libs.py` now runs at the end of `./build.sh build` and fails
if any executable defines symbols belonging to a library it maps. Verified in both
directions: deleting the `SAVANXP_LIBRARY_REPLACES_libsxgfx` line, which is the
one-line omission that causes this, produced 21 violations and exit 1 while
`smoke`, `sxgui-smoke` and `taskbar-smoke` all passed.

## Nothing resolves against the program any more

The inventory above states this; this is why it happened. The layering ended up
strictly downward:

| library | holds | needs from below |
| --- | --- | --- |
| `libsxgfx.so.0.4` | `gfx.c` (the POSIX window client, `gfx_impl.inc`), `system/gfx/pixels.c` (primitives and three font tables) | libc, syscalls |
| `libgfx2d.so.0.4` | `system/gfx/gfx2d.c`, `system/gfx/sxchrome.c`, `gfx_scaled.c` | `libsxgfx`, libc |
| `libsxgui.so.0.4` | `system/gfx/sxgui.c`, `sxgui_app.c` (the POSIX frame and host) | `libgfx2d`, `libsxgfx`, libc |
| `libmath.so.0.4` | `math.c` | nothing at all |
| `libsxmidi.so.0.4` | `sxmidi.c` | nothing; libc symbols resolve against the program |

Measured, not asserted: `libmath` asks the executable for nothing, `libgfx2d` asks
only for `malloc`/`free`/`realloc`/`memcmp`/`memset`, `libsxgui` only for the C
runtime. **No library asks the program for a single symbol.** The consequence worth
stating is that a program no longer has to be PIE in order for a toolkit to find its
drawing code — that leash existed only because `libsxgui` used to resolve `gfx_*` and
`sx_*` against the executable, and `libgfx2d` is what cut the second half of it.

`--export-dynamic` stays, and it should: the libraries still need `memcpy` and the
syscalls, which live in the runtime inside the executable.

The leak found on the way is worth keeping. The loader kept `file_fd` **and** a
whole-file `file_section` open after the segments were mapped; the struct comment
even said the descriptor "stays open but the file is not read again" — a handle
saved in order to do nothing. That is two descriptors per library, forever. It went
unnoticed for the whole migration because every program loaded at most two
libraries and the descriptor budget had slack. `windowd` launches its clients and
they inherit the table, so adding a third library took a fresh app from 20
descriptors to 21 and `windowd-smoke` failed on its capacity check. Closing them is
also why `begin_load` now zeroes the two fields: the slot belongs to a library loaded
earlier, so a failed `open` would otherwise leave a stale handle that the error path
closes — someone else's descriptor.

## The runtime variant comes from DEPENDS

Excluding a runtime unit needs one runtime target per subset, and with `libmath`,
`libsxgui` and `libsxgfx` that is eight near-identical blocks that somebody would
eventually forget to add to. So there are no flags. A library declares the units
it replaces in `SAVANXP_LIBRARY_REPLACES_<lib>`, and `savanxp_program` derives
the subset from its own `DEPENDS`. The target is named after what it is missing,
so programs with different dependencies get different targets without a list.

This removed a way to fail quietly: `WITHOUT_MATH` was something the author had
to remember, and leaving it off meant the program linked fine and carried a
private `sqrt`. That mistake is no longer expressible.

## Reading the glyphs, and why the title is still not covered

`sxgui-smoke` used to prove that the editor scrolled by comparing screenshots, and
the proof was the scrollbar thumb. A toolkit that painted every typed line in
`SXGUI_COLOR_FACE` — invisible on the white field — moved the thumb identically.
Nothing noticed.

`tools/glyphs.py` now reads the glyph and coverage tables out of the built
`libsxgfx.so.0.4` and renders text with the same arithmetic the blitter uses:
baseline at ascent, bitmap at left/top, pen by advance, blend with
`inv = 255 - alpha`. It reads the library rather than the generated `.inc`, because
the library is what the process maps; a source-derived expectation could describe a
font the process does not have. Array sizes are derived rather than assumed — the
glyph count falls out of the range table, the coverage extent out of the glyphs.

`expect_text` does not look for a position, it looks for *any* position, so it does
not have to track the control's geometry — the thing that changes every time the
toolkit is touched. It refuses outright when asked to assert text in the colour of
its background: that is indistinguishable from not painting it, and `SXGUI_COLOR_FACE`
is also the taskbar's background, so a search for a FACE-on-FACE label matches
anywhere on screen.

Two strings are asserted. The Notepad editor's lines, two consecutive ones, which
also pins the row height — `sxgui_row_height()` is `gfx_text_height() + 4`, mirrored
in the harness the way the taskbar geometry already is. And a taskbar button label,
284 glyph pixels.

Both were verified by breaking them:

| broken | result |
| --- | --- |
| editor rows painted in `FACE` | FALLA, 0 of 228 |
| taskbar label drawn as `XXXROTO` | FALLA, 0 of 284 |
| taskbar label painted in `FACE` | FALLA, 0 of 284 |

The **window caption is not asserted**, and it is worth saying why rather than
leaving it to look like coverage. The caption is a 24-band gradient with an accent
that mixes more at the left than the right; predicting it from the harness means
copying the arithmetic in `windowd_render.c`. What is left is asserting only the
glyph pixels at full coverage, which do not depend on the background — and for
"Notepad" that is **14 pixels**. Changing the caption from `SX_FONT_UI_TITLE` to
the body font still passed. Eleven pixels are not a claim. It is listed below as
work, not shipped as a check.

## The staging step did not depend on the programs

Found while proving the taskbar label assertion, by making the label draw
`XXXROTO`, rebuilding, and seeing `taskbar: OK`.

`add_custom_command(OUTPUT rootfs.stamp ... DEPENDS savanxp_userland ...)`.
`savanxp_userland` is `add_custom_target(... DEPENDS ${SAVANXP_USER_TARGETS})` — an
aggregate with no output of its own. CMake does not walk through a custom target to
find outputs when resolving a custom command's dependency, so the stamp had no file
dependency on any program and was considered up to date while the executables were
already rebuilt. `rootfs/bin/taskbar` was ten minutes older than
`build/linux/taskbar`, and `disk.img` booted the old one.

Nothing failed. The build passed, `check_shared_libs` passed, and the visual
scenarios passed — because they were testing the previous binary. The stamp now
depends on `${SAVANXP_USER_TARGETS}` directly.

Two things about how it was found are worth keeping. The first two "the assertion
did not catch it" results were *correct*: Notepad's editor does not go through
`sxgui_draw_control_text` or the listbox row painter, so breaking those changed
nothing on screen. Only breaking the path the editor actually uses made it fail,
which is what a real check does. And the build was being run with its output piped
to `/dev/null`; one cycle was lost to an edit that did not compile, leaving a stale
binary that looked like a passing test.

## The slot was the expensive part, not the count

`kMaxLibraries` went from 8 to 32, but that number is the cheap half of the change.

`Library` holds the program headers of one library in `headers[64]` and the placed
address of each segment in `placed[64]` — 4096 of its ~4300 bytes. Measured over
everything this tree builds, the highest `e_phnum` is **15**. At
`kMaxProgramHeaders = 24` the slot is ~1812 bytes, so:

| | per slot | 32 slots resident per process |
| --- | --- | --- |
| before | 4372 B | 134 KiB |
| after | 1812 B | **56 KiB** |

That number is real rather than nominal. The kernel maps every page of every
`PT_LOAD` eagerly, allocating and zeroing a physical page for each, and `p_memsz`
covers the `.bss` tail — so an untouched static array is resident memory that
`resident_user_bytes` walks and counts. An `ls` that maps nothing would have paid
134 KiB for a ceiling it never approaches.

Lowering the array also turned a silent overflow into a refusal. `e_phnum` is checked
before the headers are copied, in both `read_header` and `adopt_executable`, so an
image with more than 24 program headers is now rejected with a reason instead of
running off the end of `headers[]`.

## What blocks the rest, in order

Four phases. Each has steps and a "done when" line, because a phase without an
acceptance condition is a wish.

| phase | state |
| --- | --- |
| 0 — diagnosable failures | done, five cases, each verified by breaking it |
| 1 — fail at load | already satisfied; the loader binds eagerly and always did |
| 2 — size and coverage | closed by measurement: 5.5 MB held back by `--export-dynamic`, and the caption assertion is not discriminating |
| 3 — FFmpeg shared | the only open phase, and it does not need `dlopen` |

So one phase is open. But the phase list is not the same as the list of things that
are missing, and the two should not be confused. What no phase covers:

- **Visibility.** Only global visibility exists: no `STV_HIDDEN`,
  `STV_PROTECTED`, `.symver`. (`DT_NEEDED` resolution, listed here before, has
  its section: [DT_SONAME resolution](#dt_soname-resolution).)
  Nothing in this tree needs them, and nothing that runs on it needs them either.
- **Half the loader's failure paths were untested.** Eleven return codes; four had
  a self-test. All have one now:
  [The loader's failure paths](#the-loaders-failure-paths).
- **A non-PIE program cannot use a library**, and used to find out silently: it
  links, runs, and its libraries fail to resolve. It is still not detected at build
  time, but the loader now says so at startup (step `-11`).

### Phase 0 — make failure diagnosable

A library that cannot be resolved reports a step number and nothing else, and
nothing at all is tested for a library that is missing, one that exceeds the slot
count, or one with an unresolvable symbol. `ldtest` covers a missing *symbol* through
`ldso_lookup`, which is a different thing.

**Done.** Five items, all in the smoke catalog, each verified by breaking it:

| case | how it is provoked | what it asserts |
| --- | --- | --- |
| unresolved symbol | `libbroken.so.0.4` calls a symbol nothing defines | `g_lib_fail_symbol` and `g_lib_fail_library` name it |
| missing library | `ldso_load` on a path that is not in `/lib` | step `-2`, no slot consumed, and the path really does not open |
| full slot table | `slottest`, compiled with `SAVANXP_LD_MAX_LIBRARIES=4` | step `-1`, and `-1` is not `-2` |
| the diamond | `libdia_top` → `{left, right}` → `leaf` | `ldso_count()` is 5, not 6 |
| a chain | `ldtest`, `libchaintop` → `libchainbase` | already covered |

What the negative runs produced, which is the point of doing them:

```
already_loaded off   ->  diamondtest: tras cargar top hay 6 imagenes y deberian ser 5
slot-full reports -2 ->  slottest: la cuarta dio -2, y el cupo lleno es -1
missing reports -1   ->  missingtest: fallo en el paso -1, y el de un archivo ausente es -2
no symbol recorded   ->  brokentest: la carga fallo pero el cargador no guardo ningun simbolo
```

Two things came out of writing them rather than out of reading the code.

**The slot limit is a compile-time constant per program.** `ldso.c` is compiled
*into* each program, so `SAVANXP_LD_MAX_LIBRARIES` is a `-D` away. `slottest` runs
with a limit of 4 instead of 32: it is the same loader with a different number, and
proving the path does not need 33 test libraries in the volume.

**A broken library cannot be built the obvious way.** lld rejects a shared library
with an unresolved symbol under `--no-allow-shlib-undefined`, which is the right
default and left no way to produce the case. `ALLOW_UNDEFINED` on `savanxp_library`
exists only for `libbroken` and says so in the build file — that flag is how a
broken third-party library is actually produced. `brokentest` then loads it by path
rather than declaring it, so the linker never inspects the library the test exists
to diagnose.

### Phase 1 — fail at load instead of at the first call

**Already satisfied, and the premise was wrong.** This phase was written assuming
the loader binds lazily, which is what a glibc-style loader does and what `-z now`
exists to turn off. SavanXP's loader does not: `apply_relocs_in` applies
`DT_JMPREL` as well as `DT_RELA`, unconditionally, from `ldso_load` and
`ldso_start`. There is no deferred path anywhere in it.

`brokentest` proves it without having to be believed. The missing symbol in
`libbroken` is reached through a **call**, so its relocation is a PLT entry, and the
test asserts the failure is step `-9` — the code `ldso_load` returns when
`apply_relocs_in` fails. If binding were lazy, that library would have loaded and
crashed at the call instead.

So `-z now` is a flag the loader does not read, and adding it would be decoration.
What the phase was actually after — an unresolved symbol reported by name at
startup rather than by a crash at some later call site — is what Phase 0 delivered.

One adjacent gap did come out of checking. A relocation whose type the loader does
not implement failed with a step number and nothing else, the same dead end the
symbol cases were. It now names the type. It cannot be provoked with this linker:
lld only emits those three for a well-formed `.so`. It is there for a hand-linked
object, which is where the rest of these libraries will come from.

### Phase 2 — size and coverage

**`--gc-sections`: blocked, and the blocker is `--export-dynamic`.**

Measured, not assumed. Added to the PIE profile, it drops **nothing**: 666 `gfx_`,
926 `sx_*` and 2836 `sxgui` dead symbols before and after, and 12 KB across 82
binaries.

The reason is that the two options are mutually exclusive. `--export-dynamic` puts
every global symbol in the executable's `.dynsym`, and an exported symbol is a root
for lld's reachability analysis, so nothing is unreachable by definition. Removing
it and keeping `--gc-sections` is what works:

| | dead symbols | total executables |
| --- | --- | --- |
| today | 4428 | 65.5 MB |
| `--gc-sections` alone | 4428 | 65.5 MB |
| `--gc-sections`, no `--export-dynamic` | **361** | **59.9 MB** |

So 5.5 MB is sitting behind the flag. And the flag cannot go: without it `ldtest`
fails with `sqrt no se encontro en la libreria`, because the libraries resolve
`memcpy`, `memset` and every syscall against the executable and the executable no
longer advertises them.

That is the same conclusion the reverted work reached, now with a number attached:
the saving depends on the C runtime leaving the executable. Phase 2 as written was
understood as "add a flag"; it is actually "finish the thing that was reverted".

**The window caption: two attempts, both non-discriminating, so no check.**

The caption is the only text with no glyph assertion, and it is also the only place
`SX_FONT_UI_TITLE` is used — the title font has no coverage at all.

It is hard because the caption is a 24-band gradient with an accent that mixes more
at the left than at the right. Two ways around that were tried and measured:

*Model the gradient.* That means copying `windowd_caption_colour()` and the band
arithmetic out of `windowd_render.c` into the harness. Honest coupling, but the
harness then breaks for an unrelated reason whenever the caption design changes.

*Do not model it: require the background to be some colour present in the area.*
This is the interesting one, and it fails for a measurable reason. The desktop
wallpaper behind the caption is itself a smooth gradient, so the area contains
thousands of colours. Counting first, to keep only colours that cover area:

| minimum repetitions | candidate backgrounds |
| --- | --- |
| 1 | 3330 |
| 64 | 1321 |
| 1024 | 45 |

At 1321 candidates the check accepted **both** fonts: the right one at the real
position, and the wrong one at an unrelated offset. A glyph match stops being
evidence once almost any pixel can be explained by some background. This is the same
mistake as the 14-pixel version it replaced, one step further along: not "too few
constraints" but "constraints that constrain nothing".

Both attempts were reverted rather than shipped. What would make it possible is the
caption's band geometry, which means either the compositor reporting what it painted,
or the harness modelling the gradient on purpose and accepting the coupling.

**What is left of Phase 2 is therefore empty of cheap work**, and both halves are
recorded above with the measurements that closed them.

### Phase 3 — FFmpeg — done

**Done.** `libffmpeg.so.0.4` is built shared, installed at `/lib`, and
`mediaplayer` maps it and decodes. The acceptance test below is met.

One correction to what this section predicted: it expected `mediaplayer` to map
`libavformat`, `libavcodec`, `libavutil` and `libswscale`/`libswresample` as five
libraries. It maps **one**. Splitting FFmpeg into its own components is a
distribution convenience — change a codec without relinking everything — and this
system has nothing to gain from it. One library is also the stronger test of the
loader, because a big `.dynsym` and a big relocation table are exactly what had
never been exercised. What the prediction got right is the part that mattered:
**eight slots of the 32 and a genuine `DT_NEEDED` graph.**

What follows is the reasoning as it stood, including why `dlopen` turned out not to
be needed. It has not aged badly.

**This phase does not need `dlopen`, and an earlier draft of this document said it
did.** That is wrong for this port. `configure.sh` enables decoders with
`--enable-decoder=...` and enables **no** external codec library — there is no
`--enable-lib*` anywhere in it — so h264, hevc, vp9, opus and the rest are compiled
into `libavcodec` itself. Nothing has to be found by name at runtime, and
`DT_NEEDED` covers the whole thing.

`dlopen` would only be needed if external codec libraries were enabled, which this
port does not do. It stays on the list as a missing feature, not as a blocker here.

What it will exercise, which nothing has so far: a chain of real depth with large
tables. `mediaplayer` would map `libavformat`, `libavcodec`, `libavutil`, and
`libswscale`/`libswresample` alongside the three interface libraries — eight slots
of the 32, and a genuine `DT_NEEDED` graph rather than four hand-made libraries
built to fit.

**Done when** `mediaplayer` maps `libavcodec` and decodes. Treat it as the acceptance
test for the subsystem: it is the first workload that was not designed to fit.

**A missing library is reported by the application, not by the launcher.** The
choice is deliberate and it rests on a property of the loader worth stating: when
`ldso_start` fails, `sx_start_dynamic` prints and **returns**, and `crt0` calls
`main` anyway. A program with an unresolvable `DT_NEEDED` still starts.

That makes the design possible, and it is not free of consequences. The GOT entries
pointing into the library that failed to load are still zero, so the application
must consult the loader **before its first call** rather than merely at startup: the
window opens, the state is checked, the message goes on screen, and no `av_*`
function is ever touched.

The loader now provides the missing piece. `ldso_missing()` returns the name of the
dependency that could not be loaded, and the `needstest-missing` scenario removes a
declared library from the volume to prove two things at once:

```
loader: no se pudo abrir /lib/libneeded.so.0.4 (paso 2)
NEEDSTEST SURVIVED
```

That the program is still running is the part that was never tested. `sx_start_dynamic`
prints the failure and returns, and `crt0` calls `main` anyway — so a program with an
unresolvable `DT_NEEDED` starts. That property is what makes an in-application report
possible at all, and if someone ever makes the loader fatal, this scenario fails.

The name recorded is the **deepest** failure, not the outermost: if `top` needs
`left` needs `leaf` and `leaf` is absent, `ldso_missing()` returns `leaf`, because
that is the one actually missing.

What it does not say is *why*. `ldso_missing()` means "this dependency did not load",
not "this dependency is absent" — a library that is present and broken also leaves its
name there, and saying "missing" would be a lie. The reason is in the line the loader
already printed.
- The check is "did any dependency fail to load", not "is every symbol present". A
  library whose own `DT_NEEDED` is incomplete loads, and calls into the missing part
  fault. The application's message would be wrong in that case, and only the symbol
  diagnostics from Phase 0 would catch it.

`mediaplayer-availability` and `mediaplayer-missing` are already system-side:
`init.c` probes for the backend and prints the verdict, and the smoke catalog reads
it. They do not move; what they remove changes from the binary to the library.

## A library is not uninstallable, and that is the point

Stated here because it is a gap that will otherwise look like a bug.

`appwiz_uninstall` does two things: it unlinks one binary, and it optionally removes
the application's data directory after revalidating that it is removable. There is
no library concept in it, and adding one would be wrong. `libmath.so.0.4` is in the
image because twenty programs need it; removing it breaks all twenty, and no
uninstaller can be the thing that decides otherwise. A shared library belongs to
the set of images that reference it, not to any one of them.

So an installed third-party library lands in `/lib` beside the system's own and
there is no way to take it out again, short of rebuilding the image. For `libffmpeg`
that is the arrangement: it is part of the system once installed, and the
application that uses it is a system application that cannot be removed either.

One consequence follows from the flat layout and is worth naming before a third-party
library arrives. Resolution is one directory and an exact filename — `/lib/`
plus the `DT_NEEDED` string — so `libffmpeg.so.0.4` and `libsxgui.so.0.4` share one
namespace, and a library installed by a third party can shadow a system's. There is
no SONAME matching to keep them apart, which is the same limitation already recorded
above and now has a practical consequence rather than only a theoretical one.

## The toolkit, split into neutral and host

Item 5 of [One loader over SXN](#one-loader-over-sxn), done 2026-11-14. Until then
the toolkit's libraries mixed what draws with what talks to the window system, and
the second half was POSIX: `libsxgfx` carried the POSIX window client next to the
pixel primitives, `libgfx2d` needed `libsxgfx` for its glyphs, and `libsxgui`
carried POSIX's application frame next to the toolkit. A SUN program could map
none of them.

| library | carries | `DT_NEEDED` | asks the program for |
| --- | --- | --- | --- |
| `libgfx2d` | `pixels.c` (primitives, the three font tables), `gfx2d.c` (the painter), `sxchrome.c` | — | `malloc` `free` `memcpy` `memset` |
| `libsxgui` | `sxgui.c`, the toolkit core | `libgfx2d` | `memcpy` `memmove` `memset` `strlen`, `sxgui_default_host` |
| `libsxgfx` | `gfx.c` (POSIX's window client), `gfx_scaled.c` | `libgfx2d` | POSIX calls and libc |

`libgfx2d` and `libsxgui` are neutral: every runtime defines what they ask for.
`libsxgfx` is POSIX's. The host part of the toolkit -- the clock and the
clipboard, through `struct sxgui_host` -- is the program's: POSIX's application
frame (`sxgui_app.c`, 4 KB, in the runtime's `gui` group) and SUN's
(`system/runtime/sxngui.c`) each define `sxgui_default_host`, and both are
linked into the program rather than shared.

Two details decide whether that works:

- **The core defines no default host, not even a weak one.** It used to have a
  weak `sxgui_default_host` returning nothing, overridden by POSIX's frame in the
  same library. Inside `libsxgui.so` that weak definition would win over the
  program's: this loader searches libraries newest first and the program last
  ([Finding the executable](#finding-the-executable)), the opposite of Linux. So
  the symbol is now undefined in the library and the program provides it. POSIX's
  definition is weak, because the runtime's units are linked whole into every
  program and a SUN program brings its own.
- **A SUN PIE program is linked without `--export-dynamic`.** That flag exports
  every symbol, and an exported symbol is never garbage, so `--gc-sections`
  dropped nothing and each program carried the whole runtime, SxGL included:
  ~400 KB of text. lld exports on its own what a library on the link line asks of
  the program, and only `dlopen` needs more, which SUN does not have. POSIX PIE
  programs keep the flag, for `dlopen`.

The six SUN programs that paint with SXGUI map the toolkit now
(`SAVANXP_SUN_TOOLKIT` in `cmake/SystemServices.cmake`). Their private text,
measured with `size` against the same programs linked STATIC:

| program | STATIC | PIE with the toolkit mapped |
| --- | --- | --- |
| `taskmgr` | 160,848 | 82,393 |
| `logview` | 169,918 | 87,566 |
| `sound` | 163,456 | 82,086 |
| `mouse` | 152,384 | 71,082 |
| `diskmgmt` | 189,312 | 106,028 |
| `sxgldiag` | 244,464 | 162,826 |

About 80 KB less per process, which is the toolkit, mapped once for all of them
and for the POSIX programs that use it. That is step 1's done criterion: a SUN
program declares `DT_NEEDED libsxgui.so.0.4` and carries no copy of the toolkit,
which `check_shared_libs` enforces as it does for POSIX. `taskmgr-smoke`,
`logview-smoke`, `diskmgmt-smoke` and the visual scenarios of all six run against
the mapped toolkit.

A program built against the old layout still loads. Its `libsxgfx.so.0.4` now
needs `libgfx2d.so.0.4`, which defines the primitives it used to, and the lookup
finds them there. Its `libsxgui.so.0.4` asks it for `sxgui_default_host`, which it
does not define; the reference is weak, so that program's toolkit runs without a
clock or a clipboard instead of failing to load. The clipboard's formats came
later as a table of their own behind a second weak symbol,
`sxgui_default_host_clipboard`, for the same reason
([PLATFORM_GRAPHICS.md](../desktop/PLATFORM_GRAPHICS.md#the-clipboards-formats)). Neither case has a test with a
binary actually built before the split: no external binary in this tree maps the
toolkit, and the weak half is asserted by `libweakref` in `ldfailtest`.

## The loader's failure paths

Every step `ldso_load` and `ldso_start` can fail at, how a test provokes it, and
the line it leaves in the system log. `ldfailtest.sxe` hands the loader files from
`/system/loader-fixtures`, which `tools/make_loader_fixtures.py` writes into the
test image byte by byte -- a linker refuses to produce most of them, and would
give the rest more than the one defect under test. Each failure must also give
its slot back, except `-8`, where the library itself loaded.

| step | cause | provoked by | test |
| --- | --- | --- | --- |
| `-1` | no free slot | `slottest`, built with 4 slots | `slottest` |
| `-2` | the file does not open | a path not in `/lib` | `missingtest` |
| `-3` | no section over the file | `empty.so`, 0 bytes | `ldfailtest` |
| `-4` | shorter than an ELF header | `short.so`, 10 bytes | `ldfailtest` |
| `-5` | not a usable shared object | `notelf.so` (no magic), `exec.so` (an `ET_EXEC`) | `ldfailtest` |
| `-6` | a segment does not map | `badsegment.so`, a `PT_LOAD` past the file's end | `ldfailtest` |
| `-7` | no dynamic symbol table | `nodynamic.so`, no `PT_DYNAMIC` | `ldfailtest` |
| `-8` | a dependency did not load | `needsgone.so`, needing a library not in `/lib` | `ldfailtest`, `needstest-missing` |
| `-9` | a symbol does not resolve | `libbroken.so.0.4` | `brokentest`, `dltest` |
| `-10` | the program's image is not registered | `ldimagetest`, built to fit one program header | `ldimagetest` |
| `-11` | the program has no dynamic symbol table | `ldstatictest`, STATIC with the loader linked | `ldstatictest` |

`-6` goes through the path where a writable segment's source range fails to
open after its anonymous section is mapped, which used to leave that view
mapped; the SXN rewrite of the loader unmaps it, and this is its test.

`-10` means `adopt_executable` refused the program's own image. In a real
program the base comes from the kernel in `r8` (`interptest` asserts it does),
and by the time a program could interfere crt0 has already run the loader, which
does not run twice. So `ldimagetest` is built with
`SAVANXP_LD_MAX_PROGRAM_HEADERS=1`, the way `slottest` lowers the slot count:
every PIE has more program headers than that, the image is refused, and the
program reaches `main` with no image registered and the step in the log.

One case in `ldfailtest` is the opposite of a failure: `libweakref.so.0.4` holds a
weak reference to a symbol nothing defines, and must load with it resolved to
zero, as ELF says. Before, the loader failed the whole load at `-9`. The toolkit
depends on it: `libsxgui` asks the program for `sxgui_default_host` weakly, so a
program built before the toolkit split, which defines none, still loads it.

## DT_SONAME resolution

A program linked against `libfoo.so.0.4` carries that name in `DT_NEEDED`. The
name is the library's version contract: the major (`0`) is the ABI, and the
minor (`4`) is the system's version, which a newer system raises without
breaking the ABI. The loader resolves it in this order:

1. **The exact file**, `/lib/libfoo.so.0.4`, when it is there. This is the
   startup path of every program, and it is unchanged: one open, no listing.
2. **Otherwise the highest newer minor** of the same major: `libfoo.so.0.6` over
   `libfoo.so.0.5`. Found by listing `/lib` (`SXN_SYS_OPEN_DIRECTORY`, open to
   every subsystem for this), only when the exact open failed with "not there".
3. **Never an older minor.** `libfoo.so.0.3` may lack a symbol a 0.4 program
   calls; the program starts without the library, as with any missing one, and
   `ldso_missing()` names `libfoo.so.0.4`, the name the program asked for.

A fallback is on record. When a full version is missing and a newer minor
stands in, the loader says so, once, and the line always reaches the system
log, whichever subsystem the program is and wherever its standard error goes:

```
loader: libfoo.so.0.4 is not in /lib; falling back to the newest, libfoo.so.0.6
```

It is the line that explains a change in behaviour after an update. The
runtime's `sx_loader_report` is what guarantees the log: the native one writes
there always (`SXN_SYS_LOG`); the POSIX one writes to standard error, which the
log records when it is the console, and also to the log when it is not
(`SXN_SYS_LOG`, opened to every subsystem for this and for `syslog(3)`).
`sonametest` reads the log back and finds the line exactly once with two images
needing the same missing 0.4; the `posix-loader-log` scenario closes a POSIX
program's standard error and finds it under the log's `posix[pid]` prefix.

A name that does not give the full version asks for the latest one, chosen
before anything is opened; that is what was asked, so it is not noted:

- **`libfoo.so`, no version:** the highest `libfoo.so.M.N` of any major --
  `1.3` over `0.7`.
- **`libfoo.so.0`, the major only:** the highest minor of that ABI -- `0.7`,
  and not `1.3`, which is another ABI.
- When `/lib` has no versioned file of it, the file named as given.

This tree's libraries always carry the full version in their `DT_SONAME`, so
the second rule is for what comes from outside: a port's library linked with a
bare name, or a `dlopen("libfoo.so")`.

Exact first, and not "always the highest", was a choice: nothing a program
already runs with changes because a newer file appears, and the cost of the
rule is paid only by a program whose library is missing. The consequence is
that an update which leaves the old minor in `/lib` keeps old programs on it;
taking a library away is how an update moves them on.

A library that stands in for another keeps its own name in the slot table, so a
second image needing `libfoo.so.0.4` finds it there after the same listing and
maps nothing twice. The rule applies to a `DT_NEEDED` given as a basename and to
a `dlopen` of a bare name (through `sx_loader_resolve`, so another subsystem's
interface gets the same rule), and not to an explicit path: `ldso_load` and
`dlopen` with a `/` load exactly what they are given.

`sonametest.sxe` covers both halves with libraries built at 0.3, 0.4, 0.5 and
0.6: the `sonametest` scenario, where the exact 0.4 wins over 0.5 and 0.6, and
`sonametest-newer`, which removes the 0.4 files so that 0.6 stands in -- once,
for two images that need it -- and the 0.3 of another library is refused. It
also needs `libsonamelatest.so` and `libsonamemajor.so.0`, each with 0.2, 0.7
and 1.3 beside the unversioned file, and asserts 1.3 and 0.7. `dltest` opens
`libsonamelatest.so`, `.so.0` and `.so.0.2` by bare name and gets 1.3, 0.7 and
0.2.

## Deliberately out of scope for the first implementation

Three things were designed, cost real time, and have been taken back out. Recording
them here rather than deleting them, because the reasoning is the part that survives.

**Moving the C runtime into `libc.so.0.4`.** It cannot move as one file. `crt0` runs
before a page of any library exists, so the stack canary, `__stack_chk_fail` and
`sx_start_dynamic` have to stay in the executable; and the raw syscall wrappers have
to stay too, because they are what the loader uses to map the first library. A
library cannot be asked to open the first library.

**Making `--export-dynamic` unnecessary.** It follows from the above: while the
syscall wrappers are in the executable, every library resolves `savanxp_*` against
the program, and the program has to export. Removing the flag needs a real `ld.so`
mapped before everything else — the loader living in the executable is the reason
the wrappers have to be there.

**A shared POSIX layer.** `posix.c` is 5477 lines and none of it has a bootstrap
reason to stay, so in principle it moves easily. It is a volume-size win, not a
runtime one: each process maps one copy either way. It also comes after the
loader stops being POSIX code; see
[The loader is POSIX's, and SUN is static](#the-loader-is-posixs-and-sun-is-static).

What was reverted along with them, and why it is not coming back:

- `crt0` passing the interpreter path and the image base as arguments instead of
  storing them in libc globals. Only needed if libc could be a library.
- `sxboot.c`, a file holding exactly what `crt0` touches. Its whole justification
  was "this cannot become a library", which stops being true.
- The loader no longer calling the C library. True and harmless, but nothing in this
  scope consumes it. (Since done, with SUN as the consumer: see
  [One loader over SXN](#one-loader-over-sxn).)

One thing from that work was kept, because it was never about libc: the runtime for
external applications is now derived from the runtime for the system's own programs
instead of being listed again by hand. The two lists have to agree, and when they
did not, the symptom was an undefined symbol much later than the change that caused
it.

## Explicitly not worth doing yet

- **`mprotect` and RELRO.** Neither is needed while binding is eager, and both need
  `mprotect`, which does not exist. RELRO only becomes worth revisiting after the
  loader lives outside the program, since a GOT nothing writes to needs no
  protection.
- **File demand paging.** A file-backed section is read whole. Sharing saves
  resident memory, not mapped memory, and `memory_bytes` counts mapped-present
  pages, so Task Manager shows the same per-process figure either way.
- **Unloading in `dlclose`.** `dlopen` arrived with the port that needs it,
  [Xash3D FWGS](../roadmaps/open/XASH3D_ROADMAP.md), as predicted here; unloading did not,
  because nothing needs it yet. See [`dlopen`](#dlopen).

## The loader is POSIX's, and SUN is static

> Status: **recorded 2026-11-13; steps 1 and 2 done 2026-11-14.** The loader
> is `system/loader/ldso.c`, shared by SUN and POSIX; the toolkit is split into
> neutral libraries and each subsystem's host
> ([The toolkit, split](#the-toolkit-split-into-neutral-and-host)), and the six
> SUN programs that paint map `libsxgui`. The table is how it stood when this
> was recorded; the SUN row now reads "the same loader, through `LINK_PROFILE
> PIE` and `DEPENDS`".

Shared libraries were a POSIX capability, not a system one:

| Program kind | How it gets code it does not carry |
| --- | --- |
| POSIX | `ldso.c`, compiled into every program that declares a `DT_NEEDED`; `crt0` calls `sx_start_dynamic()` before `main` |
| SUN | it does not: always static. `savanxp_program(NATIVE)` refuses the loader because `ldso.c` speaks POSIX, which a native process does not have |
| SSW (Win32) | its own PE loader, `subsystems/win32/loader/`, inside `winhost.sxe` |
| Linux (planned) | the kernel maps the image's real `PT_INTERP` ([roadmap](../roadmaps/open/LINUX_SUBSYSTEM_ROADMAP.md)) |

There is no `ld.so` file either. `/lib/ld.so.0.4` is the `PT_INTERP` every PIE
declares, and nothing is installed under that name: the interpreter is
`ldso.c` inside the executable ([crt0 runs the interpreter](#crt0-runs-the-interpreter)).

### Why this is a gap and not a detail

- **Parity.** A POSIX program shares `libsxgfx`, `libgfx2d`, `libsxgui` and
  `libmath`; a SUN program that paints with SXGUI-C links its own copy of all
  of them. By the parity rule in `AGENTS.md`, the gap closes in SUN.
- **The saving shrinks with every migration.** New programs are SUN, and POSIX
  programs move to SUN one at a time ([SUN.md](../userland/SUN.md)). Each move
  takes a program from the population that shares the runtime to the one that
  duplicates it, so the measurement at the top of this file gets worse as the
  system follows its own rules.

### Order

1. **One loader, written over SXN, linked by SUN and POSIX alike.** It is
   platform code and lives in `system/`, not in `subsystems/posix/`; POSIX
   becomes one of its consumers. The shape has a precedent: SSW's loader is
   portable C shared by the host harness and `winhost`. A real `ld.so` mapped
   before the program is the later form of the same thing, and the one that
   removes `--export-dynamic` and lets `libc` be a library
   ([out of scope](#deliberately-out-of-scope-for-the-first-implementation)).
   See [One loader over SXN](#one-loader-over-sxn) for what it needs.
   Done when a SUN program declares `DT_NEEDED libsxgui.so.0.4`, carries no copy
   of the toolkit, and `nativetest` asserts it. **Done (2026-11-14)**, asserted by
   `check_shared_libs` and the six programs' own tests rather than `nativetest`,
   which paints nothing: [The toolkit, split](#the-toolkit-split-into-neutral-and-host).
2. **`DT_SONAME` resolution.** Lookup of the highest `libfoo.so.0.*`
   ([Decisions taken](#decisions-taken)). Until it exists, a shared library ties
   every external binary in `/bin` to one OS version, which is exactly what the
   persistent image must not do. **Done (2026-11-14)**, ahead of step 1's last
   item: [DT_SONAME resolution](#dt_soname-resolution).
3. **Then, a POSIX client library.** With 1 and 2 in place POSIX becomes what a
   subsystem is in NT: a dispatcher in the kernel plus a client library the
   system loader maps, as SSW maps `kernel32`. Ideally `libc` and `posix`
   together, not `posix` alone with `libc` left in the executable. Doing it
   before step 1 makes the library depend on its own subsystem to be loaded.

### One loader over SXN

What `ldso.c` called before this work: `savanxp_open`, `savanxp_read` and
`savanxp_close` for the file; `section_open` and `section_open_range` for a
section **backed by the file**, plus `section_create`, `map_view_at` and
`unmap_view`; and from the C library, `eprintf` and `strcmp`. Items 1 to 4
below replaced every one of them.

What is already there. A POSIX process reaches the native object calls:
`native_call_open_to_all_subsystems` (`kernel/native_dispatch.cpp`) forwards
`SECTION_CREATE`, `MAP_VIEW_AT`, `UNMAP_VIEW`, `CLOSE` and `READ`, and handles
and descriptors share one space (`ldso.c` already closes sections with
`savanxp_close`). A loader over SXN runs inside a POSIX process as it is.

What is missing, in order of weight:

1. **A file-backed section in SXN.** SXN has `SECTION_CREATE`, anonymous, and
   no counterpart of `SAVANXP_SYS_SECTION_OPEN_RANGE`. That is why `winhost`
   reads a whole file and copies it into anonymous sections. A loader over SXN
   without it would load libraries dynamically and share nothing: the
   identity cache (`inode_id` + offset + length) is only reachable through the
   POSIX call. `SXN_SYS_SECTION_OPEN_RANGE` (file handle to section), open to
   all subsystems, is the slice; the loader is its consumer and `nativetest`
   its assertion. **Done (2026-11-14):** `sxn_section_open_range`, the same
   kernel function and cache as the POSIX call
   ([SECTIONS.md](SECTIONS.md#section_open-and-what-it-does-not-do-yet)).
2. **`SXN_SYS_OPEN_FILE` open to POSIX.** It is not in the forwarded list. One
   line, plus its `handletest` check. **Done (2026-11-14):** a POSIX process
   opens a file with `sxn_open_file`, reads and closes it as a descriptor, and
   backs a section with it; the path is absolute, since the native call knows
   no working directory.
3. **No C library in the loader.** `eprintf` and `strcmp` become the loader's
   own. This is the work that was reverted for lack of a consumer
   ([out of scope](#deliberately-out-of-scope-for-the-first-implementation));
   SUN is the consumer. **Done (2026-11-14):** `ldso.c` opens, reads and maps
   with `sxn_open_file`, `sxn_read`, `sxn_section_open_range`,
   `sxn_section_create`, `sxn_map_view_at`, `sxn_unmap_view` and `sxn_close`,
   all inline traps; it compares names and formats its messages itself, and
   copies structures with its own loop (`no_builtin`, or the compiler calls
   `memcpy`). `check_shared_libs` fails the build if a compiled copy imports
   anything else ([the guarantees](#the-guarantees-and-what-enforces-each)).
   A closed handle is -1, not 0: a native process with no standard input gets
   handle 0 from its first open.
4. **The runtime ends the process, not the loader.** `SXN_SYS_EXIT` and
   `SXN_SYS_LOG` are native-only, and that stays. (`SXN_SYS_LOG` did not: it
   opened to every subsystem on 2026-11-14, under POSIX's `syslog(3)`, so the
   loader's notes reach the system log from a POSIX process too.) The loader returns a status
   and a message; each subsystem's `crt0` reports it and exits in its own
   terms. `dlopen` and `dlfcn.h` are a thin POSIX layer over the common core;
   SUN names its own. **Done (2026-11-14):** the loader never ended the
   process, and still does not; what it had was `eprintf`, a POSIX write a
   native process would not have. Every line now goes to
   `sx_loader_report(text, length)`, a runtime hook like `sx_process_exit`:
   weak and POSIX in `libc.c` (standard error), strong in
   `system/runtime/sxn_runtime.c` (handle 2 through `SXN_SYS_WRITE`).
   `sx_start_dynamic` reports the failed step through it too. `dlopen` is the
   POSIX layer in two ways: `<dlfcn.h>`, and the working directory, since the
   core opens absolute paths only and `dlopen` joins a relative one with
   `getcwd` first.

   **Moved (2026-11-14).** The core is `system/loader/ldso.c`; `<dlfcn.h>` is
   `subsystems/posix/userland/dlfcn.c`, over the interface subsystems build
   on, `include/loader/loader.h` (`sx_loader_open`, `_find`, `_symbol`,
   `_address`, `_retain`, `_release`, `_make_global`, `_program`). The PIE
   profile puts the core in every PIE program and `dlfcn.c` only in POSIX
   ones; `savanxp_program(NATIVE)` takes `LINK_PROFILE PIE` and `DEPENDS`.
   `sunlibtest.sxe` is the consumer: a SUN program that maps `libmath`,
   asserted by the `sun-libtest` scenario. A SUN program started by the
   system has no handle 2, so the native `sx_loader_report` falls back to the
   system log, and a library that did not load still says so. A SUN program
   that paints still links its own toolkit until item 5.
5. **Libraries split into neutral and host.** `libsxgfx` carries `gfx.c`, the
   POSIX window client, and `libsxgui` carries `sxgui_app.c`, the POSIX
   application frame. The neutral parts (`gfx2d`, `sxchrome`, the `sxgui`
   core, `math`) resolve `memcpy` and `malloc` against the program, which both
   runtimes define, so one file serves both. The host part is per subsystem:
   the POSIX client on one side, `sxngui` on the other. **Done (2026-11-14):**
   [The toolkit, split](#the-toolkit-split-into-neutral-and-host).

### What SSW and Linux get from it

Neither runs the ELF loader: SSW maps PE images with its own loader, and a
Linux image's `PT_INTERP` is musl's, mapped by the kernel
([roadmap](../roadmaps/open/LINUX_SUBSYSTEM_ROADMAP.md)). What they share is
the kernel primitive, not the loader.

- **SSW.** `SXN_SYS_SECTION_OPEN_RANGE` lets `winhost` map the subsystem's own
  DLLs from the file instead of copying them into the native heap. They are
  built at Windows XP's bases and never relocate, so their code and read-only
  data can be one copy for every Win32 process. Two conditions: the file
  alignment has to be a page so a section maps at a page offset (`lld-link`
  writes `0x200` today; `/FILEALIGN:0x1000` fixes it), and the pages the loader
  writes, the import address tables, stay private copies as writable segments
  do here.
  A guest's own images keep the copy: they are loaded once, and their file
  alignment is not the subsystem's choice. `winhost` itself is a SUN program,
  so its 64-bit side gains the shared libraries like any other.
- **Linux.** Its step S2b needs private file mappings for musl's loader. A
  read-only file-backed range is the base of that, and gives one copy of
  `libc.so`'s text across Linux processes. Private writable mappings, partial
  unmap and `mprotect` are still S2b's own work: views cannot do them.

### Why a shared POSIX layer is not first

Measured on the current tree, `posix.c` is about 55 KB of `.text` and 369 KB
of `.bss`. Only the text could be shared; the `.bss` is per-process state and
stays one copy per process either way. The consumers that remain on POSIX in the
long run are ports and external programs (`doomgeneric`, `ccleste`, FFmpeg),
which link the `core` group statically through `runtime_units.sh` and are the
ones that most need a stable ABI, which step 2 provides and nothing provides
today.

What is worth doing before any of it is cheap: give the POSIX layer its own
group in `units.txt`, separate from `core`, with a declared export list a test
checks. That makes the subsystem's contract explicit, makes the POSIX/SUN
parity checkable, and is the export list a future `libposix` would ship.

## FFmpeg as one library, and what it cost to find out

`libffmpeg.so.0.4` is the first library the loader meets that it was not built
for. The six system libraries weigh 2 KiB to 136 KiB. This one is 6.8 MB on
disk, exports 2334 symbols and carries 9228 relocations. Everything below came
out of building it and failing to load it.

### One thing the loader was missing, found by this library

**`R_X86_64_64` was not implemented.** A data slot holding the address of an
*imported* symbol — `&func` stored in a table — needs a runtime resolution, and
the loader aborted the load on it. There are 253 of them. It is the same work as
`R_X86_64_GLOB_DAT`: look the name up, write the value. The diagnostic added in
the earlier phase named the type on its own, which is how it was found.

### Symbol lookup was tried and rejected

`resolve` walks the whole `.dynsym` with `strcmp`, once per relocation needing a
name. With FFmpeg that is 2029 lookups against 2334 symbols — about 4.7 million
string comparisons, which reads like the obvious bottleneck.

It was implemented and measured. `lld` writes a SysV `DT_HASH` table into
`libffmpeg.so.0.4`, so the loader was given one to use:

| | ms |
| --- | --- |
| load resolving through `DT_HASH` | 3585 |
| load with the hash path disabled, linear scan forced | 3577 |

Noise. The load is not CPU-bound on lookup, so `DT_HASH` was removed again and
`resolve` still walks the table. Two further facts killed it:

- **No library in the tree emits SysV `DT_HASH`.** All six system libraries carry
  `DT_GNU_HASH`; only the port-built FFmpeg library has the SysV table. So the
  change would have applied to one library out of thirteen.
- **A `DT_GNU_HASH` implementation would not have helped either**, for the same
  reason the SysV one did not. That is the whole reason to leave the scan alone:
  the measurement says lookup is not what costs, so implementing either hash
  format buys nothing that the measurement can see.

Phase timing inside `ldso_load` says where the time actually goes:

| phase | ms |
| --- | --- |
| place segments | 1015 |
| dynamic table | 1 |
| `DT_NEEDED` chain | 0 |
| 9228 relocations | 1226 |

Two halves, and neither is the algorithm. Placing the segments reads 6.8 MB and
allocates 17.25 MB of BSS. The relocations are 133 µs each, which is absurd for
a header scan and an 8-byte store: they are first-touch page faults spread over
18 MB. Opening and mapping the file, by contrast, is 3 ms — `map_view_at` is
lazy, so the read cost lands inside the load, not before it.

The kernel's per-page `memset` in `allocate_section` was the obvious suspect and
is not: removing it made the load *slower* (4059 ms), because the zeroing only
moves to the first-touch fault handler. It stays — the free-page list hands back
dirty memory, and skipping it would leak stale bytes across processes.

### The 17.25 MB of BSS is FFmpeg's, not the link's

The fourth `PT_LOAD` is `filesz` 13.5 KiB and `memsz` 17.25 MB. Across the five
archives, 17 MB of BSS of which **16 MB is FFT tables** — `ff_tx_tab_*`, every
transform size in every precision.

The obvious suspect is `--whole-archive`, which does pull in every codec and
every table. It was measured: linking with the 48 FFmpeg entry points the player
actually uses as `-u` roots, and letting archive semantics pull the transitive
closure, gives the same 6.8 MB and the same 17.99 MB of BSS. The codec registry
is one table that references every decoder, so the closure genuinely reaches the
transform tables. `--whole-archive` costs nothing and is the more robust link,
so it stays.

The consequence is a user-visible one: loading FFmpeg maps about 25 MB, and the
library's resident footprint is more than triple its size on disk. Anything that
wants that down has to change FFmpeg's build configuration, not our loader.

### The link produced an ET_EXEC, and the guard caught it

`clang -target x86_64-unknown-none-elf -nostdlib -shared` warns
`argument unused during compilation: '-shared'`, ignores the flag, and links an
`ET_EXEC` with a library's name on it. `read_header` rejected it by type, which
is exactly what it is there for. `-Wl,-shared` is the form the driver honours.

### What this says about the loader

Nothing measured here argues for optimising `ldso` further. The linear scan cost
nothing against a memory-bound load, and the memory is FFmpeg's. What the phase
timing *does* say is that the loader's cost is proportional to bytes mapped, so
the lever is the image, not the algorithm — and the image is an FFmpeg build
decision.

### Four test programs that nothing ran

`brokentest`, `missingtest`, `slottest` and `diamondtest` are built, staged into
`/bin`, and exercise the failure paths of the loader: an unresolved symbol, a
missing library, a full slot table, and a diamond of four libraries. **No smoke
scenario ran any of them**, and the reason is worth writing down because it is not
obvious from the outside.

They had no success token. Every assertion in the four is an `eprintf` on the way
out, and `return 0` at the end. So there was nothing for a scenario to look for:
the only string they could produce was the text of a failure. They had been
validated by breaking the loader on purpose, which proves a diagnostic once — the
moment you made the break — and not twice.

A program that only speaks when something is wrong cannot be checked that
something is right. Each of the four now prints its token and has a scenario, and
each was verified by disabling the exact loader logic it tests:

| scenario | what it disables | result |
| --- | --- | --- |
| `brokentest` | `report_unresolved` stops naming the symbol | stops passing |
| `missingtest` | a failed load leaves its slot occupied (`end_load(1)`) | stops passing |
| `slottest` | the `kMaxLibraries` cap in `begin_load` | stops passing |
| `diamondtest` | `already_loaded`, so the shared leaf loads twice | stops passing |

`slottest` earned its keep during that check. The first attempt to disable it
replaced the cap in `adopt_executable()` instead of the one in `begin_load()`, and
the scenario passed — which proved nothing except that the cap in `begin_load`
works. A check that passes when you break something else is not yet a check.

## The first application with a library, and the two bugs it found

`mediaplayer` is the first program in the tree that depends on a shared library.
It is still built by the FFmpeg port, not by CMake — its engine is FFmpeg, and a
system must work without FFmpeg installed — but it is a PIE with one
`DT_NEEDED`, and it went from 7 MB with the archives linked in to 344 KB.

Two things came out of that, and neither was visible from the test programs.

### The first FFmpeg call was before the first FFmpeg call's guard

The player was told to check `ldso_missing()` before touching FFmpeg, and the
check went into `media_open`, which is where every `av_*` call was believed to
live. It did not work, and the way it failed is worth writing down:

```
loader: no se pudo abrir /lib/libffmpeg.so.0.4 (paso 2)
sx_start_dynamic: el interprete fallo (paso 8)
user: exception #14 name=mediaplayer-ffmpeg cr2=0x4b6d6 rip=0x4b6d6
```

`0x4b6d6` is `av_log_set_level@plt + 6`, and `main` calls `av_log_set_level` as
its very first statement — before the `--selftest` dispatch, before any window,
before `media_open`. So the process jumped through an unrelocated GOT slot into
an address below `kUserBase` and died. The check was correct and in the wrong
place. It is now the first thing in `main`, and `media_missing_library()` in
`media.c` is the single place that answers the question.

**The general lesson:** "before the first call" is not a place you can pick. It is
the earliest statement of `main`, and you only find out where that is when a
dependency is missing.

### A missing dependency left the program unrelocated, so it could not report it

With the library gone, the program printed *nothing*. The loader had reported the
problem, and then `main` ran and could not even say so.

The cause was the order in `ldso_start`. The `DT_NEEDED` chain is walked before
relocations, because a GOT entry pointing at a dependency can only be filled once
that dependency is mapped. But the chain's failure returned immediately, so
`apply_relocs_in(0)` never ran for the executable. Every `R_X86_64_RELATIVE` in
the program stayed at its link-time value — including `stdout`, which is
`FILE* stdout = &g_stdout_file`. `printf` wrote through a pointer into nowhere
and the message was lost. It did not fault; that was luck.

So a failed chain now still relocates, and relocations are **tolerant**: entries
that cannot be resolved are skipped and counted, rather than aborting the table on
the first one. Aborting was what left the image half-built:

```
loader: 39 entradas de el ejecutable se quedaron sin reubicar porque libffmpeg.so.0.4 no cargo
mediaplayer: falta libffmpeg.so.0.4, y sin ella no hay motor de reproduccion
```

39 is the count of `DT_JMPREL` entries, and it matches. That count is worth
having: it says how much of the program is not working, and the name alone does
not.

The load stays non-fatal, which is what makes any of this reachable: a program
that cannot load a dependency still has to be able to say so. "Non-fatal" was
never allowed to mean "half-loaded".

`image_name()` exists because the executable has no `DT_SONAME` — that field is
for shared objects only — and the message came out as `39 entradas de  se
quedaron`. The image's name is the first thing anyone reads in a diagnostic.

### A missing library took every other one with it

Moving the Media Player into the tree made it depend on five libraries instead of
two, and one of them going missing took the other four.

`load_needed_chain` returned at the first failure. An executable's `DT_NEEDED` is
a *list*, not a chain: if `libffmpeg.so.0.4` is absent that is a fact about
`libffmpeg.so.0.4` and says nothing about `libmath`, `libsxgfx`, `libgfx2d` or
`libsxgui`, which are later in the list and are needed just as much. Returning
meant none of them were ever loaded.

The symptom was the worst kind. `mediaplayer` passed its `ldso_missing()` check —
correctly, `libffmpeg.so.0.4` really was the missing one — went on to build its
window, and died on `sx_rect_make@plt`:

```
loader: no se pudo abrir /lib/libffmpeg.so.0.4 (paso 2)
user: exception #14 name=mediaplayer cr2=0x2c036
```

`sx_rect_make` is SxGUI, not FFmpeg, so nothing in the log connects the cause to
the address. **And this was my own doing.** Before the tolerant-relocation change,
an unresolvable symbol aborted the table and the failure was loud. Tolerance plus
an early return is the combination that turns a loud failure into a silent one
somewhere unrelated. Neither change is wrong alone; together they hid the real
problem behind a plausible-looking crash.

The walk now records the failure and continues, and returns non-zero afterwards so
`ldso_missing()` and `sx_start_dynamic` still report it. The difference is between
"libffmpeg is missing" and "nothing is there":

```
loader: no se pudo abrir /lib/libffmpeg.so.0.4 (paso 2)
loader: 39 entradas de el ejecutable se quedaron sin reubicar porque libffmpeg.so.0.4 no cargo
```

39, and not 74, and not everything. The count is what tells the two apart.

### A library has to declare the libraries it needs

`libffmpeg.so.0.4` had no `DT_NEEDED` at all, and `mediaplayer`'s list put it
first:

```
libffmpeg.so.0.4   libmath.so.0.4   libsxgui.so.0.4   libgfx2d.so.0.4   libsxgfx.so.0.4
```

The loader relocates each library as soon as it brings it, so `libffmpeg`'s 42
double-precision references had nowhere to resolve and the load failed with
*"libffmpeg.so.0.4 necesita 'fabs' y no esta en ninguna imagen cargada"*. It never
showed up before because the port's player carried `math.c` **statically** and so
had no `libmath` to be missing.

The order of an executable's `DT_NEEDED` is a build artefact nobody reads, so a
library that uses another has to say so itself — which is the only thing that makes
the loader bring the chain first and relocate after. `libffmpeg.so.0.4` now carries
`libmath.so.0.4`, and the port's link needs `-Wl,-Bdynamic` because clang defaults
to a static link for the `none` triple.

Putting `libmath` earlier in the program's `DEPENDS` would also have worked. That
is the wrong fix: it depends on a list nobody reads, and the next reorder breaks it
silently.

### The loader is not part of `libsavanxp.a`, on purpose

The port links `ldso.c` separately. `crt0` calls `sx_start_dynamic`, which in
`libc.c` checks whether the weak symbol `sx_run_interpreter` is defined; only
`ldso.c` defines it. Leave it out of the link and the check yields zero, the
interpreter never runs, and `DT_NEEDED` is ignored in silence — the program starts
cleanly and the library's functions do not exist. It is the quietest possible
failure, which is why it is worth a paragraph.

It stays out of `libsavanxp.a` because in the archive every static program of the
port would carry and run the loader's entry path for nothing.

### `ldso.h` moved into the SDK

`ldso_missing()` is not an internal detail: an application that depends on a
library has to be able to ask whether it loaded. The header moved from
`subsystems/posix/userland/` to `subsystems/posix/sdk/v1/include/savanxp/`, and
the port picks it up through the sysroot it already builds. The implementation
is compiled into each program rather than linked once; it lives in
`system/loader/` since it became the loader of SUN programs too.

### What the Media Player does with the answer

`--selftest`, `--probe` and `--gpu-hold` do not open a window, so they print
`mediaplayer: falta libffmpeg.so.0.4, y sin ella no hay motor de reproduccion`
and exit 2. The window path opens anyway with the message painted in it: that is
the one place a person is looking, and there is no console to fall back on. The
scenario is `mediaplayer-nolib`, and it was checked against a build with the fix
reverted, where it times out instead of finding the message.

### The FFmpeg work tree moved into the build directory

`ports/ffmpeg/env.sh` defaulted `WORK` to `$HOME/savanxp-ffmpeg` while
`ports/ccleste/env.sh` used `$OUTPUT_ROOT/ports/ccleste/work`. Both ports were
added on the same day and picked different conventions; the FFmpeg one was copied
verbatim from where it lived before it entered the tree. It now points into
`OUTPUT_ROOT`.

Worth knowing if you move a port work tree: FFmpeg's `configure` only creates the
build directory's top-level `Makefile` when it is missing
(`test -e Makefile || echo "include $source_path/Makefile" > Makefile`), so a
stale one keeps pointing at the old absolute source path and `make` fails with
`No hay ninguna regla para construir el objetivo`. Deleting it and re-running
`configure` is the fix. The freshly generated one says `include src/Makefile`,
which is relative and relocatable.

## The launcher entry, and the third stale claim

Registering the player was supposed to be one table row. It was not, because three
places in the tree described a `/bin/mediaplayer` that had never existed — or had
been deleted.

`26b16e0` removed `subsystems/posix/userland/mediaplayer.c` together with SxMedia,
and said why: *"the base system launcher gone with it"*. A launcher that delegates
to another program is pointless when nothing can be installed that is not itself a
program, because the decoder has to be inside the binary and then there is exactly
one binary and nothing to delegate to. The commit also named what would have to
exist first: *"a way for one program to use a codec library it was not built with,
by dynamic linking"*. That is what `libffmpeg.so.0.4` is, so the launcher became
meaningful again and was restored rather than redesigned.

**And then it was deleted again on purpose.** The player moved into the tree and
the port stopped building a program, so there was nothing left to hand over. The
launcher had one job beyond delegation — say the port is missing instead of
vanishing from the launcher — and the player does that itself, from `main`, with
`ldso_missing()`. Restoring it was the cheapest way to make three descriptions true
at once; keeping it was never the destination. What follows is why it existed.

What was still standing from the original design:

- a **closed** `CHANGELOG.md` section: *"`/bin/mediaplayer` is always built,
  delegates to `/bin/mediaplayer-ffmpeg` when installed, and otherwise opens an
  in-OS warning"*
- `docs/media/MEDIA_PLAYER.md`: *"`/bin/mediaplayer` is always built into the system image
  and is the program shown by the launcher"*
- the port's `.sxres`: *"The stable launcher (/bin/mediaplayer) owns the visible
  Accessories entry"*

None of it was true, and none of it was wrong on the merits. Restoring the file
made all three true at once, which is the cheapest possible fix for a documentation
lie: the documentation was the spec.

### The name of a missing library was arbitrary

`ldso_missing()` reported whichever missing library failed **last**, so the same
image with the same three files absent could name three different libraries
depending on the order of `DT_NEEDED` and on what else had recursed. It is now the
**first** one that failed — a definition rather than an accident of the walk — and
there is a count beside it.

The count took three tries to get right, and the wrong versions are the interesting
part:

- Counting failed load **attempts** gave 6 for 2 missing files. Removing
  `libsxgfx.so.0.4` makes the load of `libgfx2d` and `libsxgui` fail as well,
  because all three need it, and each retries. A number that lies is worse than no
  number, so it counts **distinct** names now, deduplicated.
- Distinct names gave 4 for the same 2 files, which is still not "4 files are
  missing". Four libraries could not be loaded; two files are absent. So the count
  means *libraries that failed to load*, the header says so, and the player says
  "no se pudieron cargar 4 librerias" rather than "faltan 4", which would be false.

What the loader can honestly answer is *how much of the program is not working*,
not *which files you forgot to copy*. The message in the window:

```
no se pudieron cargar 4 librerias; la primera es libffmpeg.so.0.4
```

### Bus-master DMA: attempted, measured, reverted

The other half of this was the PIO data phase. `rw_chunk` delivers one sector per
backend request and reaches it through the data port; the reasoning and the numbers
are in [`SXFS_ROADMAP.md`](../roadmaps/open/SXFS_ROADMAP.md) item 1, which concludes that DMA is the
only thing that removes the per-sector cost.

It was written — descriptor table in kernel-contiguous pages, the BM registers, a
PIO fallback so it could not break anything — and it **broke the volume**: the
guest could no longer `savanxp_open` anything, so no library loaded and the kernel
crashed at the first unrelated call. Two attempts, both reverted.

The first waited for BM status bit 0 to go 1 and then 0, and that is a race: the
device starts the DMA when it processes the ATA command, so a status read issued
early says *idle* and the copy comes out garbage. The second waited for BSY on the
ATA register instead, which is unambiguous, and still returned short data.

What it needs next is the completion signal the PIIX bus master actually defines
for this case — IRQ pending, bit 4, not the active bit — and a coherency argument
for a buffer the CPU wrote and the device reads. Neither was verified, so nothing
was committed. A block layer that silently returns wrong bytes is worse than a slow
one, and the 26% already banked from 32-bit accesses is safe.

### The launcher was 114 lines

**Deleted.** The player moved into the tree and the port stopped building a
program, so there was nothing left to delegate to. What follows describes what it
was and why, not what is there — it is kept because the reasoning is what decided
that the player itself checks `ldso_missing()`, and because "it was removed twice"
is a thing worth being able to check against.

It was `port_is_installed()` with `savanxp_stat`, then `exec()` with `argv[0]`
replaced.
No `fork`, no pipe, no polling: the process image is replaced, so the child's exit
code is the launcher's. Without the port it runs `sxgui_app_run` on a two-widget
window and says so.

The `--availability` probe that used to be in it is gone, with the two scenarios
that used it — the launcher now has a real behaviour to assert instead.

It is deliberately linked against `libgfx2d`/`libsxgfx`/`libsxgui` and **not**
`libffmpeg`: it does not use it and cannot, because whether it is in the volume is
the question it exists to answer.

### Where it goes in the launcher, and why it was not its own group

`tools/shoot_session.py` had an unused `open_mediaplayer()` doing `launch(2)`,
with a comment documenting the catalog as *Accessories: Calculator, Files, [Media
Player], Notepad, Shell*. That index only holds if Media Player is in the same
group, because the launcher sorts alphabetically inside each group. So it went into
the first baked group, not into a group of its own — a one-icon group would have put
that position somewhere else and broken the helper.

A third inconsistency turned up here and was **not** fixed, because fixing it moves
every application between groups. The baked table in `progman_registry.c` uses
`Main`/`Games`/`Diagnostics`; every `.sxres` declares `Accessories`/`System`/
`Games`/`Diagnostics`; `shoot_session.py` documents the manifest names. Once the
catalog is rebuilt by scanning, the manifests win and the groups become the
documented ones — the baked table is only a fallback. But the fallback disagrees
with every manifest, and `calc`, `filesapp`, `notepad` and `shellapp` all declare
`category=Accessories` while the table calls it `Main`.

### The baked table had drifted from every manifest

`progman_registry.c` carries its own list of programs, and it disagreed with the
`.sxres` files about which groups exist: the table said `Main`/`Games`/
`Diagnostics`, all fourteen manifests said `Accessories`/`System`/`Games`/
`Diagnostics`. It survived because the real catalog is rebuilt by scanning the
installed binaries and reading their stamped `category=`, so the table is only a
fallback — and a fallback nobody exercises disagrees with the real thing quietly.

It also disagreed on **order**, which was not cosmetic. The scan sorts groups
alphabetically, and the launcher's tabs are the group index, so
`tools/shoot_session.py`'s "System is always the fourth tab" was only true of the
scan. The table listed Main first.

Both lists are now the same, in the same order, and `progman-smoke` asserts it:
every group in the baked table must exist in the scanned catalog. It does not
compare the *items*, and deliberately so — the scan also finds what the ports
install (Celeste, Doom) and a baked table cannot know that. The groups it can
compare, because the whole system declares them and nothing adds any.

`Add/Remove Programs` was missing from the table entirely, even though
`appwiz.sxres` declares `category=System`. Adding it is what made the counts line
up: Accessories 5, Diagnostics 5, Games 2, System 3.

### The icon travels with the manifest

`progman-smoke`'s scan prints `icono=propio` or `icono=horneado`. With no `icon=`
in the manifest, Media Player came out the only `horneado` in the catalog — the
generic desktop icon. `icon_file` resolves *relative to the manifest*, so a copy
was once staged as `subsystems/posix/userland/mediaplayer-icon.png`; once the player
moved into the tree, `mediaplayer.sxres` simply names `icon.png` next to it, and the
copy went away.

### The scenario proves the entry, not just the file

`mediaplayer-noport` removes the **library** and drives the player from the taskbar.
It used to remove the backend binary and check the launcher's window instead, and
the move to the tree changed what it has to remove: there is no backend binary
anymore, and the window that says so is the player's own.

It asserts the player's own text, not the taskbar caption: the caption is 14 pixels
at the current font and detects no font change at all (measured, and written down in
`scenario_taskbar`), whereas the notice is the only thing the player draws when it
has no engine.

Three things had to be got right for that assertion, and all three were wrong first
time, which is the usual ratio:

- the notice is **red on black** (`gfx_rgb(230, 120, 110)` over the video area),
  not the toolkit's `TEXT` on `FIELD`. With `TEXT`/`FIELD` the search finds 0 of 335.
- a `/` does not match the glyph table — 434 of 436 pixels — so the search string
  avoids slashes.
- the search string is the library name and the phrase after it, and both had to be
  re-picked when the wording moved from the launcher's to the player's.

With the player patched to `return 1` instead of showing the window, the scenario
finds 0 pixels.

`file_assoc` gained three assertions for the same reason: the `ext_open` list in
`mediaplayer.sxres` is a claim, and `.mp3`, `.avi` and `.flac` now have to resolve to
`/bin/mediaplayer` against the stamped binaries in the real image — and to the
player, which is a program of the tree, rather than to anything the port installs.

## dlopen

`<dlfcn.h>` is implemented in `subsystems/posix/userland/dlfcn.c`, over the
loader's interface (`include/loader/loader.h`) and so on the same slot table as
the startup loader. It exists in every POSIX PIE program and in no STATIC one — where a call is a
link error rather than a silent no-op. It was built for
[Xash3D FWGS](../roadmaps/open/XASH3D_ROADMAP.md), whose engine loads its renderer, menu,
client and game logic by name, and its shape is what that port calls:
`dlopen(path, RTLD_NOW)`, `dlsym` through the handle's dependencies, `dlclose`,
`dlerror`, `dladdr` for save/restore, and constructors for its C++ modules.
`dltest` covers each of those, and was checked by breaking the scope rule and the
rollback.

### Scope: the one semantic change

Before `dlopen`, every image was in one namespace, searched newest first with the
executable last. That stays the **global scope**, and everything loaded at startup or
through `ldso_load` is in it, so nothing that existed changes behaviour.

A library opened without `RTLD_GLOBAL` is **local**: it answers only for itself and
for images whose dependency tree contains it. A relocation of image *S* searches the
global images plus *S*'s own tree (`Library.deps`, one bit per slot, closed
transitively), in the same newest-first order. The case that requires it is Xash3D's:
`client.so` and `hl.so` both define the HLSDK's `PM_*` functions, and with one
namespace the second library loaded binds its calls into the first one's copy.
`dltest` reproduces that with `libdlmod_one` and `libdlmod_base`.

- `dlsym(handle, name)` searches the image, then its dependency tree in load order.
- `dlsym(RTLD_DEFAULT, …)` and `dlsym(dlopen(NULL, …), …)` search the global scope.
- `RTLD_GLOBAL` — at load, or later through `RTLD_NOLOAD` — makes the image and its
  whole tree global, as on Linux.

### A failed `dlopen` leaves nothing behind

`ldso_load` publishes an image before walking its `DT_NEEDED` and relocating it,
which is right at startup and wrong for `dlopen`: a library that failed would stay
mapped and answer `dlsym(RTLD_DEFAULT)`. So `dlopen` rolls the table back to where it
was — unmapping every image the call added — and puts back `ldso_missing()`'s state,
which describes the program's startup and is not a `dlopen`'s to change.

It also runs **strict**. At startup, once a dependency is missing, relocation turns
tolerant and skips what cannot resolve, so the program can start and say what is
missing. A `dlopen` caller has `dlerror()` for that, so an unresolvable symbol fails
the load and `dlerror()` names it.

### `dladdr` needed the symbol count

`.dynsym` has no length field. `resolve_in` used to stop at the first entry whose
name offset fell outside `.dynstr`, which works because what follows the table
usually looks like that. A `dlsym` that misses — and Xash3D probes for optional
exports on every load — walks into that guess, and `dladdr` has to scan every symbol.
The count now comes from `DT_HASH` (its second word) or, for every library this tree
builds, from `DT_GNU_HASH` (the highest bucket start followed down its chain to the
end bit). That is the only use of either table: lookup stays a linear scan, for the
reason in [Symbol lookup was tried and rejected](#symbol-lookup-was-tried-and-rejected).
The count was checked against `readelf` for every library and two executables.

### What `dlopen` deliberately does not do

- **`dlclose` never unmaps.** It drops a reference; a later `dlopen` of the same path
  returns the same image with its data as it was left. musl behaves the same, and
  Xash3D supports musl. Unloading means unmapping, invalidating every pointer into the
  image, running destructors, and freeing a slot in the middle of an ordered table —
  real work with no consumer yet.
- **No lazy binding.** `RTLD_LAZY` is `RTLD_NOW`, as at startup.
- **One path, one image.** Identity is the path string `dlopen` was given, after
  prefixing `/lib/` to a bare name. Two spellings of one file load it twice.
- **Constructors run for `dlopen` only.** `DT_INIT` then `DT_INIT_ARRAY`, for every
  image the call brought, newest first so dependencies initialize before what needs
  them; there are no destructors, since nothing unloads. Nothing at startup runs
  constructors — not `crt0` for the program, not the loader for its `DT_NEEDED` —
  because nothing in this tree has any. Xash3D's `client.so` and `hl.so` are C++
  with global objects, which is why `dlopen` cannot wait for that.
- **The budget is shared with startup.** 32 slots and the per-process limit of 64
  section views ([SECTIONS.md](./SECTIONS.md#the-budget)), at roughly one view per
  `PT_LOAD`. Xash3D's half-dozen modules fit; a port that loads dozens would not.

### The descriptor that stayed open

Writing the rollback found a leak older than `dlopen`: `place_segment` closed the
section handle of a data segment after mapping it, but not of an executable one. A
view holds its own reference to its section, so the handle served nothing, and every
process kept one descriptor per library — out of 64 — for its whole life. It is
closed now, and a load that fails after mapping its segments unmaps them.
