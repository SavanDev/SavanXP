# SxFS namespace mutations

Creating, deleting and renaming change a directory, an inode and the
allocation bitmaps together. This page is how SxFS v2 makes each of those one
atomic step across a power cut, what a mount finishes when a step was
interrupted, and what a full volume can still do.

The journal ([`SXFS_JOURNAL.md`](SXFS_JOURNAL.md)) covers the bitmaps and the
inode table. Directory contents are file data: they are not in its payload, so
a directory changed in place before the commit was not rolled back with it. A
cut between the two left an inode nothing named, or a rename with neither name.
The rules below close that without changing the journal or the layout.

The code is in [`kernel/sxfs.cpp`](../../kernel/sxfs.cpp): `store_directory`,
`find_free_run`, `remove_status`, `rename_status`, `doom_in_place`,
`finish_doomed`.

## Rule 1: a directory is never changed in place

A mutation reads the directory into memory, changes it there, and writes the
result to **newly allocated blocks**. The directory's inode then points at the
copy and the old blocks are freed, all inside the mutation's metadata
transaction:

1. the copy is written (cached);
2. the commit's first ordering point puts it on the medium, before the journal
   header;
3. the transaction replays or it does not: the inode with the new extents, the
   bitmap with the new blocks taken and the old ones freed, and whatever else
   the mutation did (a new inode, a freed one) land together.

A cut before the header leaves the old inode and the old blocks, which nothing
wrote to. A cut after it replays to the new inode and the new blocks, which
are complete. A rename across two directories writes two copies and commits
once, so the name is in exactly one of them at every cut.

The copy is packed: free slots are dropped, so a directory's size is its live
entries and an empty directory holds no blocks. Its `mtime` is set by the same
commit, which is what makes "did anything in `/bin` change?" one `stat`.

A directory of *n* entries costs `ceil(n * 80 / 512)` sectors per mutation: one
sector up to 6 entries, 16 for the ~100 of `/bin`, 640 at the format's ceiling
of 4095.

## Rule 2: a transaction never writes into blocks it frees

The allocator gives a mutation only sectors that are free **now and were free
when the mutation started** (`busy_word`: the live bitmap OR the snapshot).
Blocks the transaction frees still belong to the state before it until the
commit is down, so nothing may be written there first.

This is what makes rule 1 hold when a mutation frees and allocates at once, and
it reaches file data too:

- `unlink` followed by a create and a write never puts the new file's bytes
  where the old file's still are, whichever commits a cut loses;
- a write that truncates (`write(..., truncate)`) puts the new contents beside
  the old ones and frees the old ones in the same commit, so the file is the
  old bytes or the new bytes, never a mix.

An overwrite inside a file's current size is still in place and has no such
guarantee: SxFS journals metadata, not file contents.

**Without room beside the old contents**, a truncating write commits the
truncation first and then writes into what it freed: the file can then be
empty across a cut. It is the only case, and it needs a volume with less free
space than the file being rewritten.

## Rule 3: file data leaves room for directories

File data never takes the last 128 sectors of free space
(`kDirectoryReserveSectors`). When a write reports the volume full, there is
still room for the copy of a directory of up to 819 entries, so a full volume
can create an empty file, rename and delete.

## Growing a file

A file has at most eight extents, and the allocator spends them carefully:

1. the sectors right after the file's last extent are taken first, which
   extends that extent instead of adding one;
2. then the first free run that holds everything still missing, else the
   longest run there is, repeatedly;
3. when that fails although the volume has room (the file is out of extents,
   or the free space is in more pieces than it has extents left), the file is
   **consolidated**: its contents are copied to one contiguous run and the old
   extents are freed, in the same transaction as the write that needed the
   room (`consolidate_inode`).

Step 3 is what two files growing side by side need. Each growth lands after
the other file's, so each is a new extent, and with growth doubling from 64
sectors the eighth extent is reached at 4 MiB: before consolidation the next
write failed with "no space" on a volume that was mostly empty. The copy costs
one read and one write of the file, once per doubling of its size.

Consolidation follows rule 2: the new run is never among the sectors being
freed, so a power cut during it leaves the file whole in its old extents or
whole in the new run. `sxfs-vfs-test` grows two files in turns to 6 MiB each
and cuts the append that moves one at 49 points across its 8282 sectors.

What still fails with room left is a file larger than every free run on a
volume fragmented into more than eight pieces. That is the eight-extent limit
itself, and it is v3's to lift.

## Deleting on a full volume

