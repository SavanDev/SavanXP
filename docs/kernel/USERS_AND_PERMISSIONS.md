# Users and permissions: one user, and elevation when it is needed

> **Status: design, nothing implemented.** This document fixes the model before
> any code is written, because it moves three things that are expensive to move
> twice: the SxFS inode, the `stat` ABI, and who is allowed to talk to
> `/dev/input0`. The phases at the end are the order to build it in.

## What the user sees

The model is the one Windows NT gives an ordinary person, not the one it gives an
administrator of a domain:

- **One account, used every day, without special rights.** Programs started from
  the desktop run as that account. They can read the system, and write only the
  user's own files.
- **Elevation when something needs it.** Installing into the system, changing
  system settings, touching another user's process: the program asks, the system
  shows a confirmation the program cannot fake, and if the user accepts, *a new
  process* runs with administrator rights. The program that asked never gains
  them itself.

This is User Account Control's "admin approval mode". It is what is taken from
NT. What is **not** taken is the machinery underneath it: SIDs, ACLs with ordered
entries, inheritance flags, the privilege list, integrity levels. With two kinds
of identity (the user and the system) an ACL says nothing that an owner, a group
and a mode cannot, and costs far more on disk and in the VFS.

So: NT's experience on top, Unix's owner/group/mode underneath. The second is
also what POSIX programs already expect.

## What exists today

`KERNEL_SECURITY.md` states it: there are no users, UIDs or credentials, and every
process runs with the same authority.

- **Processes** carry no identity. Init and idle cannot be killed; a mutating
  `ioctl` needs a descriptor opened for writing. That prevents accidents, not
  one application acting on another.
- **SxFS** stores no owner and no permission bits. `struct sxfs_inode` has a type,
  a size, a link count and extents. `struct savanxp_stat` carries `st_mode`
  (the file type only) and `st_size`.
- **The POSIX layer** answers 0 to `getuid`/`geteuid`/`getgid`/`getegid`, keeps
  `umask` without applying it, and `getpwuid` returns a made-up `root`.
