# Anonymous memory regions

Memory a program asks for by address range, with an access and nothing
behind it: what Linux's `mmap`, `munmap`, `mremap` and `brk` work on, and what
`VirtualAlloc` will. It is S2a of the shared groundwork in the
[Linux roadmap](../roadmaps/open/LINUX_SUBSYSTEM_ROADMAP.md#shared-sx-groundwork);
the first consumer is SSL's L2 ([LINUX.md](../userland/LINUX.md)).

**Built: anonymous regions, their first-touch pages, and the four
operations.** Changing the access of a range and file-backed regions are S2b.

## Why not section views

A section view maps a whole section at once and goes as a whole. Programs ask
for something else: unmap one page from the middle of a mapping, grow a block
where it is or move it, replace part of a mapping with a fixed one. Musl's
`malloc` does all of that. Raising `kMaxSectionViews` would not provide any of
it, so regions live beside the views, not instead of them.

## The list

`region::List` (`include/kernel/region_list.hpp`, `kernel/region_list.cpp`)
is the bookkeeping: a sorted array of `{start, end, access}`, page aligned,
never overlapping. Two regions that touch with the same access are one, as
Linux merges its VMAs, so a heap that maps and unmaps blocks keeps the list
short. The access bits are `kRead`, `kWrite` and `kExecute`, Linux's `PROT_*`
values.

| Operation | What it does |
| --- | --- |
| `insert` | Adds a free range and merges it with a neighbour of the same access |
| `remove` | Takes a range out of every region it overlaps, splitting one it straddles; nothing there is not an error |
| `find`, `containing`, `overlaps` | Binary searches |
| `find_gap` | The lowest free range of a size between two addresses, asking a callback what else is in the space |

The file allocates nothing and touches no page: its storage is the caller's,
and an operation that needs a new entry fails, with the list unchanged, when
there is no room. One insert or remove adds at most one entry
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
| `unmap_regions` | Takes a range out of the regions and frees its pages |
| `remap_region` | Resizes a range inside one region: shrinking frees the tail; growing takes the pages after it when they are free, otherwise, when the caller allows, moves it |
| `fault_in_region` | Makes the page of a region that holds an address, zeroed, when the region allows the access |

Each answers a `RegionStatus`, which the subsystem turns into its own error.

**What a region may not touch.** A range that holds something that is not a
region refuses a replace or an unmap with `foreign`: a section view, a page
with a table entry no region covers (the image's), or the stack's reserved
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

## Fork and exec

`clone_address_space` copies the list. The regions' pages are plain user
pages, copied like the rest. An exec builds a new space, so the regions go
with the old one.

## What is not built

- **Access changes** (`mprotect`) and **file-backed regions**: S2b, for the
  dynamic loader of L4.
- **Shared anonymous regions**: refused until a program needs one.
- **Placement randomization**: regions are placed from `kRegionBase` up, the
  same addresses on every run.
- **Freeing empty page tables**: a table left empty by an unmap stays until
  the space goes. The next region in the range reuses it.
