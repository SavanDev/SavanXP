# Native heap

> **Status: prepared (2026-10-20), not built.** The allocator's page source is
> a replaceable hook (`sx_heap_pages_map`, `sx_heap_pages_unmap`), and
> `native-test` asserts what a native program has today: the bootstrap arena
> and nothing more. The native definitions of the hook are the next step
> below.

A native program (`EI_OSABI` 0x53, [NATIVE_APPS.md](NATIVE_APPS.md)) has no
POSIX calls. The platform services (`compositord`, `windowd`, `sessiond`) get
by without a heap; SXGFX bitmaps, SXGUI-C and anything that builds a list at
run time do not. This document is how `malloc` comes to native programs.

## What there is

One allocator serves every program, in `subsystems/posix/sdk/v1/runtime/posix.c`
(`sx_malloc` and the rest, aliased to `malloc`, `free`, `realloc`, `calloc`,
`aligned_alloc`):

- First fit over a list of blocks per arena, split on allocation, coalesced on
  free, 16-byte alignment (`max_align_t` on x86-64).
- A **bootstrap arena** of 256 KiB in the BSS (`SX_HEAP_SIZE`). It needs no
  system call: the kernel maps the BSS at exec.
- Then up to **8 arenas** (`SX_ARENA_CAPACITY`) from the **page source**,
  growing geometrically from 1 MiB to 32 MiB each, so a large heap takes few
  of them. An arena that becomes entirely free goes back to the page source;
  the bootstrap arena never does.

The page source is two functions (`savanxp/libc.h`):

```c
void* sx_heap_pages_map(size_t bytes);           /* zeroed, private, read-write, or 0 */
void sx_heap_pages_unmap(void* base, size_t bytes);
```

`bytes` is a multiple of 4 KiB; unmap gets exactly what one map returned. The
runtime's definitions are **weak**: an anonymous section is created, mapped
with a private view and its handle closed (the view keeps the section alive
and costs no descriptor), and unmapped on release. They sit apart from the
allocator so that a program defining the same two functions strongly replaces
only them, and the allocator stays one.

## What a native program has today

Every static program links `posix.c` -- native ones too: its call-free parts
(`snprintf`, string functions) are what they use. So the allocator is already
there, and the bootstrap arena works in a native process: no system call is
involved. The first arena it asks the page source for fails, because the
weak definitions issue POSIX `section_create`, which is `ENOSYS` from a native
process, and `malloc` returns 0. A native program can allocate about 256 KiB
today and not a byte more. `nativetest` asserts both halves: 64 KiB comes back
(and returns to the same address after a free), 1 MiB is refused.

## The native page source

The same operations exist on the native API, with the same flag values:
`sxn_section_create`, `sxn_map_view` with `SXN_VIEW_PRIVATE`, `sxn_close`,
`sxn_unmap_view`. The native definitions of the hook are those four calls and
nothing else.

**Where they live:** a native runtime unit, `system/runtime/sxn_runtime.c`,
compiled into every native target -- `savanxp_program(... NATIVE ...)`, which
`savanxp_system_service` uses, and `tools/build-user.sh --native` for external
ones. It is the place for the rest of the native runtime as it is needed: the
next candidate is `crt0` ending a native process with `sxn_exit` when `main`
returns, which today every native program has to do itself.

Not chosen:

- **Falling back to SXN when the POSIX call returns `ENOSYS`.** It works with no
  build change, but it puts a subsystem test inside the POSIX runtime, and a
  POSIX program that someday gets `ENOSYS` there for another reason would
  silently switch APIs.
- **A second allocator for native programs.** Two allocators to keep correct
  for one difference of four calls.
- **A header-only hook in `include/sxn/`.** The native SDK headers are
  `static inline`; a weak symbol is replaced only by an external definition,
  which needs a real translation unit.

## Budget

- **Views:** 64 per process (`kMaxSectionViews`, [SECTIONS.md](../kernel/SECTIONS.md)).
  The heap takes at most 8. `windowd` maps about 25 with every slot full (client
  surfaces, input rings, the window list, the display, its own surface, the
  scene), so 33 with a full heap.
- **Section objects:** 256 system-wide (`kMaxSectionObjects`), sized for 8 arenas
  in each of a dozen processes plus surfaces and libraries.
- **Memory:** a section's pages are allocated and zeroed when it is created, so
  an arena costs its whole size at once (1 MiB for the first). The bootstrap
  arena is resident in every static program, native or not, as it already is.
- **Descriptors:** none kept; the handle is closed once mapped.

## Steps

1. **The hook.** Done: `sx_heap_pages_map`/`sx_heap_pages_unmap`, weak in
   `posix.c`, the allocator calling only them.
2. **Prove today's limit.** Done: `nativetest` (`native-test`) allocates inside
   the bootstrap arena and expects past it 0 -- the baseline the next step
   changes.
3. **The native page source.** `system/runtime/sxn_runtime.c` with the strong
   definitions, wired into native targets in CMake and in `build-user.sh
   --native`. The check of step 2 then allocates several MiB across arenas,
   frees them, and sees the arenas go back (the process's view count, through
   `SXN_SYS_PROCESS_QUERY` if it reports views, or by allocating again to the
   same addresses).
4. **Use it.** The first consumer is whatever moves next onto the native API;
   [DESKTOP_ARCHITECTURE.md](../desktop/DESKTOP_ARCHITECTURE.md) names the
   desktop and Program Manager once they have files.
