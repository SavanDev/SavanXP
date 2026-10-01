# Section objects and section views

A **section object** is a refcounted, contiguous run of physical pages with a
capability grant. A **view** is one process's mapping of a section at an address
it chose. This document explains the model, the rules it enforces, and — because
a dynamic loader is the next thing to be built on it — where library images are
meant to live in it.

The kernel side is `object::SectionObject` (`include/kernel/object.hpp`),
`vm::map_section_view` (`kernel/vmm.cpp`) and the syscall handlers in
`kernel/process.cpp`. The userland surface is `section_create` / `map_view` /
`unmap_view` (`subsystems/posix/sdk/v1/include/savanxp/libc.h`).

## What uses a section today

Three callers, and they want different things from it:

| Consumer | Wants | Lifetime |
| --- | --- | --- |
| Heap arenas (`sx_arena_acquire`, `runtime/posix.c`) | anonymous, writable, private | per process |
| GPU surfaces (`GPU_IOC_IMPORT_SECTION`) | shared with another process by handle | while a handle is open |
| File images (`section_open`) | read-only, backed by a file's bytes, shared by inode | as long as one view exists |
| Library images (not built yet) | the same; they are what file images exist for | as long as one view exists |

The heap allocator is the reason this subsystem is budget-driven rather than
cheap: arenas are per process and per-process caps multiply.

## `section_open` and what it does not do yet

`section_open(fd, flags)` takes an already-open read descriptor and returns a
section handle whose pages hold the file's bytes. The whole file is read at
create time — there is no demand paging for files — and the tail of the last page
is zero when the file is not a multiple of 4 KiB.

Write access cannot be requested: `SAVANXP_SECTION_WRITE` is `EINVAL`. That is
the design, not an omission. The backing is read-only so that no page another
process has mapped is ever written, which is what keeps copy-on-write out of the
first milestone; a loader fills writable segments from the file into private
anonymous sections instead.

Two things are deliberately missing:

- **No in-place update detection.** The cache key is the SxFS `inode_id` plus the
  range backed — the range matters because "the whole file" and "one slice of it"
  are different sections, and keying on the inode alone mapped a segment with the
  whole image behind it. But `write_file` rewrites an existing inode rather than
  allocating a new one, so overwriting a library's bytes in place would leave the
  cache serving the old image. This is unreachable today and must stay
  that way until it is handled: **there is no install syscall**, Add/Remove
  Programs can only uninstall, and the build rewrites `disk.img` from the host
  with no kernel running to be stale. The moment an install syscall lands — or
  anything else that writes a file's bytes in place — the key needs a content
  generation bumped by `write_file`, `truncate_file`, `unlink_file`,
  `rename_path` and create, **plus the test that proves it**: open a section,
  overwrite the file with same-size different content, open again, expect
  different bytes. Adding the counter before the mutation path exists would be
  five hooks guarding a case that cannot occur, and one forgotten hook would be
  a silent staleness bug.
- **No end-to-end execution from the mapping.** `sectiontest` proves the bytes
  arrive intact and that the view maps executable, but code that actually *runs*
  out of a file-backed view needs a loader to apply relocations first. The
  anonymous-section test is the one that executes real machine code.

Sharing is observable rather than assumed: `savanxp_system_info` reports
`sections_live` and `file_sections_live`, and `sectiontest` checks that a forked
child opening the same file does not push the count up. The parent has to sample
while the child still holds its handle — hence the event handshake, because a
child that closed first would make the count return to its old value and the
check would pass whether or not the pages were shared.

## `map_view_at`, and why a loader needs it

`map_view` lets the kernel choose the address. `map_view_at(handle, base, flags)`
lets the caller choose it, which is what a loader requires: an ELF's program
headers state where each segment goes, and a loader that cannot honour `p_vaddr`
cannot place anything.

The kernel already supported a caller-chosen base internally —
`vm::map_section_view` honours a non-zero `base_address` and checks
`user_range_is_free` — so this exposes existing behaviour rather than adding a
new one. Two rules keep it honest:

- The base must be page-aligned, and a misaligned one is `EINVAL` rather than
  being silently rounded down. The kernel maps whole pages; returning a page that
  starts before the one asked for would be a lie about the layout.
- The range must be entirely free. A second view over an occupied address is
  refused rather than replacing the first.

`base == 0` is exactly `map_view`.

