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

- **Lazy PLT binding.** No `BIND_NOW`, so a relocation error surfaces at the first
  call through that entry point rather than at load.
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
acceptance condition is a wish. Phases 0 and 1 come before anything that makes the
system more capable: neither depends on future work, both are small, and together
they are the difference between a loader that works on what was tested and one that
fails in a way you can act on.

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

Add `BIND_NOW` to the PIE profile. Every relocation is applied during load, so a
missing or wrong dependency is reported while the loader can still say what it was.

**Done when** a library with an unresolvable symbol fails at startup with the name
in the message, instead of crashing later at whichever call site happened to be
first.

### Phase 2 — size and coverage

1. `--gc-sections` in the PIE profile, which removes the 3534 dead symbols. The
   risk is dropping something a program reaches only indirectly, so verify with the
   full smoke plus all 18 scenarios, and check the binaries shrank.
2. The window caption is the only text with no glyph assertion. Either model
   `windowd_caption_colour()` in the harness, or have `windowd` report what it
   painted. The current solid-only mode gives 14 pixels and does not catch a wrong
   font, so it is deliberately not shipped as a check.

### Phase 3 — FFmpeg

The reason any of this exists, and the phase nothing above delivers on its own.

`ports/ffmpeg/configure.sh` still passes `--disable-shared --enable-static`. Codec
libraries are loaded **by name**, so they need `dlopen` — a different feature from
`DT_NEEDED`, and not implemented at all. The library array is fixed at load time and
there is no reference counting, so nothing can be unloaded either.

**Done when** a program maps `libavcodec` and its codec, and `dlopen` exists to load
one by name. Treat this as the acceptance test for the whole subsystem: it is the
first workload that genuinely needs it.

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

- **`mprotect` and RELRO.** `BIND_NOW` does not need either; lazy PLT binding does,
  and Phase 1 removes the laziness. RELRO becomes worth revisiting only after
  Phase 4, once nothing resolves against the executable and there is no longer a
  GOT to protect from it.
- **File demand paging.** A file-backed section is read whole. Sharing saves
  resident memory, not mapped memory, and `memory_bytes` counts mapped-present
  pages, so Task Manager shows the same per-process figure either way.
- **`dlopen` as a general feature.** It is not on the critical path and Phase 7 is
  the only thing that wants it, so it should be built for the codec case or not at
  all — a general `dlopen` with reference counting and unloading is a much larger
  feature than the one FFmpeg needs, and building the larger one first would put a
  second subsystem between here and a working player.