- **The in-memory tree** (`/dev`, and the initramfs under `/rescue`; the
  system itself runs from the disk at `/`, see
  [`FILESYSTEMS.md`](FILESYSTEMS.md#the-root-is-the-system-disk)) is writable in
  RAM. Opening an initramfs file for writing copies it into one of 32 dynamic
  files of 16 KiB; the change lasts until reboot. Any process can do it,
  `/rescue/bin/init` included.
- **Devices** under `/dev` are open to every process. `/dev/input0` in particular:
  any program can read the keyboard, which matters for the elevation prompt below.

## The identities

| identity | uid | gid | who |
| --- | --- | --- | --- |
| system | 0 | 0 | init, `windowd`, the services init starts, and elevated processes |
| the user | 1000 | 100 (`users`) | the desktop and everything launched from it |

One user. Multi-user (a second account, a login screen, fast user switching) is
out of scope, and the format below does not prevent it later.

## The token

Each process has a **token**: `{real uid, effective uid, gid, elevated}`. It is
set when the process is created and never changes afterwards; there is no
`setuid()` that raises it.

- `spawn`, `fork` and `exec` inherit the parent's token unchanged.
- An elevated process has `real uid = 1000`, `effective uid = 0`. `getuid()` still
  says who the person is; `geteuid()` says it acts as the system. Files it creates
  belong to the system, as files an elevated installer creates belong to
  Administrators on NT.
- Only a process whose effective uid is 0 can create a process with a different
  token. In practice that is one process: the elevation broker.

**There is no setuid bit.** NT has none either, and elevation replaces it: a
program that needs rights asks for them through the broker instead of carrying
them in its file mode. `S_ISUID` and `S_ISGID` are stored if a port sets them and
ignored at `exec`.

The token belongs in the process, beside the handle table. The handle model in
[`KERNEL_NATIVE_API.md`](KERNEL_NATIVE_API.md#handle-model-decided) already
checks `granted_access` at use time; the token decides what access an `open`
grants, and the existing check enforces it from there.

## Files

### What each inode stores

Owner uid (32 bits), group gid (32 bits) and mode (16 bits: type plus `rwx` for
owner, group and others). Modification time is worth adding at the same time,
because `stat` needs it and it is the same format change.

**This fits in SxFS v2.** `struct sxfs_inode` has 46 bytes unused: `reserved0`
(16 bits, right after `type`) and `reserved[44]`. Neither the kernel driver nor
`libsxfs` validates them, and both modify an inode in place, zeroing it only when
the inode is allocated or freed. So:

- A superblock feature flag (`SXFS_FEATURE_OWNERSHIP`, in `flags` or in the
  reserved tail) says the fields are meaningful.
- **A volume without the flag is read as before**, with every inode getting the
  default ownership of its path (below). Nothing on it is lost. That is the
  requirement for `build/disk.img`: Doom installed today has to still be there
  and still run.
- The first writable mount of such a volume by a kernel that knows the flag
  stamps the defaults into every inode and sets the flag, in one journalled
  commit. `fscheck` learns to check the fields only when the flag is set.
- An older kernel mounting a stamped volume ignores the fields. New inodes it
  creates are zeroed, and mode 0 means "no ownership recorded": the newer kernel
  applies the default again. A downgrade loses permissions on new files only.

No format version bump. [`SXFS_ROADMAP.md`](../fs/SXFS_ROADMAP.md) keeps all
planned work inside v2, and this keeps that true.

### Default ownership

Applied to unstamped volumes, to inodes with mode 0, and by the host tools that
build the image:

| path | owner | mode |
| --- | --- | --- |
| `/users/<name>` and below | the user | `0700` dirs, `0600` files |
| `/tmp` | system | `1777` |
| everything else on the disk (`bin`, `lib`, `games`, icons, wallpaper, the root itself) | system | `0755` dirs, `0644` files, `0755` for anything under `bin`/`lib` |

`/users/<name>` is the user's profile, NT's `Users\<name>`. It does not
exist yet. Programs that save today (Paint, the editors) write wherever they are
pointed, often the root itself, and in phase 4 that starts failing for an
unelevated process. Moving their default save location into the profile is part
of that phase, not an afterthought.

`tools/sxfs_sync.py` and `libsxfs` install files from the host. They write the
default for the destination path, so an image built on the host is already
stamped. `sxfs_sync.py` also compacts an image by rebuilding it; that copy has to
carry the three fields across, or a compaction silently resets every file a user
or an elevated installer created to its default.

### The in-memory tree

The initramfs is CPIO `newc`, which carries mode, uid and gid per entry.
`mkinitramfs.py` already writes the mode (`0755` under `bin/`, `0644`
elsewhere). It writes uid 0, gid 0, and the VFS starts reading all three
instead of only the file type.

Writing to an initramfs file, or creating a file in the in-memory tree, becomes a
system-only operation. Nothing in the tree does it today except tests, which run
as system. `/rescue` is the system's in every case: it is what boots when the
disk does not, so nothing the user runs may change it.

### Devices

`device::register_node` gains an owner and a mode. Most nodes stay open to the
user (`/dev/gpu0` through `windowd`, `/dev/audio0` through `audiod`, the
clipboard, `/dev/power` for shutting down). The ones that must not be:

- **`/dev/input0` and `/dev/mouse0`: system only, read by `windowd`.** If any
  process can read the keyboard, it can read a password; if any process can
  write input, it can click "Yes" on the elevation prompt. The prompt below is
  worth nothing without this.
- Raw block devices, if they ever get a node.

### The check

One function in the VFS, `vfs::check_access(token, vnode, want)`, called before a
driver is reached. It belongs with the rules `FILESYSTEMS.md` already lists as
"enforced for every driver", so a FAT or ISO driver inherits it without
repeating it. The semantics are Unix's: owner bits, else group bits, else other
bits; effective uid 0 passes read and write, and passes execute only if some `x`
bit is set.

## Elevation

Three parties, and the order matters:

1. The program asks the **broker** (a system service init starts) to run
   something elevated: a path and arguments, over a channel it can open.
2. The broker asks **`windowd`** to show the confirmation: the program's name,
   and Yes/No.
3. `windowd` draws it **on its own layer, above every client**, and takes the
   answer only from real input it reads from `/dev/input0`. No client can
   cover the dialog, draw something that looks like it, or send it input.
4. If the user says yes, the broker spawns the program with an elevated token.
   The program that asked gets back the pid, never the token.

That is UAC's secure desktop, reduced to what a compositor that already owns
the screen and the input can do. Without step 3 the model is decorative: a
program could approve its own request.

There is no password at first. The one user is the administrator, and the
confirmation is consent, not authentication, as it is for an NT administrator in
approval mode. A password belongs with a second user, not before.

### What requires elevation

| operation | why |
| --- | --- |
| writing system files: `/bin`, `/lib`, global settings | installing and uninstalling |
| killing a process that is not yours | services, `windowd` |
| setting the system clock | shared by every process |
| raw devices, PCI configuration | they bypass every check above |
| `fscheck` repair, formatting | the whole volume |

Shutdown stays unelevated, as it is on a desktop NT. Reading the system
(`/bin`, logs, the process list) stays unelevated.

## POSIX, as a consequence

| call | becomes |
| --- | --- |
| `getuid`, `geteuid`, `getgid`, `getegid` | the token |
| `stat` | `st_uid`, `st_gid`, full `st_mode`, `st_mtime` (an ABI change to `savanxp_stat`) |
| `chmod`, `fchmod`, `chown` | implemented; `chown` needs effective uid 0 |
| `access` | answers with the real uid, as POSIX says |
| `umask` | applied on create |
| `getpwuid`, `getpwnam` | the two identities above, from a fixed table at first |
| `setuid`, `seteuid` | lowering only (an elevated process may drop to the user); raising is `EPERM` |

`struct stat` changing is the reason this has to come before
[`libposix.so`](SHARED_LIBRARIES.md): once it is shared, that change breaks every
installed binary instead of being fixed by a rebuild.

## Phases

1. **The format.** Ownership fields and the feature flag in SxFS, the default
   table, stamping on first mount, `fscheck` support, `sxfs_sync.py` and
   `libsxfs` writing the defaults. *Done when* an existing `build/disk.img`
   mounts, is stamped, and `verify_doom_persistence.sh` passes.
2. **Tokens and the check.** The token in the process, inheritance,
   `vfs::check_access`, device owners, the POSIX calls above. **Everything
   still runs as system**, so behaviour does not change. *Done when* the smokes
   pass, and a test process spawned as uid 1000 is refused a write to `/bin`
   and allowed one in its profile.
3. **The broker and the prompt.** The service, the `windowd` layer, input
   ownership. *Done when* a scenario asks for elevation, answers through real
   input, and gets an elevated child; and a client that synthesizes the answer
   is ignored.
4. **The desktop runs as the user.** Init starts `windowd` and the services as
   system and the shell as uid 1000. Programs that write outside the profile
   move their defaults into it; installers ask for elevation. *Done when* the
   full smoke set passes with the shell unelevated.

## Deliberately out of scope

- **ACLs.** The inode keeps room (the reserved tail is not used up by the fields
  above), but nothing is built until a second user or a shared folder needs one.
- **More than one user**, login, switching users, passwords.
- **Capabilities or privileges** split out of "elevated": one bit until a case
  needs two.
- **Network identity.** Sockets need no permission yet. Binding a low port
  could be made system-only later, if a server ever runs on the system.

## Open questions

- The profile path: `/users/<name>` (NT's `Users`) or `/home/<name>`
  (Unix's). This document uses the first.
- The user's name, and whether the installer or first boot asks for it, or it is
  fixed until there is a second user.