## The grant is the intersection of three things

`SAVANXP_SECTION_READ`, `_WRITE` and `_EXEC` are requested at two separate
moments — `section_create` grants a section, `map_view` asks for a view — and
`map_view_handle` requires the request to be a subset of *both* the handle's
granted access and the section's own grant:

```c
if ((requested_access & entry.granted_access) != requested_access) return EACCES;
if ((requested_access & section_object->access_mask) != requested_access) return EACCES;
```

So a process cannot obtain a permission that neither its handle nor the section
allows, and cannot obtain one the section does not grant even if the handle was
generous.

## Execution is a view permission

Before this existed, `map_section_view` set `kPageNoExecute` unconditionally:
**no user page outside a validated `PT_LOAD` could ever be executed.** NX is now
a permission of the view, derived the same way write is, and the section grant
above is what authorises it.

W^X stays structural. A view that is writable and executable at once is refused
in two places on purpose:

- `vm::map_section_view`, so no caller — including `fork`, which re-maps views —
  can produce a page that is both.
- `map_view_handle`, additionally, because that rejection can be reported as
  `EINVAL` instead of the generic `ENOMEM` the bool-returning mapping function
  gives for every failure. A loader that misdeclares a segment gets an error that
  says so.

W^X therefore lives in the **view**, not the section. A section may grant read,
write and execute together — that is the grant, not a page flag — while every
view taken from it is either writable or executable.

## Why `mmap` deliberately does not gain `PROT_EXEC`

`sx_mmap` still rejects `PROT_EXEC` with `ENOSYS`, and `mmaptest` asserts it.

The POSIX surface is unchanged on purpose: an executable mapping is available
only through the explicit `section_create` + `map_view` pair, which is where the
grant and the W^X rule are enforced. Widening `mmap` would have made executable
memory reachable from the path least able to explain why it is allowed.

This is the whole of what was added. There is no `mprotect`, no file-backed
mapping, and no dynamic loader yet.

## The budget

Both limits exist because the multiplications, not because a section is
expensive.

`kMaxSectionViews` (per process, `kSectionViews` inline in `VmSpace`):

| Consumer | Views |
| --- | --- |
| Heap arenas | up to `SX_ARENA_CAPACITY` = 8 |
| Library images (projected) | ~3 per library (file-backed text/rodata, private data, bss) |
| Explicit anonymous `mmap`, GPU surfaces | a handful |

~40 in the realistic worst case, so the limit is 64. The array is inline in
`VmSpace`, which is inline in `Process`, so 64 views cost 32 bytes each across
`kMaxProcesses` = 64 processes: 128 KiB of kernel BSS. Raising it further buys
nothing.

`kMaxSectionObjects` (system-wide) is set by the heap, not by libraries:
`kMaxProcesses` × `SX_ARENA_CAPACITY` = 512 is the theoretical ceiling. A real
desktop session runs 8–12 processes, so 12 × 8 + ~10 libraries + surfaces lands
near 115 and the limit is 256. Each entry is 80 bytes, so the table is 20 KiB.

Exhausting either is a clean `ENOMEM`, never a corruption. `sectiontest` proves
both claims rather than asserting them: it holds 48 views at once in one
process, and forks eight children behind a barrier until the global table is
full, which requires every child to have terminated on a clean errno.

Note that views do **not** consume descriptors: `map_section_view` takes its own
reference, so `map_view` followed by `close` is the normal pattern and is what
`sx_arena_acquire` does. `kMaxFileHandles` (64) is therefore not what bounds a
process's view count.

## Where a library image belongs

This is the decision that constrains the loader, and it is why the table is
global rather than per process.

A file-backed section has to be **findable by identity**, so that two processes
asking for the same `libSxGFX.so` get the *same* object and therefore the same
physical pages. That is the entire memory benefit; mapping the file separately
per process would share nothing and would be strictly worse than today's static
link. The global section table is the natural home for that identity, because it
is already the registry of shared page runs and already refcounted.

The backing store is **read-only**. Text and rodata map as `READ|EXEC` from the
shared object; the writable segments are anonymous private sections that the
loader fills from the file. That split is what keeps copy-on-write out of the
first milestone: nothing ever writes a page that is also mapped by another
process, so there is no write fault to handle and no per-page state to track.
It costs a copy of each writable segment per process, which is what every
`DT_NEEDED` shared library already costs on Linux.

