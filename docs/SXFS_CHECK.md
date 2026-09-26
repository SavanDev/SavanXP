# SxFS consistency reporting

This document is about the one question the filesystem driver could not answer
from inside SavanXP: what does the volume think it is holding.

It also records why the answer is a report and not a repair, and what would have
to be true before a repair is possible.

## The gap that motivated it

Mounting SxFS already validates a substantial amount. `home_metadata_is_valid`
in `kernel/sxfs.cpp` walks every inode and rejects the volume if:

- the root inode is not allocated
- an inode's allocation bit disagrees with its type
- an extent falls outside `[data_lba, total_sectors)`
- two extents inside one inode overlap
- a free inode still carries extents or a nonzero size
- an inode's `size` exceeds its extent capacity
- a sector claimed by an inode is marked free in the block bitmap
- a sector is claimed by two different inodes

That is a real amount of safety, and it is why a corrupt image normally fails to
mount at all rather than mounting into nonsense.

Two things were outside it, and both are invisible in normal operation:

- **A leaked block.** A sector marked occupied in the bitmap that no inode
  claims. Mounting does not care, because every check runs in the direction
  "an occupied sector has an owner". The consequence shows up only as space that
  `df` cannot account for: `free_bytes` is `total - used` over the bitmap
  (`kernel/sxfs.cpp:1209`), so a leak is a permanent, silent subtraction from
  free space with no way to find it from the guest.
- **An orphan inode.** Allocated, structurally valid, holding its own sectors,
  and named by no directory. It passes every per-inode check. Only reachability
  from the root exposes it.

The host had a validator for both. `sxfs_walk` in `libsxfs/sxfs_core.c` walks
the tree, rejects duplicate names within a directory, and refuses to follow two
paths to one inode. But it runs on the host against an unmounted image file,
which is exactly the position you are not in when the machine does not boot
under QEMU.

## What `fscheck` is

A userland program that asks the kernel for a consistency report of the mounted
volume and prints it.

```text
fscheck
```

It is **read-only by construction**. It holds the volume mutation lock, reads,
and reports. It writes no byte, allocates no block and repairs nothing, so the
worst outcome of running it on a damaged volume is a report. The core smoke
scenario runs it against a volume it has just written to and deleted from, which
means a false positive here fails the build.

Exit status is `0` when there are no findings and `1` when there are. It does not
distinguish "repaired" from "could not repair", because there is no repair.

### Reading the output

```text
SxFS consistency report
geometry:
  total_sectors       131072
  data_lba            197
  data_sectors        130875
  superblock_seq      20
  clean_shutdown      yes
  journal_valid       no
  journal_pending     0
population:
  inodes_allocated    152
  files               142
  directories         10
block accounting:
  data_used           105277
  data_claimed        105277
reconciliation:
  used == claimed (balanced)
findings:
  none

result: clean
```

The two numbers that carry the most information are `data_used` and
`data_claimed`. One counts sectors the bitmap marks occupied; the other counts
sectors some inode claims. On a healthy volume they are equal. When they differ,
the findings below say on which side:

| Finding | Meaning |
| --- | --- |
| `leaked_blocks` | occupied in the bitmap, owned by no inode |
| `lost_blocks` | claimed by an inode, free in the bitmap |
| `double_claimed` | two inodes on one sector |
| `metadata_unmarked` | sector in `[0, data_lba)` marked free |
| `orphan_inodes` | allocated and unreachable from the root |
| `alias_inodes` | two directory entries naming one inode |
| `duplicate_names` | one name twice in one directory |
| `bad_dir_entries` | entry that fails validation |
| `unreadable_dirs` | directory unreadable, or deeper than the format allows |

`data_used > data_claimed` means leaks. The reverse means lost blocks, which is
the more dangerous of the two: a sector an inode claims while the bitmap offers
it is a sector two files can be given.

### Two counters that look like findings and are not

Both cost a real debugging detour, so they are written down here.

**`journal_valid = no` is the healthy state.** `clear_journal()` in
`kernel/sxfs.cpp` zeroes the journal header, and `sxfs_journal_valid()` requires
the magic. A cleared journal therefore does not validate. The report only
interprets a header that carries its magic, and only counts a finding when a
header with magic fails its checksum.

**`clean_shutdown = no` is also normal.** The superblock is marked dirty whenever
a full mount needs to write, which is every boot. It is reported because it is
useful context when reading a report, not because it is a defect, and it is
deliberately excluded from the clean/dirty verdict.

## Implementation notes

Two decisions in `kernel/sxfs.cpp` are worth keeping in mind before changing
this code.

**The tree walk is iterative and the stack budget is 32 KiB.** A process gets 8
kernel stack pages (`kKernelStackPages` in `kernel/process.cpp`). The host
validator copies a directory's entries into a 255-entry array, which is 20 KiB
and would not fit alongside recursion. The in-kernel walk instead keeps a
`kMaxDirectoryDepth` stack of directory ids and detects duplicate names by
re-reading earlier entries. That is O(n²) reads over a directory the format caps
at 255 entries, in exchange for 0 bytes of buffer. Reachability uses a 32-byte
stack bitmap for the same reason.

**`extent_claims` is reused as scratch.** It is zeroed and refilled on every
`home_metadata_is_valid` call and read nowhere else, so the report rebuilds it
and adds no BSS. The two volumes of `g_volumes` are the reason that matters:
each is already around 185 KiB of static metadata, and growing the block bitmap
for a larger volume would make that the dominant cost.

## Why there is no repair yet

A repair needs to write to a filesystem that may be inconsistent, which means it
cannot run against a mounted volume, and it needs a mount slot. There are none
free:

```c
// kernel/sxfs.cpp
constexpr size_t kMaxVolumes = 2;   // el instalador usa ambos: raiz y destino
```

A repair mount would need that raised, and it would inherit the per-volume BSS
cost. That is a deliberate decision to defer, not an oversight.

There is a second reason to be careful. Reclaiming a leak means clearing a bit in
the allocation bitmap, and the bitmap is exactly what a torn write leaves
inconsistent. Clearing bits is the one operation here that can destroy data
rather than reveal it: if a sector is marked used because an inode exists that
the tree walk could not reach, freeing it discards that inode's data. A repair
has to decide what an unreachable-but-present inode means, and that decision
belongs in a document rather than in a first implementation.

The order that makes sense is: get the report in place, use it to find out
whether leaks and orphans actually occur in practice, and only then decide what a
repair should do about them.

## Test coverage

`tests/host/sxfs_volume_test.cpp` runs the real driver against in-memory
backends, which is what makes the negative cases possible:

- a freshly built image reports no findings of any kind
- `check()` leaves the image byte-identical, and two consecutive reports match
- five injected leaked blocks are reported as exactly five, and the volume
  **still mounts**, which is the point: this defect is invisible to the mount path
- one orphaned inode is reported, and again the volume mounts

The last two are the cases that matter. A checker that only ever runs against
healthy volumes has not been tested.
