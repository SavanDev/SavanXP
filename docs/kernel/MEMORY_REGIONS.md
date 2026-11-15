# Memory regions

Memory a program asks for by address range, with an access and nothing
behind it: what Linux's `mmap`, `munmap`, `mremap` and `brk` work on, and what
`VirtualAlloc` will. It is S2a of the shared groundwork in the
[Linux roadmap](../roadmaps/open/LINUX_SUBSYSTEM_ROADMAP.md#shared-sx-groundwork);
the first consumer is SSL's L2 ([LINUX.md](../userland/LINUX.md)).

**Built: anonymous regions and their first-touch pages (S2a), plus range
protection and eager private file snapshots (S2b).** SSL's L4 dynamic linker
is the first consumer of S2b. These mappings use regions, not section views.

## Why not section views

A section view maps a whole section at once and goes as a whole. Programs ask
for something else: unmap one page from the middle of a mapping, grow a block
where it is or move it, replace part of a mapping with a fixed one. Musl's
`malloc` does all of that. Raising `kMaxSectionViews` would not provide any of
it, so regions live beside the views, not instead of them.

## The list

`region::List` (`include/kernel/region_list.hpp`, `kernel/region_list.cpp`)
is the bookkeeping: a sorted array of `{start, end, access}`, page aligned,
never overlapping. An `anonymous` bit distinguishes lazy zero pages from
eager private snapshots. Two regions that touch with the same access and
backing kind are one, as
Linux merges its VMAs, so a heap that maps and unmaps blocks keeps the list
short. The access bits are `kRead`, `kWrite` and `kExecute`, Linux's `PROT_*`
values.

| Operation | What it does |
| --- | --- |
| `insert` | Adds a free range and merges it with a neighbour of the same access |
| `remove` | Takes a range out of every region it overlaps, splitting one it straddles; nothing there is not an error |
| `covers`, `protect` | Check full coverage, then split at both protection boundaries and merge compatible neighbours; a hole or insufficient storage leaves the list unchanged |
| `find`, `containing`, `overlaps` | Binary searches |
| `find_gap` | The lowest free range of a size between two addresses, asking a callback what else is in the space |

The file allocates nothing and touches no page: its storage is the caller's,
and an operation that needs a new entry fails, with the list unchanged, when
there is no room. A protection change needs at most two extra entries; one
insert or remove adds at most one entry
(`kMaxGrowth`). `tests/host/region_list_test.cpp` (`region-list-test`)
compiles it as the kernel does and checks merging, overlap, splitting, the
capacity limit, ranges that wrap past the top, and the gap search with an
obstruction.

## In the address space

`vm::VmSpace` holds a pointer to its list, made on the kernel heap at the
first region and freed with the space, and `image_end`, the page-aligned end
of the loaded image's highest segment, where a program break begins. The
storage doubles from 16 entries up to `kMaxRegions` (4096). A space that never
had a region costs one null pointer.

The operations are in `include/kernel/vmm.hpp`:

| Function | |
| --- | --- |
| `map_region` | A new region. `anywhere` takes the hint when it is free, otherwise the lowest free range from `kRegionBase`; `exact` takes the address or fails; `replace` first takes out the regions in the range |
| `protect_regions` | Changes the access of a fully covered range, retaining resident pages, with W^X enforced |
| `map_private_file` | Copies the file into private pages, then installs the snapshot at a hint, an exact address, or over existing regions |
| `unmap_regions` | Takes a range out of the regions and frees its pages |
| `remap_region` | Resizes a range inside one region: shrinking frees the tail; growing takes the pages after it when they are free, otherwise, when the caller allows, moves it |
| `fault_in_region` | Makes the page of a region that holds an address, zeroed, when the region allows the access |

Each answers a `RegionStatus`, which the subsystem turns into its own error.

Linux ELF image segments and their interpreter are regions too, so musl can
apply RELRO to their pages. Native/POSIX image loading keeps its existing
mapping path.

**What a region may not touch.** A range that holds something that is not a
region refuses a replace or an unmap with `foreign`: a section view, a page
with a table entry no region covers (a native/POSIX image's), or the stack's reserved
range. Regions are placed below the stack's guard page. The gap search skips
those mappings through the same check, so a region never lands on a view or
the image, and a view never lands on a region: `user_range_is_free`, which the
views use, counts regions too, even where no page exists yet.

**Writable and executable at once** is refused, as everywhere else.

## Pages appear on first touch

A region's pages are not allocated when it is made. The first access maps a
zeroed page with the region's access:

- From the program: a not-present #PF goes to `process::fault_in_user_region`
  after the stack's growth (`arch/x86_64/cpu_init.cpp`). A write needs
  `kWrite`, an instruction fetch `kExecute`, and any access needs some bit: a
  `PROT_NONE` region, or an access the region does not allow, ends the
  process as before.
- From the kernel: `vm::is_user_range_accessible`, through which every check
  of user memory passes, makes the missing pages before the copy, as it does
  for the stack. A `read()` into fresh memory works; a `write()` from a
  `PROT_NONE` page is `EFAULT`.

A replaced or regrown range therefore reads zero again: its old pages were
freed, and the next touch makes new ones.

## Freeing pages and the other cores

An unmapped page may still be in the TLB of another core that holds the
space (a thread of the same process on another core). Freeing it at once
could hand a page to someone else while that core can still write it. The
kernel never waits for the other cores (see "TLB coherence" in `vmm.hpp`), so
`release_mapped_pages` does what the views do with their sections: when
another core holds the space, the physical pages go on a deferred list with
the shootdown number, and `reclaim_deferred_views` frees them once those cores
have flushed. The record lives on the kernel heap, never in the freed pages
themselves. When it cannot be allocated, the unmap fails with nothing changed.
A space no other core holds frees at once.

`remap_region` moves page table entries, not contents. It makes every table
the destination needs first, so the move cannot fail half way. The pages stay
owned, so the old addresses only need the other cores to stop using them.

## Private file snapshots and protection

`map_private_file` resolves placement first, then stages the file in private physical
pages, without a user-visible writable alias. It allocates the pages and destination page tables and reads
all bytes before a fixed replacement discards old pages. There is no page
cache or retained file identity: writes to the mapping are private, closing
or unlinking the file does not invalidate it, and later file modifications
are not reflected. Splitting and protection preserve the backing kind.
A partially filled last file page is zero-padded; whole pages beyond EOF have
no backing and cannot be faulted in as anonymous pages. A kernel user-copy
from those pages fails; a user access takes the usual fatal page fault (SSL
has no SIGBUS delivery before L7).

A touched `PROT_NONE` page retains its physical page behind a supervisor-only
leaf with a software marker. User copies and the CPU cannot access it;
restoring permissions restores its data. Unmap, movement, cloning and resident
accounting recognize that marker. Protection changes flush this CPU's TLB.
They currently refuse with `no_memory` if another CPU still holds the address
space, rather than grant new access while old permissions may remain there.
A threaded consumer will need a synchronous protection shootdown before that
restriction can be lifted; SSL's current profiles have one thread.

Growing a private file mapping with `mremap` is refused: without a retained
file identity, newly exposed file bytes cannot be supplied. Shrinking works.
Shared file mappings remain unsupported.

## Fork and exec

`clone_address_space` copies the list. The regions' pages are plain user
pages, copied like the rest. An exec builds a new space, so the regions go
with the old one.

## What is not built

- **Shared anonymous and file regions**: refused until a program needs one.
- **Placement randomization**: regions are placed from `kRegionBase` up, the
  same addresses on every run.
- **Freeing empty page tables**: a table left empty by an unmap stays until
  the space goes. The next region in the range reuses it.
