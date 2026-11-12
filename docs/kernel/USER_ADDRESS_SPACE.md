# User address space

How the lower half of a process's address space is laid out, and why the user
half starts at `0x400000`.

## Layout

All constants are in `include/kernel/vmm.hpp` unless noted.

| Address | What lives there | Constant |
| --- | --- | --- |
| `0x0` – `0x3fffff` | Never mapped. Any access faults | — |
| `0x400000` | Program image: the first `PT_LOAD`, with the ELF and program headers, then the text on the next page | `kUserBase` |
| `0x1000000000` (64 GiB) | Kernel-chosen section views (`map_view`, `SXN_SYS_MAP_VIEW`), allocated upwards | `kSectionViewBase` |
| below `kUserStackBottom` | One guard page, never mapped | `kUserStackGuardBottom` |
| `0x7000000000` downwards | User stack: 1 MiB reserved, 8 pages mapped at start, grown on demand | `kUserStackTop` |
| `0x800000000000` | End of the user half: PML4 entry 256 and up belong to the kernel | `kKernelPml4Start` (`kernel/vmm.cpp`) |

A caller-chosen view (`map_view_at`, `SXN_SYS_MAP_VIEW_AT`) may go anywhere
between `kUserBase` and the stack guard that is free; see
[`SECTIONS.md`](SECTIONS.md).

## Why `0x400000`

The value dates from the first kernel with a user mode and was not written
down at the time. These are the properties it gives the system, and the code
that now depends on them:

1. **It is the x86-64 default for non-PIE executables.** GNU ld and lld link
   an `ET_EXEC` at `0x400000` unless told otherwise. The SDK linker script
   (`subsystems/posix/sdk/v1/linker.ld`) uses the same base, so SDK binaries
   have the addresses any x86-64 toolchain expects.
2. **It catches null-pointer accesses with large offsets.** The low 4 MiB are
   never mapped, so `NULL->field`, `NULL[i]` and a struct member far from a null
   base all fault instead of reading whatever happened to be there.
3. **It is 2 MiB aligned.** The hole covers two whole page-directory entries,
   and the image starts on a large-page boundary.
4. **It is the PE32 default `ImageBase`.** A Win32 program image wants
   `0x400000` too, and in a native process that range is free for it. The
   Win32 subsystem relies on this
   ([`WIN32_SUBSYSTEM_ROADMAP.md`](../roadmaps/open/WIN32_SUBSYSTEM_ROADMAP.md)).

## Who enforces it

- `vm::canonical_user_address` (`kernel/vmm.cpp`) rejects any user address below
  `kUserBase`. Every user pointer the kernel validates goes through it.
- The ELF loader (`kernel/elf.cpp`) refuses a `PT_LOAD` below `kUserBase`, and
  loads an `ET_DYN` (PIE) image with a bias of `kUserBase`, so a PIE program
  sits where an `ET_EXEC` would.
- `map_view_at` returns `EINVAL` for a range below `kUserBase`.

The linker script and `kUserBase` must agree. Changing one without the other
makes every SDK program fail to load.

## Planned change

The Linux subsystem lowers the floor to `0x10000` for every subsystem, because
a Linux `ET_EXEC` is linked wherever its linker chose
([`LINUX_SUBSYSTEM_ROADMAP.md`](../roadmaps/open/LINUX_SUBSYSTEM_ROADMAP.md)).
SDK images would still load at `0x400000`. Only the lowest address the kernel
accepts would change, so null-pointer accesses would still fault but large
offsets from a null base would be caught only up to 64 KiB.
