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

## What blocks the rest, in order

1. **PIE for real programs.** The mechanism is proven end to end — `libtest` links
   `libmath.so.0.4`, `crt0` runs the loader, and `sqrt` is called normally — but
   every other program is still `ET_EXEC` with no table to resolve against.
   `SECTIONS.md` has the detail. It is a per-program decision, not another
   mechanism.

2. **Wider libraries.** `libmath.so.0.4` is linked by one test. The saving it
   promises — `sqrt` mapped once instead of in every binary — is real but only
   `libtest` collects it so far. SxGFX, SxGUI and FFmpeg come after the programs
   they live in are PIE.

3. **Export control and versioning.** `DT_SONAME` is read but ignored; a `DT_NEEDED`
   name is looked up verbatim under `/lib`. Symbol visibility beyond global, and
   versioned names, are not implemented.

4. **`dlopen`, and unloading.** The library array is fixed at load time and there
   is no reference counting, so nothing can be removed once mapped. `kMaxLibraries`
   is 8.

## Explicitly not worth doing yet

- **`mprotect` and RELRO.** `BIND_NOW` does not need either; lazy PLT binding does.
- **File demand paging.** A file-backed section is read whole. Sharing saves
  resident memory, not mapped memory, and `memory_bytes` counts mapped-present
  pages, so Task Manager shows the same per-process figure either way.
- **`dlopen`.** Loading by name is a different feature from `DT_NEEDED`, and the
  FFmpeg codec case needs `dlopen`, not this. `CHANGELOG.md` records music as
  deferred for exactly this reason.
