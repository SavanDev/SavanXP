# File locks

Byte-range locks on files: one table in the kernel (`kernel/file_lock.cpp`),
two owner policies over it, one per subsystem that asks: POSIX's `fcntl`
record locks and SUN's `sxn_lock_file`.

## What a lock is

A lock covers the bytes `[first, last]` of one file, the `vfs::Vnode`, which
is unique per file across volumes ([FILESYSTEMS.md](FILESYSTEMS.md)). It is
**shared** or **exclusive**. Two locks conflict when they overlap, at least
one is exclusive and the policy below says their owners differ. A range may
run past the end of the file: `last` is `file_lock::kToEnd` for "to the end
and beyond", which is what POSIX's `l_len = 0` means.

Locks are **advisory**. `read`, `write` and the mappings never look at them,
so a program that does not ask is never stopped. Windows' locks are
mandatory; SavanXP does not implement that part.

## Two policies

| | POSIX (`Policy::process`) | SUN (`Policy::open_file`) |
|---|---|---|
| Owner | the process | the open file (the `IoObject`), shared by its duplicates and by inheritance |
| Its own locks | never conflict: a new lock replaces, splits and merges its ranges | conflict like anyone else's: not reentrant |
| Released | when the process closes **any** descriptor of the file, at exit, by `F_UNLCK` over any range | when the open file is destroyed (its last handle closed), or by an unlock of exactly a range it locked |
| A child of `fork` | holds none | shares the parent's, through the shared open file |
| Waiting | `F_SETLKW`; `EDEADLK` for a cycle | without `SXN_LOCK_FAIL_IMMEDIATELY`; no deadlock check, as on Windows |

The POSIX column is POSIX's own rule, the one every Unix shares, including
its best-known trap: a library that opens and closes the file behind the
program's back drops the program's locks. The SUN column is `LockFileEx`'s.
Both live in one table, so a POSIX program and a SUN one locking the same file
see each other.

The hooks that release them sit where handles go away, whatever the
subsystem:

- `sx::close_handle`, the replacement of a slot in `install_handle` (`dup2`),
  and `release_all_handles` at exit: `file_lock::handle_dropped` releases the
  process's POSIX locks on that file.
- `destroy_io_object`: `file_lock::open_file_destroyed` releases the open
  file's.
- `reset_process_slot`: `file_lock::process_ended`, a safety net. Every path
  that ends a process has released its handles already; this keeps a lock
  from outliving its owner on one that has not.

## Waiting

A thread whose lock is taken parks with `WaitReason::file_lock`, with what it
asked for in `Thread::lock_request` and a ticket. Nobody rechecks it on the
tick: whoever frees bytes of that file (an unlock, a close, an exit, a POSIX
lock turned from exclusive to shared) calls `grant_waiters`, which walks the
parked threads oldest ticket first and grants every request that no longer
conflicts. A grant applies the lock for the waiter and completes its call
with 0; the walk starts over when a grant freed bytes itself. A waiter whose
process dies leaves nothing behind: the request lives in its thread.

There is no timeout and no signal to interrupt the wait (the kernel has no
signal delivery). A POSIX wait that would close a cycle is refused instead:
before parking, the kernel follows the owners of the locks in the way to the
threads they have parked, and the locks in *their* way, up to 16 steps, and
returns `EDEADLK` when the chain comes back to the caller. Of two processes
that each hold what the other asks for, the one that asks second gets it.

## Limits

- 128 locks for the whole system (`file_lock::kMaxLocks`). A request that
  needs a slot (a new lock, or splitting one) when none is left fails with
  `ENOLCK` and changes nothing.
- Every lookup walks the table. That is right for a handful of programs
  locking a few ranges each, and wrong for a database.
- Only files have locks. A pipe, a device or a socket is `EINVAL` in POSIX
  and `EBADF` in SUN.

## POSIX

`fcntl(fd, F_GETLK | F_SETLK | F_SETLKW, struct flock*)` reaches the kernel as
`SAVANXP_SYS_FCNTL` with a `struct savanxp_flock` of fixed widths. The kernel
resolves `l_whence` against the descriptor (its offset for `SEEK_CUR`, the
file's size for `SEEK_END`), so the range is computed with the state the lock
sees. A read lock needs the descriptor open for reading and a write lock one
open for writing (`EBADF`). `F_GETLK` reports the first conflicting lock of
another owner, with `l_whence = SEEK_SET`, `l_len = 0` for one that runs to
the end, and the owner's pid; or `l_type = F_UNLCK`. `locktest`, in
`smoke smoke`, checks each of these rules with two processes.

## SUN

`sxn_lock_file(handle, offset, length, flags)` and `sxn_unlock_file(handle,
offset, length)` (`SXN_SYS_LOCK_FILE`, `SXN_SYS_UNLOCK_FILE`,
[KERNEL_NATIVE_API.md](KERNEL_NATIVE_API.md)) take a regular file's handle
open for reading or writing and an offset and a length, as `LockFileEx` does;
a length of 0 is refused instead of locking nothing. The unlock names exactly
a range that was locked. `nativetest` checks the rules with two opens of one
file, which are two owners inside one process, and a thread that waits.

## Pending

- The Linux subsystem's `fcntl` and the Win32 subsystem's `LockFileEx` do not
  reach the table yet.