Separating anonymous arenas into their own table, leaving this one for images
only, would make the budget honest. It is not done: with 256 entries and a
realistic peak near 115 the sharing is affordable, and splitting a subsystem that
two unrelated callers use is a larger change than the numbers justify.

## The premise: the executable has to export symbols

A shared library can only be useful if the program and the library end up sharing
one copy of things. That needs the executable to expose a dynamic symbol table so
a library can resolve against it — and on SavanXP it does not have one.

This is not a missing linker flag. **lld emits no dynamic symbol table for a
non-PIE `ET_EXEC`**, verified three ways: `--export-dynamic` on the `-static` link
produces an `EXEC` with no `.dynsym`; `-Bdynamic --export-dynamic` does the same;
and adding `--unresolved-symbols=ignore-all` changes nothing. The flag only has an
effect when the output is `ET_DYN`.

The consequences are worth stating plainly, because they are what the remaining
work is made of:

- A library cannot resolve `__stack_chk_fail` or the rest of the runtime, so
  `libmath.so.0.4` is built without `-fstack-protector-strong`.
- There is no interposition: `sqrt` in the program and `sqrt` in `libmath.so` are
  two separate copies.

The way out is PIE. `kernel/elf.cpp` now accepts `ET_DYN` and relocates it onto a
load bias (still a constant, not entropy yet), and the mechanism is proven to
work — an SDK runtime file compiled `-fPIC -fPIE -mcmodel=medium` and linked
without the fixed linker script does produce a `DYN`. What is missing is the
userland half, which is not a small step: `SAVANXP_USER_LINK_OPTIONS` carries
`-static` and `-T` for every target and CMake cannot drop them for one, so the
link options need restructuring per profile first, and then the whole SDK runtime
has to be recompiled as PIC.

## The loader, and what it proved

The decisions, the order of the remaining work and what blocks it are in
[`SHARED_LIBRARIES.md`](SHARED_LIBRARIES.md).

`ldso` is the smallest thing that can fail interesting: one `.so`, its `PT_LOAD`s
placed where the ELF says, its relocations applied, its symbols resolved by name. `ldtest` calls `sqrt`, `fabs` and `floor`
through the loaded pointers and checks the results, so the code really executes
from a file-backed section with its relocated data beside it.

Two things it had to get right that are easy to get wrong:

- **A load bias.** An `ET_DYN` has `p_vaddr` near zero, and zero is not a usable
  user address. The first segment, whose `p_vaddr` is zero, is mapped with the
  kernel choosing; that address minus its `p_vaddr` is the bias everything else
  is placed against.
- **A segment is a slice, not the file.** `section_open_range` exists for this.
  Mapping the whole file per segment put the ELF header at the text segment's
  address and everything shifted by a page.

### What it does not do yet

No `PT_INTERP` handling in the loader itself, but the plumbing exists: the kernel
reads the segment, copies the path onto the initial stack and passes it to
`crt0` in `rcx`, and `savanxp_interpreter_path()` returns it. `interptest` is
linked with `--dynamic-linker` and checks it arrives. What does not happen yet is
`crt0` *running* that interpreter — it still goes straight to `main`. Until it
does, no `DT_NEEDED` chain, and the loader can only use symbols it defines
itself, which is why `libmath.so.0.4` is built without
`-fstack-protector-strong` (its canary would need `__stack_chk_fail` from the
executable). No lazy binding, so every `JUMP_SLOT` is resolved up front. And
`lseek` is declared but not implemented, so no code can reposition a descriptor —
which is why segment bytes are read through a temporary range section rather
than `read`.

## Not decided yet

- **No `mprotect`.** `BIND_NOW` relocation does not need it; lazy PLT binding and
  RELRO do. Adding it means per-page copy-on-write, because a view that drops
  write while another view still holds it writable would otherwise change the
  other process's mapping. The existing `add_user_page_flags` only *adds*
  permissions.
- **No demand paging for files.** A file-backed section is read whole at create
  time. That saves resident memory through physical sharing, but not *mapped*
  memory, and `memory_bytes` counts mapped-present pages — so Task Manager will
  report the same per-process figure as before.
- **No `dlopen`.** Loading by name is a separate feature from `DT_NEEDED`, and
  the `FFmpeg` codec case needs the first, not the second.
