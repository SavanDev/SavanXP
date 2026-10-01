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

1. **The executable in the symbol scope.** `resolve` searches the loaded
   libraries, newest first. It does not yet reach the executable, so a library
   with an undefined `__stack_chk_fail` cannot load and
   `libmath.so.0.4` is still built without `-fstack-protector-strong`. Locating
   the executable from a library means finding its `.dynsym`, which means knowing
   where its image was placed; `pietest` is the first one with a table to read.

2. **PIE for userland.** The premise: lld emits no dynamic symbol table for a
   non-PIE `ET_EXEC`, so the executable cannot export anything to a library.
   `SECTIONS.md` has the detail. The link options are restructured per profile,
   the SDK runtime has a PIC-clean check target, and `pietest` is the first `DYN`
   with `.dynsym`. What is missing is migrating real programs to the profile —
   which is a per-program decision, not another mechanism.

3. **`crt0` runs the interpreter.** The kernel still maps the main image itself
   rather than handing the whole job to `ld.so` as Linux does. The plumbing is in
   place; changing that would alter how all 82 binaries start, so it is worth
   doing once 1-2 are solid.

## Explicitly not worth doing yet

- **`mprotect` and RELRO.** `BIND_NOW` does not need either; lazy PLT binding does.
- **File demand paging.** A file-backed section is read whole. Sharing saves
  resident memory, not mapped memory, and `memory_bytes` counts mapped-present
  pages, so Task Manager shows the same per-process figure either way.
- **`dlopen`.** Loading by name is a different feature from `DT_NEEDED`, and the
  FFmpeg codec case needs `dlopen`, not this. `CHANGELOG.md` records music as
  deferred for exactly this reason.
