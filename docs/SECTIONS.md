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
| Library images (not built yet) | read-only, backed by a file, shared by every process that loads it | as long as one view exists |

The heap allocator is the reason this subsystem is budget-driven rather than
cheap: arenas are per process and per-process caps multiply.

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
