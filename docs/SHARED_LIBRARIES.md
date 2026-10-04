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
| `DT_NEEDED` walked from `/disk/lib`, each dependency loaded once | done |
| A two-library dependency chain, proven by `libchaintop` → `libchainbase` | done |
| A diamond in the dependency graph | handled, **not tested** |
| `ldso_lookup()`, `ldso_loaded()`, `ldso_symbol_is_shared()` | done |
| 32 library slots, 24 program headers per image | done |

### The libraries

372 KB total, six files. The layering is strictly downward: nothing depends on
anything above it.

| library | size | `DT_NEEDED` |
| --- | --- | --- |
| `libmath.so.0.4` | 44 KB | — |
| `libsxgfx.so.0.4` | 100 KB | — (syscalls only) |
| `libgfx2d.so.0.4` | 92 KB | `libsxgfx` |
| `libsxgui.so.0.4` | 136 KB | `libgfx2d`, `libsxgfx` |
| `libchainbase.so.0.4`, `libchaintop.so.0.4` | 4 KB each | each other (test chain) |

`libffmpeg.so.0.4` is not in this table because the tree does not build it: the
FFmpeg port links it and stages it at `/disk/lib`, so it exists only in an image
that has the port installed. It is two orders of magnitude larger than anything
above, and [its section](#ffmpeg-as-one-library-and-what-it-cost-to-find-out)
records what loading it exposed.

### The programs

82 executables: 30 are `ET_DYN`, 27 declare a `DT_NEEDED`. By library:
`libsxgfx` 20, `libgfx2d` 15, `libsxgui` 10, `libmath` 2.

No library asks the application for a single symbol. What each still needs from the
executable:

| library | asks the executable for |
| --- | --- |
| `libmath` | nothing |
| `libgfx2d` | `malloc` `free` `realloc` `memcmp` `memset` |
| `libsxgui` | `clipboard_*` `memmove` `memset` `puts_fd` `savanxp_close` `sleep_ms` `uptime_ms` |
| `libsxgfx` | syscalls and libc |

All of those live in the runtime, not in the application. That is what removes the
old constraint — a program no longer has to be PIE in order for a toolkit to find
its drawing code.

### The guarantees, and what enforces each

| guarantee | enforced by |
| --- | --- |
| No executable carries a private copy of a library it maps | `tools/check_shared_libs.py`, at the end of every `./build.sh build`, exit 1 |
| The loader never calls the C library | `ldso.c`'s undefined list: zero `libc` entries |
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
`verify_doom_persistence.sh`, and 18 visual scenarios in `shoot_session`.

### Limits that are part of the current design, not bugs

- **No lazy binding, deliberately.** The loader applies `DT_JMPREL` at load, like
  `BIND_NOW` and without reading it. It costs a little work per library at startup
  and buys the property that a broken dependency is a startup failure with a name
  rather than a crash at whichever call site happened to be first.
- **`--export-dynamic` is still required.** The raw syscall wrappers stay in the
  executable because the loader needs them to map the first library, so every
  library resolves `savanxp_*` against the program.
- **A non-PIE program cannot use a library.** An `ET_EXEC` has no `PT_DYNAMIC`, so
  it has nothing to declare a `DT_NEEDED` with. This is a property of the link
  model, not of the loader.
- **`DT_SONAME` is read and ignored**; a `DT_NEEDED` is looked up verbatim under
  `/disk/lib`.
- **Global visibility only.** No `STV_HIDDEN`, `STV_PROTECTED`, or `.symver`.
- **No `dlopen`, no unload, no reference counting.** The library array is fixed at
  load time.
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
| `libsxgfx.so.0.4` | `gfx_impl.inc` and three font tables | libc, syscalls |
| `libgfx2d.so.0.4` | `gfx2d.c`, `sxchrome.c` | `libsxgfx`, libc |
| `libsxgui.so.0.4` | `sxgui.c`, `sxgui_app.c` | `libgfx2d`, `libsxgfx`, libc |
| `libmath.so.0.4` | `math.c` | nothing at all |

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
depends on `${SAVANXP_USER_TARGETS}` and `savanxp-busybox` directly.

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

- **`DT_SONAME` and visibility.** A `DT_NEEDED` is looked up verbatim by filename,
  and only global visibility exists: no `STV_HIDDEN`, `STV_PROTECTED`, `.symver`.
  Nothing in this tree needs them, and nothing that runs on it needs them either.
- **Half the loader's failure paths are untested.** Eleven return codes; four have a
  self-test. `-3` (a section that will not open), `-4`, `-5` (a file that is not a
  usable ELF — the message exists, nothing produces it), `-6`, `-7`, `-8` (a
  dependency's own chain failed) and `-10`/`-11` are all unexercised.
- **A non-PIE program silently cannot use a library.** Not a crash: it links, runs,
  and its libraries fail to resolve. Nothing detects it at build time.

### Phase 0 — make failure diagnosable

A library that cannot be resolved reports a step number and nothing else, and
nothing at all is tested for a library that is missing, one that exceeds the slot
count, or one with an unresolvable symbol. `ldtest` covers a missing *symbol* through
`ldso_lookup`, which is a different thing.

**Done.** Five items, all in the smoke catalog, each verified by breaking it:

| case | how it is provoked | what it asserts |
| --- | --- | --- |
| unresolved symbol | `libbroken.so.0.4` calls a symbol nothing defines | `g_lib_fail_symbol` and `g_lib_fail_library` name it |
| missing library | `ldso_load` on a path that is not in `/disk/lib` | step `-2`, no slot consumed, and the path really does not open |
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

### Phase 3 — FFmpeg

The reason any of this exists, and the phase nothing above delivers on its own.

`ports/ffmpeg/configure.sh` still passes `--disable-shared --enable-static`. The work
is to build it shared, declare it, and have `mediaplayer` use it.

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
loader: no se pudo abrir /disk/lib/libneeded.so.0.4 (paso 2)
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

So an installed third-party library lands in `/disk/lib` beside the system's own and
there is no way to take it out again, short of rebuilding the image. For `libffmpeg`
that is the arrangement: it is part of the system once installed, and the
application that uses it is a system application that cannot be removed either.

One consequence follows from the flat layout and is worth naming before a third-party
library arrives. Resolution is one directory and an exact filename — `/disk/lib/`
plus the `DT_NEEDED` string — so `libffmpeg.so.0.4` and `libsxgui.so.0.4` share one
namespace, and a library installed by a third party can shadow a system's. There is
no SONAME matching to keep them apart, which is the same limitation already recorded
above and now has a practical consequence rather than only a theoretical one.

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

**A shared POSIX layer.** `posix.c` is 5046 lines and none of it has a bootstrap
reason to stay, so in principle it moves easily. It is a volume-size win, not a
runtime one: each process maps one copy either way.

What was reverted along with them, and why it is not coming back:

- `crt0` passing the interpreter path and the image base as arguments instead of
  storing them in libc globals. Only needed if libc could be a library.
- `sxboot.c`, a file holding exactly what `crt0` touches. Its whole justification
  was "this cannot become a library", which stops being true.
- The loader no longer calling the C library. True and harmless, but nothing in this
  scope consumes it.

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
- **`dlopen` as a general feature.** Not on the critical path: this FFmpeg port
  compiles its decoders into `libavcodec` and enables no external codec library, so
  nothing is looked up by name at runtime. If a port ever does enable one, `dlopen`
  arrives with it — a general `dlopen` with reference counting and unloading is a
  much larger feature, and building that first would put a second subsystem between
  here and a working player.

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

### Four test programs that nothing runs

`brokentest`, `missingtest`, `slottest` and `diamondtest` are built, staged into
`/disk/bin`, and exercise the failure paths of the loader: an unresolved symbol, a
missing library, a full slot table, and a diamond of four libraries. **No
smoke scenario runs any of them.** They were validated by breaking the code on
purpose, which proves the diagnostic once and not twice. A scenario per program,
with `remove_paths` for the two that need a library absent, is the obvious gap
this work exposed.

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
loader: no se pudo abrir /disk/lib/libffmpeg.so.0.4 (paso 2)
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
loader: no se pudo abrir /disk/lib/libffmpeg.so.0.4 (paso 2)
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
loader: no se pudo abrir /disk/lib/libffmpeg.so.0.4 (paso 2)
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
stays in `userland/`, because it is compiled into each program rather than
linked once.

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
  delegates to `/disk/bin/mediaplayer-ffmpeg` when installed, and otherwise opens an
  in-OS warning"*
- `docs/MEDIA_PLAYER.md`: *"`/bin/mediaplayer` is always built into the system image
  and is the program shown by the launcher"*
- the port's `.sxres`: *"The stable launcher (/bin/mediaplayer) owns the visible
  Accessories entry"*

None of it was true, and none of it was wrong on the merits. Restoring the file
made all three true at once, which is the cheapest possible fix for a documentation
lie: the documentation was the spec.

### The launcher is 114 lines

`port_is_installed()` with `savanxp_stat`, then `exec()` with `argv[0]` replaced.
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

### The icon is a copy, and that is the lesser evil

`progman-smoke`'s scan prints `icono=propio` or `icono=horneado`, and with no
`icon=` in the manifest Media Player came out the only `horneado` in the catalog —
the generic desktop icon. `icon_file` resolves *relative to the manifest*, so
pointing at `ports/ffmpeg/overlay/mediaplayer/icon.png` would put the system's tree
inside a port's layout. The PNG is therefore copied to
`subsystems/posix/userland/mediaplayer-icon.png`, and the duplication is deliberate:
the backend keeps its own because it can also be launched directly, and the two can
drift because it is artwork, not code.

### The scenario proves the entry, not just the file

`mediaplayer-noport` removes `/disk/bin/mediaplayer-ffmpeg` from the volume and
drives the launcher through the taskbar. It asserts the launcher's own text, not the
taskbar caption: the caption is 14 pixels at the current font and detects no font
change at all (measured, and written down in `scenario_taskbar`), whereas the
launcher's four lines are the only thing it can draw — when the port is installed it
opens no window at all.

Two things had to be got right for that assertion, and both were wrong first time:
the text sits on `FIELD` (white), not `FACE`, and a `/` does not match the glyph
table (434 of 436 pixels), so the search string avoids slashes. With the launcher
patched to `return 1` instead of showing the window, the scenario finds 0 of 345
pixels.

`file_assoc` gained three assertions for the same reason: the `ext_open` list in
`mediaplayer.sxres` is a claim, and `.mp3`, `.avi` and `.flac` now have to resolve to
`/bin/mediaplayer` against the stamped binaries in the real image. They must resolve
to the **launcher** and not to the backend — the backend is a port artifact that may
not be there, and the launcher is what explains itself.
