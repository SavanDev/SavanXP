# Users and permissions: one user, and elevation when it is needed

> **Status: built, phases 1–4.** Every process carries an identity and the
> VFS checks it on every operation; the desktop, and everything started from
> it, runs as the user, and what needs the system's rights goes through the
> elevation prompt. The record of installed programs is what is left (see
> phase 4).
> This document fixed the model before the code, because it moves three things
> that are expensive to move twice: the SxFS inode, the `stat` ABI, and who is
> allowed to talk to `/dev/input0`. The phases at the end are the order to build
> it in.

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

## What existed before

Before phase 1 there were no users, UIDs or credentials, and every process ran
with the same authority. What changed, and what is still true:

- **Processes** carry a token (phase 2, `kernel/credentials.hpp`). init and
  the services it starts are the system's; since phase 4 `sessiond` starts the
  desktop as the user, and every program launched from the desktop inherits
  that.
- **SxFS** records uid, gid, mode and modification time on every inode, and the
  VFS publishes them on the vnode it checks.
- **The POSIX layer** answers with the token, applies `umask` on create, and
  `stat` reports ownership and `st_mtime`.
- **The in-memory tree** (`/dev`, and the initramfs under `/rescue`; the
  system itself runs from the disk at `/`, see
  [`FILESYSTEMS.md`](FILESYSTEMS.md#the-root-is-the-system-disk)) is writable in
  RAM, by the system only: opening an initramfs file for writing copies it into
  one of 32 dynamic files of 16 KiB, and the change lasts until reboot.
- **Devices** under `/dev` are 0666 (0444 when read-only), except
  `/dev/input0` and `/dev/mouse0`, which are the system's only (0600).
- **`/system/programs.txt`** is the build's list of the programs it shipped, and
  Add or Remove Programs trusts it to tell the system from what was installed
  ([`SXE_FORMAT.md`](../userland/SXE_FORMAT.md)). It is the system's, so only
  the system can rewrite it.
- **Settings lived at the root of the disk** until phase 4 moved them into the
  profile ([below](#settings-the-users-and-the-machines)).

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
  token (`SXN_SPAWN_FLAG_IDENTITY`,
  [`KERNEL_NATIVE_API.md`](KERNEL_NATIVE_API.md#slice-26-identities-on-ports-and-spawn)).
  In practice that is one process: the elevation broker.

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

| field | where | what |
| --- | --- | --- |
| `mode` | `sxfs_inode`, offset 6 (was `reserved0`) | 16 bits, POSIX-style: the type bits (`040000` directory, `0100000` file) plus `07777` |
| `uid`, `gid` | `sxfs_inode`, offsets 84 and 88 (from `reserved[44]`) | 32 bits each |
| `features` | `sxfs_superblock`, offset 72 (from `reserved[440]`) | `SXFS_FEATURE_OWNERSHIP` (bit 0) |

36 bytes of the inode stay reserved. Modification time is not in it yet: keeping
it current dirties the inode on every write, and that cost belongs with `stat`
in phase 2, measured against `bytes_per_commit`
([`SXFS_ROADMAP.md`](../fs/SXFS_ROADMAP.md)).

**It fits in SxFS v2, with no version bump.** Neither the kernel driver nor
`libsxfs` validated the reserved bytes, and both modify an inode in place,
zeroing it only when it is allocated or freed. So:

- **The feature bit is apart from `flags`.** A mount rewrites `flags` whole on
  every commit (clean or dirty), which would clear a feature kept there.
- **Mode 0 means "no ownership recorded"**, and is never a recorded mode: the
  type bits are always set. A recorded mode whose type bits disagree with the
  inode's type is a defect.
- **A volume without the feature is read as before.** The first writable mount
  by a kernel that knows it gives every reachable inode the default of its path
  (`stamp_ownership` in `kernel/sxfs.cpp`) and sets the feature, in one
  journalled commit. A read-only mount applies the defaults in memory only.
  `sxfs-cli apply` and `rm` do the same on the host (`sxfs_stamp_ownership`),
  so `./build.sh build` stamps `build/disk.img` before QEMU ever sees it.
- **A stamped volume with zeroed inodes** — new files an older kernel created,
  which leaves the fields at zero — gets their defaults on the next mount. A
  downgrade loses ownership on new files only.
- **New inodes get the default of their path** (`prepare_new_inode`,
  `init_inode` in `libsxfs`), since there are no tokens yet to say who created
  them.
- **`fscheck` and `sxfs-cli check`** count reachable inodes with mode 0 or a
  mode that disagrees with their type, only when the feature is set
  (`ownership_recorded`, `unowned_inodes` in `savanxp_fscheck_report`, which
  took two reserved fields: the struct's size did not change).

The table lives in `include/sxfs/sxfs_ownership.h`, freestanding, so the kernel
and the host tools apply the same one.

### Default ownership

Applied to unstamped volumes, to inodes with mode 0, and by the host tools that
build the image:

| path | owner | mode |
| --- | --- | --- |
| `/users/<name>` and below | the user | `0700` dirs, `0600` files |
| `/tmp` | system | `1777` |
| `/system` and below | system | `0755` dirs, `0644` files |
| everything else on the disk (`bin`, `lib`, `games`, icons, wallpaper, the root itself) | system | `0755` dirs, `0644` files, `0755` for anything under `bin`/`lib` |

`/system` is the machine's: `programs.txt` today, and later the registry of
installed programs and the machine-wide settings. Being system-owned is what
makes `programs.txt` a list Add or Remove Programs can trust; until phase 2,
the check that enforces it does not exist.

### Settings: the user's and the machine's

NT splits them into `HKEY_CURRENT_USER` and `HKEY_LOCAL_MACHINE`. Every
setting the system has today turned out to be the user's, as on NT, where the
volume and the keyboard layout are per user too:

| file | what |
| --- | --- |
| `progman.ini`, `desktop.cfg`, `mines.ini` | menu arrangement, wallpaper, best times |
| `assoc.ini`, `mimeicon.ini` | file associations and icons, hand-written |
| `keyboard.cfg`, `audio.cfg` | the layout and the volume; init applies them at boot from the one user's profile |

They live in the profile, `/users/<name>`, NT's `Users\<name>`.
`savanxp_settings_path()` in the runtime says where: the profile of the real
uid, so an elevated program finds the same one, and for reading, when the
profile has no copy yet, the old one at the root, so an existing image keeps
its menu and wallpaper until the first save moves them. The system's profile
is the root itself, which is why the smokes, which run as the system, read and
write where they always did. `getenv("HOME")` answers the same directory.
`/system` holds no setting yet; it is where a machine-wide one would go, and
writing it would take elevation.

init makes `/users/user` at boot (0700, the user's) if it is missing. The
desktop works in it, so a file saved under a bare name lands there; Paint and
Notepad offer it for a new document, and Files opens on it. Doom keeps its
settings and saves in `$HOME/doom`; the game's directory, `/games/doom`, is
the system's, and saves an older Doom made there stay there.

`tools/sxfs_sync.py` and `libsxfs` install files from the host, with the
default of the destination path. `sxfs_sync.py` also compacts an image by
rebuilding it from extracted host files, which carry no owner or mode: `sxfs-cli
extract` writes an `owner <path> <uid> <gid> <octal mode>` line per path to a
side file, and the rebuild applies them (`carried_ownership`), except for what
the build itself reinstalls or retires. Without that, a compaction would
silently reset every file a user or an elevated installer created to its
default. `apply` accepts `owner` lines in any manifest.

### The in-memory tree

The initramfs is CPIO `newc`, which carries mode, uid and gid per entry.
`mkinitramfs.py` already writes the mode (`0755` under `bin/`, `0644`
elsewhere). It writes uid 0, gid 0, and the VFS starts reading all three
instead of only the file type.

Writing to an initramfs file, or creating a file in the in-memory tree, becomes a
system-only operation. Nothing in the tree does it today except tests, which run
as system. `/rescue` is the system's in every case: it is what boots when the
disk does not, so nothing the user runs may change it.

**A rescue boot has no user.** With no disk there is no profile and no
account, so init and the rescue shell run as system, as Windows' recovery
environment runs as SYSTEM. That is not a hole: whoever can boot the machine
without its disk can also read the disk from another machine, and the boot
chain is outside this model ([`KERNEL_SECURITY.md`](KERNEL_SECURITY.md)).

### Devices

`device::register_node` gains an owner and a mode. Most nodes stay open to the
user (`/dev/gpu0` through `windowd`, `/dev/audio0` through `audiod`, the
clipboard, `/dev/power` for shutting down). The ones that must not be:

- **`/dev/keyboard`** (0666) has the layout ioctls of `/dev/input0` and none
  of its events: how the user's desktop and init set and read the layout.
- **`/dev/input0` and `/dev/mouse0`: system only, read by `windowd`.** If any
  process can read the keyboard, it can read a password; if any process can
  write input, it can click "Yes" on the elevation prompt. The prompt below is
  worth nothing without this. The mode keeps the user out; within the
  system, `windowd` claims both (`INPUT_IOC_CLAIM`,
  [`INPUT.md`](INPUT.md#who-reads-the-events)), and from then on nobody else
  can read them. No device node accepts events written into it.
- Raw block devices, if they ever get a node.

### The check

One function in the VFS, `vfs::permits(credentials, vnode, want)`, called before a
driver is reached. It belongs with the rules `FILESYSTEMS.md` already lists as
"enforced for every driver", so a FAT or ISO driver inherits it without
repeating it. The semantics are Unix's: owner bits, else group bits, else other
bits; effective uid 0 passes read and write, and passes execute only if some `x`
bit is set.

It runs on the vnode, after `normalize_path`. That matters for the `/disk`
alias ([`FILESYSTEMS.md`](FILESYSTEMS.md#the-root-is-the-system-disk)):
`/disk/bin/x` and `/bin/x` are one vnode, so the alias cannot be a way around
the check. Nothing may be decided from the path text alone.

## Elevation

Three parties, and the order matters:

1. The program asks the **broker** (a system service init starts) to run
   something elevated: a path and arguments, over a channel it can open.
2. The broker asks **`windowd`** to show the confirmation: the program's name,
   and Yes/No.
3. `windowd` draws it **on its own layer, above every client**, and takes the
   answer only from real input it reads from `/dev/input0`. No client can
   cover the dialog or send it input. One can draw a look-alike in its own
   window, but answering that elevates nothing: only the broker does.
4. If the user says yes, the broker spawns the program with an elevated token.
   The program that asked gets back the pid, never the token.

That is UAC's secure desktop, reduced to what a compositor that already owns
the screen and the input can do. Without step 3 the model is decorative: a
program could approve its own request.

### How it is built

The protocol is `include/elevation/elevation_protocol.h`; a POSIX program
calls `elevate()` in the libc.

- **The broker is `elevationd`** (`system/elevationd`), native, started by
  init before the session so its port name (`elevation`) is taken first. It
  serves one request at a time. Who asked comes from the connection
  (`SXN_SYS_PORT_ACCEPT_EX`), never from the request, and the elevated program
  runs with that person's uid and gid and euid 0. The program is the broker's
  child: the broker reaps it, and with `SAVANXP_ELEVATION_WAIT` reports its
  exit status to the requester. A requester that already acts as the system
  is not asked: it could start the program itself.
- **The prompt is windowd's**, on a second port, `windowd.consent`. The
  broker checks that the system created it (`SXN_SYS_PORT_CONNECT_EX`), so a
  program that took the name first cannot answer; windowd hears a consent
  request only from a connection that acts as the system, and `elevtest`
  checks that it refuses the user. The prompt is the top layer of the scene,
  above the Alt+Tab switcher and a fullscreen window, drawn on windowd's own
  surface (`windowd_consent.c`). While it is up, every key and pointer event
  is its own: no window, hotkey or WM key sees any.
- **What it shows:** the program's path, every argument after `argv[0]`, and
  who asked. What is drawn is what runs: a byte the font would not show as
  itself is written `\xNN`, and an argument that is empty or holds a space, a
  quote, a backslash or such a byte is quoted, with `\"` and `\\` inside, so
  where each argument ends is visible. The prompt grows to fit all of it;
  one that would not fit on the screen is not shown, and the request fails
  with `E2BIG`. Nothing is cut off.
- **What counts as an answer:** Y, N or Esc, Enter or Space on the focused
  button (it starts on No, so Enter alone is no), or a click whose press and
  release both land on one button. Nothing counts until 500 ms after it
  appears, so a key the user was typing into a window cannot answer it, and a
  held key is not a press: the keyboard core marks its repeats
  (`SAVANXP_KEY_MOD_REPEAT`), and the prompt ignores them.
  Nothing the requester writes reaches windowd, and the broker reads nothing
  it writes after its request; hanging up withdraws the request and closes
  the prompt. An unanswered prompt is no after ten minutes.

There is no password at first. The one user is the administrator, and the
confirmation is consent, not authentication, as it is for an NT administrator in
approval mode. A password belongs with a second user, not before.

### What requires elevation

| operation | why |
| --- | --- |
| writing system files: `/bin`, `/lib`, `/system` | installing and uninstalling, machine settings |
| killing a process that is not yours | services, `windowd` |
| setting the system clock | shared by every process |
| raw devices, PCI configuration | they bypass every check above |
| `fscheck` repair, formatting | the whole volume |

Shutdown stays unelevated, as it is on a desktop NT. Reading the system
(`/bin`, logs, the process list) stays unelevated.

What enforces each, since phase 4: the VFS for the files; `kill` for another
person's or the system's process (`EPERM`, POSIX's rule: the caller's real or
effective uid against the target's real one); `SET_REALTIME` for the clock
(`EPERM`). Raw devices have no node yet, and `fscheck` repair runs at boot as
the system.

Uninstalling is writing `/bin` (and, with the box ticked, deleting a
`data_dir`), so Add or Remove Programs asks for elevation: run by the user, it
starts `appwiz --uninstall <path> [--data]` through the broker, which finds
the entry again in a scan of its own rather than trusting the arguments. The
registry of installed programs that should replace the `programs.txt`
subtraction, as Windows keeps under `Uninstall`, is written by the installer
while it is elevated, into `/system`, and belongs with that phase too.

## POSIX, as a consequence

| call | becomes |
| --- | --- |
| `getuid`, `geteuid`, `getgid`, `getegid` | the token |
| `stat` | `st_uid`, `st_gid`, full `st_mode`, `st_mtime`, through `STAT_EX`/`FSTAT_EX` (`struct savanxp_stat_ex`). `STAT` keeps the 8-byte `savanxp_stat`, now with the real permission bits: a binary built before passes a buffer that size |
| `chmod`, `fchmod`, `chown` | implemented; `chown` needs effective uid 0 |
| `access` | answers with the real uid, as POSIX says |
| `umask` | applied on create |
| `getpwuid`, `getpwnam` | the two identities, from a fixed table: `root` (0) and `user` (1000, `/users/user`) |
| `setuid`, `seteuid` | lowering only (an elevated process may drop to the user); raising is `EPERM` |

`struct stat` already had room for the new fields, so its layout did not
change; the kernel's `savanxp_stat` did, which is why it got new calls instead
of growing. A binary that creates a file passes its mode through
`SAVANXP_OPEN_MODE` (the third argument of OPEN) and `MKDIR_MODE`; one built
before them leaves that register undefined, so without the flag a file is 0644
and a directory 0755.

### What the check covers

| operation | needs |
| --- | --- |
| reaching any path | search (`x`) on every directory above it |
| `open` | `r` and/or `w` on the file, by the flags |
| create, `mkdir` | `w` and `x` on the parent; in the in-memory tree, the system |
| `unlink`, `rmdir`, `rename` | `w` and `x` on the parent (both parents for a rename); in a sticky directory (`/tmp`), also being the entry's owner, the directory's or the system |
| `truncate` | `w` on the file |
| spawn, `exec` | `x` on the file, for the caller (the child inherits its token) |
| `chdir` | `x` on the directory |
| `stat` | only reaching it |
| `chmod`, `fchmod` | being the owner, or the system |
| `chown` | being the system |

A refusal is `EACCES`, and `EPERM` for `chmod`, `chown` and raising an
identity. `set_identity` in `posix_translation.cpp` is the only place a token
changes, and it only lowers.

### Modification times

Kept in the inode in seconds, from `rtc::epoch_seconds()`: the CMOS is read
once and the monotonic clock carries it, because a read waits out the chip's
update cycle. Creating, growing, truncating and writing set it. An overwrite
in place commits it only when the second changed, so a burst of writes costs
one commit a second, not one a write; the core smoke's `bytes_per_commit` did
not move (4237 against 4328). `sxfs-cli apply` takes the host file's time, and
`extract` puts it back on the host file, so a compaction keeps it.

## Phases

1. **The format — done.** Ownership fields and the feature bit in SxFS, the
   default table, stamping on first mount and on the host, `fscheck` support,
   `libsxfs` writing the defaults, and the compaction carrying them. Verified:
   an existing `build/disk.img` stamped by the build, a copy of the same
   pre-ownership image stamped by the kernel on boot, the host regression
   (`sxfs_compaction_test.py`: stamping, the default table, a non-default owner
   surviving compaction, a mode refused for the wrong type), and
   `verify_doom_persistence.sh`.
2. **Tokens and the check — done.** The token in the process, inheritance,
   the check in the VFS (`vfs::permits`, `may_access`), device owners, the
   POSIX calls above, and the modification time in the inode. Everything still
   runs as the system. Verified by `idtest` in the core `smoke`: a fork lowered
   to uid 1000 is refused `/bin`, `/system/programs.txt`, `/dev/input0`,
   `chmod` and `chown` of what is not its own, and raising itself back, and is
   allowed its profile and its own files in `/tmp`, but not the system's.
3. **The broker and the prompt — done.** The service, the `windowd` layer,
   input ownership. Verified by `elevation-smoke`: as the system, a second
   reader and a second claim of the keyboard are refused, and a request runs
   without a prompt; lowered to the user, the keyboard and windowd's consent
   port refuse it, a request followed by a forged yes is declined by a real N
   (QMP, through PS/2), and a second one accepted by a real Y runs elevated
   (uid 1000, euid 0, writes `/system`) and reports its exit status. The input
   rules are also in `windowd --selftest`.
4. **The desktop runs as the user — done, but for the install record.**
   `sessiond` starts the desktop as uid 1000 (`session-smoke` checks it, and
   that windowd and compositord are the system's, through
   `SAVANXP_PROC_FLAG_SYSTEM`); the settings, the save defaults and Doom's
   saves moved into the profile; `/dev/keyboard`; `kill` and the clock
   checked (`idtest`); Add or Remove Programs elevates. The full smoke set passes, and
   the visual scenarios run in a session whose shell is the user's. *Left:*
   there is no installer inside the system yet (programs are installed from
   the host, `tools/build-user.sh`), so nothing records what it installed, and
   Add or Remove Programs still subtracts `programs.txt`. The record belongs
   with the first installer, written while elevated into `/system`.

## Deliberately out of scope

- **ACLs.** The inode keeps room (the reserved tail is not used up by the fields
  above), but nothing is built until a second user or a shared folder needs one.
- **More than one user**, login, switching users, passwords.
- **Capabilities or privileges** split out of "elevated": one bit until a case
  needs two.
- **Network identity.** Sockets need no permission yet. Binding a low port
  could be made system-only later, if a server ever runs on the system.

## Decided

- **The profile path is `/users/<name>`**, NT's `Users`, not Unix's `/home`.
- **The user is `user`** (uid 1000, profile `/users/user`), fixed until there
  is a second user. Neither the installer nor the first boot asks for a name;
  that belongs with multi-user, which is out of scope.