Rule 3 is a policy of this driver, not of the format: a volume written by the
host tools, or one with large or many directories, can have no room for a
copy. Deleting must still work there, because it is how room is made.

When the parent's copy cannot be allocated, the deletion is done in three
steps, each of which leaves a state a mount completes:

| step | on disk afterwards |
| --- | --- |
| 1. One commit sets the inode's `link_count` to 0 and frees its blocks | the inode is **doomed**; its entry is still in the parent |
| 2. The entry's slot is cleared in place | a doomed inode nothing names |
| 3. One commit frees the inode | the deletion is complete |

Step 1 is the point of no return. Step 2 is one single-sector write: an entry
starts at a multiple of 16 bytes, so the 16 bytes that mark a slot free
(`SXFS_DIR_ENTRY_HEAD_BYTES`) never cross a sector, and there is no state to
protect because the entry already means nothing.

## Doomed inodes

`SXFS_FEATURE_LINK_COUNT` (superblock feature bit 2) says `link_count` is
meaningful. With it:

- an allocated inode with `link_count == 0` is **doomed**: its deletion is
  committed;
- an entry naming a doomed inode is not part of the tree. Every reader skips
  it: the kernel's tree, `fscheck`, and the host tools;
- a writable mount finishes the job (`finish_doomed`): it clears the slots that
  still name doomed inodes, then frees all doomed inodes in one commit. In that
  order, so a cut in the middle leaves doomed inodes nothing names and never an
  entry naming a free inode.

`fscheck` reports them as `pending_deletions`, which is not a finding.

Without the bit the field is not trusted and every allocated inode is live. A
writable mount of such a volume writes 1 into every allocated inode that holds
0, commits, and only then sets the bit: the bit lives in the superblock, which
is not journalled, and a bit that reached the disk before the counts would
doom whatever still held a 0. `sxfs_format` sets it on new volumes. Every
creator has always written `link_count = 1`, so in practice the adoption only
sets the bit.

A reader that predates the bit sees a doomed inode as an empty file (while its
entry exists) or as an unreachable inode, which the kernel tolerated and the
host tools refuse whole. Neither loses anything.

## A file unlinked while open

When something still holds the file (a handle, a section), `unlink` and a
rename over it remove the name in the usual commit and, in that same commit,
doom the inode instead of freeing it: `link_count` 0, blocks kept. The holders
read and write it as before. When the last one goes, `release_unlinked` frees
the blocks and the inode in one commit.

A power cut in between leaves a doomed inode nothing names, with its blocks:
the case `finish_doomed` already handles, so the next writable mount frees
it. The name never comes back. Until then `fscheck` counts it in
`pending_deletions`, and the space it holds is used, not free.

A volume without `SXFS_FEATURE_LINK_COUNT` cannot record this, which only
happens on a read-only mount, where nothing is unlinked.

## What is guaranteed

For create, mkdir, unlink, rmdir, and rename within a directory, across
directories, of a directory with its contents, and over an existing file or
empty directory:

- a power cut at any sector, followed by a mount, shows the whole tree exactly
  as before the mutation or exactly as after it;
- a rename shows the object under exactly one of its two names; a replacing
  rename shows the old target under its name or the new object under it, and
  frees what was replaced in the same commit;
- no other file or directory changes, `fscheck` reports nothing, and no
  deletion is left pending;
- a second cut during the recovery recovers to the same answer.

An I/O error returned to the caller leaves the driver's memory at the state
before; the commit that follows writes every metadata sector, so disk and
memory agree again ([`SXFS_JOURNAL.md`](SXFS_JOURNAL.md#knowing-what-changed)).
Until that commit, a reboot may show either state, as with a power cut.

Create and rename fail with "no space" when a copy cannot be allocated; they
never fall back to changing a directory in place.

## Tests

`tests/host/sxfs_namespace_test.cpp` (`sxfs-namespace-test`) runs the real
driver against the fake device of [`SXFS_JOURNAL.md`](SXFS_JOURNAL.md#tests),
which lands cached writes in reverse order. For each of fourteen mutations it
cuts the power after every sector the mutation writes, then recovers once
cleanly and again with a second cut at seventeen points of the recovery, and
requires a writable mount, a clean `fscheck`, no pending deletion and a tree
that matches one of the mutation's permitted states file by file and byte by
byte. The matrix runs on four volumes: mostly empty, filled through the driver
until file data is refused, with not a sector free, and fragmented into
one-sector holes; and once more with an I/O error at every write instead of a
cut.
