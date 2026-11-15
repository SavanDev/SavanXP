# i386 memory objects and ELF32 libraries

The active i386 kernel now uses the v0.4.0 section model for anonymous memory,
heap arenas and library images. This is milestone 3 of [the port](I386_PORT.md),
not a claim of complete v0.4.0 parity. The full SxGUI session is the next dependency.

## Address space and ownership

Two-level, non-PAE page tables keep the image base at `0x08000000`. The user range
now ends at `0x40000000`, below the supervisor physical aliases at
`0x40000000..0x80000000`. Automatic section views start at `0x10000000` and scan
for free ranges, wrapping within that region. Fixed views must be aligned,
unoccupied and wholly inside user space. The image and views cannot occupy the
reserved stack or its guard. Device identity mappings that overlap user space or
the direct map are rejected; supporting other framebuffer placements needs an
explicit kernel device mapping. RAM allocation remains limited to the first GiB.

The user stack keeps its existing top, `0x09000000`, but reserves the reference
1 MiB, with the permanent guard at `0x08eff000`. Only eight pages are initially
resident. A user not-present page fault inside that reserve materializes a zeroed
page and resumes the original context. Protection faults and accesses to the
guard still terminate the task. Syscall validation does not grow absent buffers.

`kernel/section.cpp` is extracted from the pinned reference's `kernel/object.cpp`.
Its 256 global section slots, 64 MiB per-section bound, individual physical-page
allocation and failure rollback remain the reference algorithms. The reference
kernel heap supplies page-list storage; explicit 16-byte metadata alignment makes
its headers valid with 32-bit pointers. One initial heap arena stays resident;
additional empty arenas return to the physical allocator.

A handle and a view each own a reference. A view maps the section's physical pages
with a software PTE ownership bit: destroying a directory must not free pages
owned by a live section. Unmapping removes the borrowed leaves before releasing
the view's reference and reclaims empty page tables. Closing a descriptor while
a view exists is normal. Exit, kill and contained faults release views and
handles; successful `exec` releases old views but preserves open descriptors.
A failed `exec` leaves the old image, views and descriptors intact.

Each process has 64 views and 64 descriptors. Exhaustion reports an errno and
rolls back partial mappings. Grants intersect the handle and section grants.
Writable executable views are rejected, as in the reference. The legacy CPU has
no NX: this is an API grant rule, not hardware execute-disable; a readable user
page remains executable. Simultaneous RW and RX aliases are also possible under
the reference's per-view rule.

`fork` copies private image/stack pages and CPU/FPU state, retains shared views,
clones `SAVANXP_VIEW_PRIVATE` sections and retains every descriptor with its
original grant. Heap arenas and writable library segments use private views.
The six-process capacity remains. Pipe handle queues use the reference four-slot
ownership protocol: send retains, receive installs then releases the queue
reference, a full descriptor table leaves the entry queued, and the final reader
closing drains abandoned entries. I/O objects cannot be sent through this queue.

## Immutable file snapshots

`section_open` and `section_open_range` read a whole file or a bounded byte range
into immutable backing. Anonymous sections are zeroed; a file tail is zero-padded
to a page. Short backing reads fail and release the entire allocation.

SxFS backing is shared by volume, inode and byte range. Initramfs files have no
SxFS identity and are not cached. A later request may add an execute grant to the
same immutable backing, but it cannot upgrade an already-issued handle. File
sections never grant writes.

The POSIX milestone made in-place file mutation reachable, invalidating the old
reference documents' assumption that only a stopped host build could update an
image. VFS writes, truncation and unlink remove old snapshots from lookup without
changing their bytes or references. New opens read the changed content, including
same-size overwrites and inode reuse after unlink. No disk format or journal
change is required. Rename preserves identity and bytes; it does not need to
invalidate the snapshot.

## The ELF32 adaptation

The kernel loads `ET_EXEC` at its linked addresses and `ET_DYN` with a bias at the
existing user base. It validates native header/program-header sizes, ranges,
alignment, entry placement and incompatible writable/executable segments, including
page-sharing unions. The main PIE's header must be mapped. `PT_INTERP` is copied
beside argv without overwriting argument strings. Registers retain the established
i386 entry ABI: EDI argc, ESI argv, EDX native guard, ECX interpreter path, EBP image
header. The guard still uses the bring-up seed; reference entropy remains a
security parity gap. The pointer array and guard are four bytes; the call stack is 16-byte aligned.

