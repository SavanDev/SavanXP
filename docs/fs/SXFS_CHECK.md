# SxFS checking, recovery and repair

Three separate things, kept separate on purpose:

- **Checking** answers "what does the volume think it is holding". It never
  writes: `fscheck` in the guest, `sxfs-cli check` on the host.
- **Recovery** finishes what an interrupted mount left: a pending journal
  transaction, the clean mark. Its outcome is fully determined by the image.
  A writable mount does it, and so does `sxfs-cli recover`.
- **Repair** changes a damaged image so it validates again. It is offline, on
  the host, on a copy: `sxfs-cli repair`.

For what is left to do in SxFS, see
[`SXFS_ROADMAP.md`](../roadmaps/open/SXFS_ROADMAP.md).

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
  total_sectors       2097152
  data_lba            3077
  data_sectors        2094075
  superblock_seq      20
  clean_shutdown      yes
  journal_valid       no
  journal_pending     0
population:
  inodes_allocated    103
  files               98
  directories         5
block accounting:
  data_used           59575
  data_claimed        59575
metadata cost:
  commits             19
  bytes_written       29933056
  bytes_per_commit    1575424
reconciliation:
  used == claimed (balanced)
findings:
  none

result: clean
```

The `metadata cost` block is what makes the journal's price a measurement rather
than an inference. The sample above predates sparse journals, when every commit
copied the whole metadata; since then a commit writes only the sectors it
changed, and the same smoke reports `bytes_per_commit 3872`. See
[`SXFS_JOURNAL.md`](SXFS_JOURNAL.md#what-it-costs).

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

`pending_deletions`, in the population block, is not a finding. It counts
inodes whose deletion is committed and not finished: a file unlinked while
still open, or a deletion a power cut interrupted. The last close or the next
writable mount frees them ([`SXFS_NAMESPACE.md`](SXFS_NAMESPACE.md#doomed-inodes)).

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

## Looking without touching

Nothing that only looks writes, on either side:

- `sxfs::probe` (what `fs::mount` calls to recognise a device) reads the
  superblocks, the metadata and the journal, replays a pending transaction
  **in memory**, validates the result and stops. The device is byte for byte
  what it was.
- A read-only mount (`fs::attach(mount, /*read_only=*/true)`,
  `SXN_STORAGE_MOUNT_READ_ONLY`, the "Read-only" box of Disk Management's
  Mount) publishes that recovered view and never writes: not the recovery,
  not the link counts, not the ownership defaults, not a sync. Every change
  through the VFS is `EROFS`
  ([`FILESYSTEMS.md`](../kernel/FILESYSTEMS.md#read-only-mounts)).
- `sxfs_open` in `libsxfs` does the same replay in memory, so `sxfs-cli
  check`, `info` and `extract` read an interrupted image as recovered and
  leave the file alone. `check` says what is pending.

A writable `attach` is the one place a mount writes: it persists the recovery
(replayed metadata home, clean superblocks, header cleared, in that order),
then adopts link counts, finishes doomed inodes and stamps ownership. If the
device refuses the recovery the volume stays read-only on the recovered view.

## Recovery on the host

`sxfs-cli recover <image>` persists what `sxfs_open` recovered, with
`sxfs_recover`, in the same order as the kernel. The journal stays pending
until everything it describes is home, so it is idempotent: stopped after any
of its writes and run again, it produces the same image, byte for byte. It
writes only a state that validates; an image whose replay does not validate
is repair's business.

`apply` and `rm` call it first and say so on stderr. They have to: a journal
left pending would be replayed over whatever they wrote, which is also why
`sxfs_flush` refuses while `ctx->recovery` is set.

`check` reports, without failing: a pending transaction, a superblock not
marked clean (or two copies that differ), a journal header that is not one
(ignored, as the kernel ignores it), and sectors marked used that nothing
claims.

## Offline repair

```text
sxfs-cli repair [--dry-run] <image>
```

It works on a copy (`<image>.repair`) and replaces the image, with one
`rename`, only when the copy is complete. The original stays as
`<image>.before-repair`, a hard link taken before the rename. An interrupted
repair therefore leaves the image as it was, and running it again starts over
from the original; an existing backup that is not the image itself is never
overwritten. `--dry-run` reports and discards the copy. The exit status is 0
only when the result passes the ordinary check and nothing was left
unresolved.

What `sxfs_repair` does, in this order:

1. **Inodes.** An inode marked allocated with no record is released; a record
   left in a free inode is cleared. An allocated inode whose record
   contradicts itself (wrong id, bad extent, size beyond its capacity) is
   *damaged*: counted and not touched.
2. **The tree, from the root.** An entry is removed when it is malformed,
   names a free inode, is a second name for an inode already reached (an alias
   or a cycle), or repeats a name in its directory. That loses a name, never
   contents. An entry whose type disagrees with its inode is corrected.
3. **Blocks.** The bitmap is rebuilt from what the inodes claim: a claimed
   sector that is free is marked; a marked sector nothing claims is freed.
4. **Committed deletions** (doomed inodes) are finished.
5. **Orphans.** Every allocated inode nothing reaches gets a name under
   `/lost+found`: `inode-<id>` for a file, `dir-<id>` for a directory. A lost
   subtree is attached once, at its top, so the names inside it survive. Only
   the new entry is written; the contents are never copied or moved.

Step 3 comes before step 5 so that `lost+found` is never allocated over a
file whose sectors the bitmap had lost, and orphans are rescued rather than
freed: an unreachable inode with valid extents is somebody's data until a
person says otherwise.

What it leaves, and reports as `NOT REPAIRED`, is damage the image does not
attribute:

| Left as found | Why |
| --- | --- |
| a damaged inode | its record cannot say what it owns |
| unclaimed sectors, while any inode is damaged | they may be that inode's |
| a sector two inodes claim | either could be the owner; no doomed inode is freed either, since its sectors could be the other's |
| orphans, when `lost+found` cannot be made | no room, or the name is a file |

The same damage always produces the same image and the same report, and
repairing a repaired image changes nothing.

`fscheck` stays a report. Repair needs a volume nobody is using and room for
a copy, which the host has and a running system does not.

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

`tests/host/sxfs_recovery_test.cpp` (`sxfs-recovery-test`) links the kernel
driver, the real VFS and `libsxfs` over the same images:

- a power cut after every sector of a series of mutations: the host opens
  each crashed image without writing a byte, sees the tree the kernel mounts
  it to, and `sxfs_recover` stopped after each of its writes and repeated
  gives the same image;
- a pending journal under `probe` and a read-only mount: nothing written, the
  image compared byte for byte, eight kinds of change refused with `EROFS`,
  and the writable mount that follows persists the recovery;
- repair of an image with a lost file, a lost directory with a file in it, a
  dangling entry, leaked sectors, claimed sectors marked free and a stale
  record: contents read back from `lost+found`, the kernel mounts it with a
  clean `fscheck`, the result is deterministic and idempotent;
- cross-linked sectors and a damaged inode: reported, and left.
