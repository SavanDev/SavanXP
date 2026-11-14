# User address space

How the lower half of a process's address space is laid out: the user half
starts at `0x10000`, and SDK programs load at `0x400000`.

## Layout

All constants are in `include/kernel/vmm.hpp` unless noted.

| Address | What lives there | Constant |
| --- | --- | --- |
| `0x0` – `0xffff` | Never mapped, and refused as a user address. Any access faults | — |
| `0x10000` | The lowest user address: a Linux `ET_EXEC` may load here or anywhere above | `kUserFloor` |
| `0x400000` | An SDK program image: the first `PT_LOAD`, with the ELF and program headers, then the text on the next page. Also where an `ET_DYN` image goes | `kUserBase` |
| `0x1000000000` (64 GiB) | Kernel-chosen section views (`map_view`, `SXN_SYS_MAP_VIEW`), allocated upwards | `kSectionViewBase` |
| below `kUserStackBottom` | One guard page, never mapped | `kUserStackGuardBottom` |
| `0x7000000000` downwards | User stack: 1 MiB reserved, 8 pages mapped at start, grown on demand | `kUserStackTop` |
| `0x800000000000` | End of the user half: PML4 entry 256 and up belong to the kernel | `kKernelPml4Start` (`kernel/vmm.cpp`) |

A caller-chosen view (`map_view_at`, `SXN_SYS_MAP_VIEW_AT`) may go anywhere
between `kUserFloor` and the stack guard that is free; see
[`SECTIONS.md`](SECTIONS.md).

## Why the image base is `0x400000`

The value dates from the first kernel with a user mode and was not written
down at the time. Until SSL's L1 it was also the floor. These are the
properties it gives the system, and the code that now depends on them:

1. **It is the x86-64 default for non-PIE executables.** GNU ld and lld link
   an `ET_EXEC` at `0x400000` unless told otherwise. The SDK linker script
   (`subsystems/posix/sdk/v1/linker.ld`) uses the same base, so SDK binaries
   have the addresses any x86-64 toolchain expects.
2. **It caught null-pointer accesses with large offsets.** With the floor at
   `0x400000` the low 4 MiB were never mapped. With the floor at `0x10000`,
   an access through a null base still faults up to 64 KiB away. Between that
   and 4 MiB it faults only because nothing is mapped there, which a caller
   of `map_view_at` or a Linux image can change.
3. **It is 2 MiB aligned.** The hole covers two whole page-directory entries,
   and the image starts on a large-page boundary.
4. **It is the PE32 default `ImageBase`.** A Win32 program image wants
   `0x400000` too, and in a native process that range is free for it. The
   Win32 subsystem relies on this
   ([`WIN32_SUBSYSTEM_ROADMAP.md`](../roadmaps/open/WIN32_SUBSYSTEM_ROADMAP.md)).

## Who enforces it

- `vm::canonical_user_address` (`kernel/vmm.cpp`) rejects any user address below
  `kUserFloor`. Every user pointer the kernel validates goes through it.
- The ELF loader (`kernel/elf.cpp`) refuses a `PT_LOAD` below `kUserFloor`,
  and loads an `ET_DYN` (PIE) image with a bias of `kUserBase`, so a PIE
  program sits where an SDK `ET_EXEC` would.
- `map_view_at` returns `EINVAL` for a range below `kUserFloor`.

The linker script and `kUserBase` must agree. Changing one without the other
makes every SDK program fail to load.

## Why the floor is `0x10000`

A Linux `ET_EXEC` is linked wherever its linker chose: lld puts a static one
at `0x200000`, and other toolchains use other addresses below 4 MiB. The
Linux subsystem runs such files unchanged
([`LINUX_SUBSYSTEM_ROADMAP.md`](../roadmaps/open/LINUX_SUBSYSTEM_ROADMAP.md)),
so the floor is the same for every subsystem. `0x10000` is Linux's own
default `vm.mmap_min_addr`.