SavanXP retains its embedded interpreter design: the kernel maps the executable;
`crt0` invokes the weak C startup hook; `ldso.c` walks `DT_NEEDED` under `/disk/lib`
and resolves eagerly before `main`. `PT_INTERP` records `/disk/lib/ld.so.0.4`; it is
not a Linux-style separately mapped interpreter executable. Library text comes
from shared immutable sections, writable segments from private anonymous copies.
The executable supplies exported SDK symbols. One bias governs every library's
segments; the dependency graph, newest-first symbol scope, 32 image slots and
24 program headers per library follow v0.4.0. Missing-dependency reporting retains
the reference's tolerant startup policy rather than silently claiming success.

Native ELF structures use 32-bit addresses, offsets, dynamic words and symbols.
`DT_REL` entries contain no explicit addend: it is the original destination word.
The loader handles `R_386_NONE`, `R_386_RELATIVE` (`B+A`), `R_386_32` (`S+A`),
`R_386_PC32` (`S+A-P`), `R_386_GLOB_DAT` and `R_386_JUMP_SLOT` (`S`). Unresolved weak
symbols resolve to zero. Unsupported relocation types fail explicitly. These
formulas follow the [x86 ABI relocation table](https://docs.oracle.com/cd/E19683-01/817-3677/chapter6-26/index.html).

An i386 bootstrap must relocate its own GOT before protected C reads the stack
canary through it. `crt0` establishes the GOT using a relative call/pop, records
entry registers through local offsets, then calls an unprotected bounded helper
to apply the main image's RELATIVE entries. An invalid bootstrap exits 126. The
ordinary loader skips those entries for the main image, avoiding a second bias.
Library and imported-symbol relocation still belongs to the ordinary loader.

Dynamic tables, symbol counts and relocation writes are bounded by mapped
segments. The native build emits SysV hash tables for exact symbol counts.
Temporary file and range-section descriptors close after use, including fd 0;
rejected unpublished images release already-placed views. Published dependency
failures retain the reference diagnostic state until process exit. There is no
`dlopen`/unload, lazy binding, TLS, COPY relocation support, ASLR or `mprotect`.
The existing POSIX anonymous `mmap` still rejects `PROT_EXEC`.

## Build and verification

`cmake/elf32-libraries.cmake` supplies native PIC library and PIE executable
profiles, compiling the SDK runtime with the appropriate units removed. `libtest`
excludes static math and declares a real `DT_NEEDED` on `libmath.so.0.4`.
`tools/check_shared_libs.py` runs on staged build artifacts and rejects a program
that duplicates a library it declares. LLVM `nm` (or host `nm`) is discovered
through CMake, alongside LLVM readelf. Libraries stage below `/disk/lib` and
CPIO `/lib`; synchronization remains additive and preserves external files.

```sh
./build.sh build
./build.sh smoke shared
./build.sh smoke shared-memory  # 32 MiB, mandatory section/fork ENOMEM rollback
./build.sh smoke
./build.sh smoke memory
./build.sh test
python3 tools/verify_posix_persistence.py
./tools/verify_doom_persistence.sh
```

`shared` executes the reference heap and mmap tests, PIE/interpreter/math/chain/
diamond/slot/missing-symbol/dependency tests and two focused native regressions. `vmtest32` checks zeroing,
aliases, grants, RX execution, range overflow, immutable file sharing/mutation,
queued-handle cleanup, 256 live global objects, 64 simultaneous views, stack
fault recovery and exec cleanup. On 32 MiB it also requires exact free-page
recovery after partially allocated 64 MiB sections and failed private-view fork.
`dyntest32` proves implicit nonzero absolute addends, PC-relative data, weak
symbols, imported functions, shared text across simultaneously live processes,
private spawned/forked data, all 61 nonstandard descriptors free after loading,
and repeated partial-image rollback with fd 0 available.

The full smoke includes this workload plus POSIX/shell, CPU/FPU/fault and minimal
desktop glyph/input checks. It does not exercise SxGUI; that requires the GPU,
window protocol and compositor port and restoration of `sxgui-smoke` in milestone
4. The 64-process reference capacity, complete signal/object APIs and hardware
coverage remain later parity work. Clean builds also expose existing LP64
assumptions in the SDK's `intmax_t` aliases/parsers and `<inttypes.h>` format
macros; a full SDK integer-width audit is still required for application parity.
The memory/loader tests do not certify every libc function. Passing memory and loader tests does not
establish that the full desktop or external Doom runtime works.
