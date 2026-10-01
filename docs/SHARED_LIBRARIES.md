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
| Two `PT_LOAD`s may share a page, union of permissions | done |

## What blocks the rest, in order

1. **PIE for userland.** The premise: lld emits no dynamic symbol table for a
   non-PIE `ET_EXEC`, so the executable cannot export anything to a library.
   `SECTIONS.md` has the detail. Needs the link options restructured per profile,
   then the whole SDK runtime compiled as PIC. Everything below depends on it.

2. **A real symbol scope.** `resolve` currently looks only in the library being
   loaded. A `DT_NEEDED` chain needs: the library, its dependencies in reverse
   order, then the executable. Loading a dependency overwrites the working
   library's state, so `ldso` has to become an array of libraries with per-library
   program headers, `dynsym`/`dynstr` and bias, and `at_vaddr` has to ask which
   library owns an address. That question has no answer in the current code.

3. **`DT_NEEDED`.** Recursive loading, and the search path that goes with it.

4. **`crt0` runs the interpreter.** The kernel still maps the main image itself
   rather than handing the whole job to `ld.so` as Linux does. The plumbing is in
   place; changing that would alter how all 82 binaries start, so it is worth
   doing once 1-3 are solid.

## Explicitly not worth doing yet

- **`mprotect` and RELRO.** `BIND_NOW` does not need either; lazy PLT binding does.
- **File demand paging.** A file-backed section is read whole. Sharing saves
  resident memory, not mapped memory, and `memory_bytes` counts mapped-present
  pages, so Task Manager shows the same per-process figure either way.
- **`dlopen`.** Loading by name is a different feature from `DT_NEEDED`, and the
  FFmpeg codec case needs `dlopen`, not this. `CHANGELOG.md` records music as
  deferred for exactly this reason.
