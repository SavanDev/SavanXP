# Linux profile: Xeyes on musl

> **Status: inventory recorded (2026-11-14); nothing implemented.** This is
> L0 of [SSL](../roadmaps/open/LINUX_SUBSYSTEM_ROADMAP.md), the Savan
> Subsystem for Linux: the measured scope of its first profile. A system call
> or an X request that is not in this document is not written.

The goal is the unmodified Alpine Linux `xeyes` with its musl runtime and X11
libraries, on the SavanXP desktop. On the way, the L1 to L5 gates each have a
fixture, traced here on Linux with the same files. Everything below can be
regenerated with one script, and the outputs it produced are committed.

## How it was measured

```sh
subsystems/linux/profile/profile.sh all
subsystems/linux/profile/summarize.py build/linux-profile/out subsystems/linux/profile/expected
```

`profile.sh setup` unpacks the pinned Alpine minirootfs and installs the
pinned packages from `.apk` files it checks against
[`alpine.lock`](../../subsystems/linux/profile/alpine.lock) before `apk`
verifies their signatures. `fixtures` builds the fixtures, and `trace` runs
each one under `strace -f` inside that root. Everything runs unprivileged in a
user namespace (`bwrap`), with no network and a fixed host name. Nothing is
installed on the host and nothing fetched is committed; the root, the
packages and the raw outputs live in `build/linux-profile/`.

X clients run against Xvfb through `x11rec.py`, a recording proxy that
**answers every `QueryExtension` with "absent"**, which is what `x11d` will
answer. The clients are therefore traced on the code path SavanXP will run,
with no extension in use. `x11ctl.py` moves the pointer and sends
`WM_DELETE_WINDOW` over a connection of its own that never appears in the
trace. ImageMagick crops the screenshots.

`summarize.py` normalizes each trace: it starts at the program's own
`execve`, and replaces process ids, addresses, times, the bytes of
`getrandom` and the host-side socket path. A run of identical lines becomes
one line with its count. Two consecutive runs gave byte-identical outputs,
statuses, screenshots and normalized traces, except for the X clients'
traces. Those differ only in how many times their event loops turned
(`poll`, `recvmsg`, `QueryPointer`), which depends on timing.

| Reference host | Version |
| --- | --- |
| Linux kernel | 7.2.15 (Arch Linux), x86-64 |
| strace | 6.19 (Alpine `strace-6.19-r1`) |
| X server | Xvfb 21.1.26 (Alpine `xvfb-21.1.26-r0`), 640x480x24 |
| clang and lld | 23.1.4, the project's host toolchain |
| FreeBSD Linuxulator studied | `f429c4a6ee50c99fbff672a3eaa7e5cdaa198a16` (2026-11-14): `sys/amd64/linux/syscalls.master`, `linux_sysvec.c` |

## The distribution

**Alpine Linux 3.24.4**, from `alpine-minirootfs-3.24.4-x86_64.tar.gz`,
SHA-256 `cdaa38cb8de3537967ee5b6c9b6cd00cc962da2b018e6eea48696bb274923012`,
plus 85 packages from the `v3.24` `main` and `community` repositories, each
by name, version and `.apk` SHA-256 in `alpine.lock`. The minirootfs already
holds `musl` 1.2.6-r3. Most of the 85 serve the reference host only: Xvfb
with its Mesa and fonts, strace, the musl and Xlib headers. **The profile
uses three closures from them.**

Alpine's stable branch replaces a package when it bumps its release, and the
mirror then drops the old file. When a pinned `.apk` disappears, the lock is
regenerated and the profile retraced. Old hashes are never edited in place.

### Xeyes: 19 files

`xeyes` 1.3.1-r0 is a position-independent executable (`ET_DYN`) whose
interpreter is `/lib/ld-musl-x86_64.so.1`. Its closure, followed through
`DT_NEEDED`, is the 18 shared objects the roadmap counted from the package
index. The interpreter also answers for `libc.musl-x86_64.so.1`.

| Name it is loaded by | Real file | Size | SHA-256 | Package |
| --- | --- | ---: | --- | --- |
| `xeyes` | `usr/bin/xeyes` | 32,488 | `c2c0ba389108f921f760e9ff668e0374eff247c31aef8207ce411acc66d5830a` | `xeyes` |
| `libc.musl-x86_64.so.1` | `lib/ld-musl-x86_64.so.1` | 670,312 | `095b047c631447e4fa50401fa7ffce01f678587040d65ba18c574981dc630de5` | `musl` |
| `libXi.so.6` | `usr/lib/libXi.so.6.1.0` | 63,280 | `b2280de9ea01dc90de961c2974830acae28232b67cfd6feb41d36266f20aae5d` | `libxi` |
| `libXmu.so.6` | `usr/lib/libXmu.so.6.2.0` | 89,072 | `9a9e893c24f40db28f6dadd6682d5429d6275186948a7dd1bd0e3884dab34c28` | `libxmu` |
| `libXt.so.6` | `usr/lib/libXt.so.6.0.0` | 355,824 | `8fbe437a8282161ac7fe3d6fdcf18c08c8aaa2656f88337156714611dc409d6d` | `libxt` |
| `libXext.so.6` | `usr/lib/libXext.so.6.4.0` | 68,496 | `a25b874bdaeed2ebea69eedd5be32f91bc868d714a584eed357ced0399694f54` | `libxext` |
| `libX11.so.6` | `usr/lib/libX11.so.6.4.0` | 1,161,312 | `5a7c2df00148b914e9d66b7455680bd2a1b29a0eaef68b115a0133ce441d2a76` | `libx11` |
| `libXrender.so.1` | `usr/lib/libXrender.so.1.3.0` | 42,880 | `92f49cdff11f38b0c8d46ba388787da305e29dad40ea8c3d9089c25bf81376dd` | `libxrender` |
| `libX11-xcb.so.1` | `usr/lib/libX11-xcb.so.1.0.0` | 13,880 | `f7a9e0281693f41309192880f242249e29afab9329eac458eed3f79dfb8f00c2` | `libx11` |
| `libxcb-present.so.0` | `usr/lib/libxcb-present.so.0.0.0` | 14,056 | `83e090bf1e523f120694c7676e168ed3068b8ae0164c86d7f02d8b09df3342f4` | `libxcb` |
| `libxcb-damage.so.0` | `usr/lib/libxcb-damage.so.0.0.0` | 14,056 | `d920e5600492125b4051bd17e617a29b362697548d8fd437ad18f21788c89967` | `libxcb` |
| `libxcb-xfixes.so.0` | `usr/lib/libxcb-xfixes.so.0.0.0` | 34,536 | `2e98e1a986a53e0651da324cbe79dbfb75857a6c24728282931498c3f150eeb5` | `libxcb` |
| `libxcb.so.1` | `usr/lib/libxcb.so.1.1.0` | 153,344 | `fd7bc55e8680c183fe038b22d5cc7407c323e3b1e1bd6ee488b57137124b0292` | `libxcb` |
| `libSM.so.6` | `usr/lib/libSM.so.6.0.1` | 34,592 | `906834d613d149db5b5a118da66fcf42bb669ec0c4942ed754e5a45191f7bc1d` | `libsm` |
| `libICE.so.6` | `usr/lib/libICE.so.6.3.0` | 88,608 | `07d3be6e4b18fb20d86fe1b5b6494ec6fbbcedffd95e0aafd668242d65e1e8a2` | `libice` |
| `libXau.so.6` | `usr/lib/libXau.so.6.0.0` | 13,936 | `0fd15be141dc06c149f2f2b92408dc91f4429d1f872a3fb9a655490e8c994578` | `libxau` |
| `libXdmcp.so.6` | `usr/lib/libXdmcp.so.6.0.0` | 26,384 | `cb4e243ca95695d59f1de5abc8cb0092d5ac1066811302c0f874a9341778b7eb` | `libxdmcp` |
| `libuuid.so.1` | `usr/lib/libuuid.so.1.3.0` | 30,624 | `69f5feff165ce0adf2f9db2d9e745338bdcbf43e1c965a49b01e8f485328dea4` | `libuuid` |
| `libbsd.so.0` | `usr/lib/libbsd.so.0.12.2` | 64,272 | `a5b321623fe7274c695805b7f46bb2ed8ac4e970413a4930f09701a17c44ed21` | `libbsd` |
| `libmd.so.0` | `usr/lib/libmd.so.0.2.0` | 63,408 | `4fd22b05b1297facf99c57988915345ff0ba033fbb2c6f07e4e3bb41fdedfeea` | `libmd` |

Each library is reached through its `DT_NEEDED` name, which in the package
is a symbolic link to the real file. The file system has none, so the guest
tree installs each real file under the name the closure uses.

**Data files Xeyes reads:** `/usr/share/X11/locale/locale.alias`,
`locale.dir` and `en_US.UTF-8/XLC_LOCALE`. libX11 reaches the last one
because musl's default locale, `C.UTF-8`, aliases to it. Every other path it
asks for may be absent, and was: `/etc/ld-musl-x86_64.path`, `~/.Xauthority`,
`~/.Xdefaults` and `~/.Xdefaults-<host>`, 24 app-defaults locations for
`XEyes`, and `libXcursor`, which libX11 tries to `dlopen` and lives without.

### The other closures

| Profile | File | SHA-256 | Package |
| --- | --- | --- | --- |
| L3 | `bin/busybox.static` (1,038,696 bytes) | `1690ed6db71fa35621537580135deb8de4f55f30a50d9d591050256e334cd8b9` | `busybox-static` 1.37.0-r31 |
| L4 | `usr/bin/jq` | `08eee682e6f041c89a61b7b95bdfcdac182d4ced3c342f57884e603ace4430ed` | `jq` 1.8.2-r0 |
| L4 | `usr/lib/libjq.so.1.0.4` | `59d38cbad1e15be199449f7ebc1b1d74194d9bd50fe1ea3320257264577ee4ce` | `jq` 1.8.2-r0 |
| L4 | `usr/lib/libonig.so.5.5.0` | `5c2f25ea34db8ab4408ad220f8760fb037346c20f683bd16787963ac7308524f` | `oniguruma` |
| L4 | `lib/ld-musl-x86_64.so.1` | as above | `musl` |

**`jq` is the L4 program**: dynamically linked, no display, two shared
libraries besides libc, and deterministic output. `libonig` is the one its
`test()` filter exercises.

## The fixture toolchain

**No musl toolchain is needed.** The project's clang and lld link every C
fixture against the pinned `musl-dev` files alone: `crt1.o`, `crti.o`,
`libc.a` (or `libc.so` for the X client), `crtn.o` and the headers, with
`-nostdlib -nostdinc` so nothing comes from the host's C library or compiler
runtime. Leaving out `-nostdlib` silently pulls in the host GCC's
`crtbeginT.o`, `libgcc` and `libgcc_eh`, which are pinned nowhere. The
fixtures need no compiler runtime at all. Each is linked with `-s` and
`--build-id=none` and rebuilds to the same bytes. A different clang produces
different bytes, so each output below belongs to the hash it was taken from
([`fixtures.sha256`](../../subsystems/linux/profile/expected/fixtures.sha256)).

| Fixture | Gate | Source | SHA-256 |
| --- | --- | --- | --- |
| `hello` | L1 | `fixtures/hello/hello.S`, libc-free | `9130a5d55763a9968c8d0474c1c6f7876ffa396fd5dab1b243cc4c5a6f7d2c20` |
| `startup` | L1 | `fixtures/startup/`, libc-free | `0358a1a33984d5e7e2839188367808075cc9501a8a02599c1d6f51a8952a3400` |
| `args` | L2 | `fixtures/static/args.c`, static musl | `0a32f755b540fb1f60855edc7d746c5b2f0103b029a5d929b99e38485dfffbfa` |
| `tls` | L2 | `fixtures/static/tls.c` | `2560af35461ed027456a838768762943237cb4f0843398a975881c2f0dc91871` |
| `heap` | L2 | `fixtures/static/heap.c` | `406042fa539ef686864c6f28093229502ef581e15f6b1f2f4210180d6d978b01` |
| `clock` | L2 | `fixtures/static/clock.c` | `06ade467f92f35356bd539153936443ab7b8af59ac9a6be45da422bfe4f058cf` |
| `files` | L2 | `fixtures/static/files.c` | `73a9f19420036b49e8504b20af2ebf9b0b34ecc0fc22a36410edf07e35235249` |
| `xclient` | L5 | `fixtures/xclient/xclient.c`, dynamic, Alpine's Xlib | `36e0ef7c6116acd290ad837f17d166591ea356756fad045653b3b10d8dca500c` |

Sources are under `subsystems/linux/fixtures/`.

- **`startup`** prints argc, argv, envp, which general registers were not
  zero at the entry point, the stack alignment, and the thirteen auxiliary
  vector entries the roadmap promises. Where a value can be predicted from
  its own headers, it checks it (`AT_PHDR`, `AT_PHENT`, `AT_PHNUM`,
  `AT_ENTRY`). It then makes two million zero-byte `write` calls with
  sentinels in RBX, RBP, RDI, RSI, RDX, R8-R10, R12-R15, XMM0, XMM7 and XMM15,
  checking each register after each call, and RCX and R11 as `syscall` sets
  them. On Linux: **all registers zero at entry, RSP 16-byte aligned, every
  auxiliary entry present and right, zero failures, exit status 3**.
- **`files`** has three modes, `write`, `check` and `remove`, so the
  restart test of L2 reads in a new boot what an earlier one wrote.
- **`xclient`** opens a 200x150 window titled `xclient`, fills a rectangle
  and a circle on each Expose, then reports the pointer's root position
  whenever it changes. It polls with `XQueryPointer`, as Xeyes does without
  XInput 2. It exits on `WM_DELETE_WINDOW` after `XDestroyWindow` and
  `XCloseDisplay`.

## Reference outputs

Kept in [`subsystems/linux/profile/expected/`](../../subsystems/linux/profile/expected/):
for each run its `.stdout`, `.stderr`, `.status` and normalized `.strace`,
the X request logs (`.x11`), and the screenshots under `screens/`. The runs:

| Run | Command (in the guest root, cwd `/work`) | Status | What it shows |
| --- | --- | ---: | --- |
| `hello` | `/fixtures/hello` | 7 | `hello` |
| `startup` | `env -i A=1 B=two /fixtures/startup x 'y z'` | 3 | as above |
| `args` | `env -i A=1 B=two /fixtures/args x 'y z'` | 3 | argv, envp, `getenv` |
| `tls` | `/fixtures/tls` | 0 | `tls ok` |
| `heap` | `/fixtures/heap` | 0 | checksums from 1 byte to 6 MiB, `realloc` to 4 MiB |
| `clock` | `/fixtures/clock` | 0 | `clock ok` |
| `files-write`, `-check`, `-remove` | `/fixtures/files MODE /work` | 0 | one line per check |
| `bb-echo`, `bb-cat`, `bb-ls`, `bb-pwd`, `bb-wc`, `bb-mkdir` | `/bin/busybox.static APPLET ...` on a prepared tree | 0 | the applet's output |
| `bb-cat-missing` | `busybox.static cat /work/tree/missing` | 1 | `cat: can't open '/work/tree/missing': No such file or directory` |
| `jq` | `jq -c '{d: (.a \| map(. * 2)), m: (.s \| test("ab+c"))}' in.json` | 0 | `{"d":[2,4,6],"m":true}` |
| `jq-missing-lib` | `jq --version` in a tree without `libonig.so.5` | 127 | `Error loading shared library libonig.so.5: No such file or directory (needed by /usr/bin/jq)`, then 11 more lines |
| `jq-missing-interp` | `jq --version` in a tree without the interpreter | 127 | `execve` fails with `ENOENT` |
| `xclient` | `/fixtures/xclient`, `DISPLAY=:1` | 0 | `window mapped`, `expose 200x150`, one `pointer` line per position, `delete requested`, `closed` |
| `xeyes` | `/usr/bin/xeyes`, `DISPLAY=:1` | 0 | on stderr: `Xlib:  extension "XInputExtension" missing on display ":1".` |

The X runs move the pointer to `75 50`, `0 0`, `149 99` and `100 20`, which
are inside both windows, then `400 300`, `620 10` and `10 460`, which are
outside both. A 220x170 screenshot from the screen's corner is taken at each
position. No window manager runs, so each window sits at `0,0` without a
frame: Xeyes at its default 150x100, black, with two white eyes whose pupils
point at the pointer in all seven captures. Xeyes's stderr line is the
expected result of an absent XInput, not an error. It is part of the
reference.

## System calls

Counts are over all runs of a gate. For the two X clients, "start" ends with
the last call that opens or looks for something, and "loop" is the event
loop and the exit. Loop counts vary from run to run. `execve` appears
because a trace begins with the launch; it is not a call the profiles make,
except in the two `chroot` runs, where BusyBox's `chroot` executes `jq`.

| Call | L1 | L2 | L3 | L4 | L5 start | L5 loop | L6 start | L6 loop |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| `write` | 2000069 | 3 | 2 | 1 |  |  |  |  |
| `exit` | 1 |  |  |  |  |  |  |  |
| `exit_group` | 1 | 7 | 7 | 3 |  | 1 |  | 1 |
| `arch_prctl` |  | 7 | 7 | 2 | 1 |  | 1 |  |
| `set_tid_address` |  | 7 | 7 | 2 | 1 |  | 1 |  |
| `brk` |  | 3 | 8 | 7 | 2 |  | 3 |  |
| `mmap` |  | 39 | 10 | 342 | 27 | 2 | 89 |  |
| `munmap` |  | 37 | 5 | 324 |  | 3 | 2 |  |
| `mremap` |  | 2 |  |  |  |  |  |  |
| `open` |  | 5 | 5 | 18 | 19 |  | 70 |  |
| `read` |  | 8 | 2 | 5 | 6 |  | 91 |  |
| `writev` |  | 13 | 4 | 27 |  | 121 | 17 | 42 |
| `close` |  | 4 | 4 | 5 | 7 | 1 | 22 |  |
| `lseek` |  | 6 |  |  |  |  | 2 |  |
| `fstat` |  | 2 |  | 3 | 6 |  | 18 |  |
| `stat` |  | 3 | 5 | 3 |  |  |  |  |
| `ioctl` |  | 9 | 5 | 2 |  | 1 |  |  |
| `unlink` |  | 1 |  |  |  |  |  |  |
| `nanosleep` |  | 1 |  |  |  |  |  |  |
| `fcntl` |  |  | 2 | 3 | 6 | 3 | 21 |  |
| `getcwd` |  |  | 1 | 1 |  |  |  |  |
| `getdents64` |  |  | 4 |  |  |  |  |  |
| `getuid` |  |  | 7 |  |  |  |  |  |
| `lstat` |  |  | 4 |  |  |  |  |  |
| `mkdir` |  |  | 5 |  |  |  |  |  |
| `sendfile` |  |  | 2 |  |  |  |  |  |
| `umask` |  |  | 2 |  |  |  |  |  |
| `mprotect` |  |  |  | 7 | 8 |  | 20 |  |
| `readlink` |  |  |  | 6 |  |  |  |  |
| `getrandom` |  |  |  | 1 |  |  |  |  |
| `access` |  |  |  |  | 1 |  | 26 |  |
| `socket` |  |  |  |  | 2 |  | 2 |  |
| `connect` |  |  |  |  | 2 |  | 2 |  |
| `getsockopt` |  |  |  |  | 1 |  | 1 |  |
| `getpeername` |  |  |  |  | 1 |  | 1 |  |
| `uname` |  |  |  |  | 1 |  | 3 |  |
| `poll` |  |  |  |  |  | 320 | 32 | 255 |
| `recvmsg` |  |  |  |  |  | 528 | 43 | 765 |
| `recvfrom` |  |  |  |  |  | 3 | 3 |  |
| `shutdown` |  |  |  |  |  | 1 |  |  |

**One call is missing from every trace and is required: `clock_gettime`.**
On Linux, musl reads the clocks through the vDSO, so `clock` and `time()`
made no system call. SSL offers no vDSO (`AT_SYSINFO_EHDR` is absent), and
the same fixture will then issue `clock_gettime` for `CLOCK_REALTIME` and
`CLOCK_MONOTONIC`. It belongs to L2.

**What is absent matters as much.** No run issued `clone`, `futex`,
`rt_sigaction`, `rt_sigprocmask`, `kill`, `fork`, `wait4`, `pipe`,
`epoll_*`, `eventfd`, `shmget`, `memfd_create` or `clock_nanosleep`. Xeyes
and the X client are single-threaded and install no signal handler. That
confirms the roadmap's choice to move signals, threads and process creation
past L6.

### The variants that size each piece

| Gate | Call | Forms seen |
| --- | --- | --- |
| L2 | `mmap` | anonymous `PROT_READ\|PROT_WRITE`, `MAP_PRIVATE\|MAP_ANONYMOUS`, no address; and one `PROT_NONE` `MAP_FIXED` anonymous page at a hint |
| L2 | `mremap` | `MREMAP_MAYMOVE`, growing a block (musl's `realloc` above its mmap threshold: 260 KiB to 1 MiB to 4 MiB) |
| L2 | `ioctl` | `TIOCGWINSZ` and `TCGETS`, both answered `ENOTTY` (the output is not a terminal) |
| L2 | `open` | `O_RDONLY`, and `O_WRONLY\|O_CREAT\|O_TRUNC` with mode `0644`; `O_LARGEFILE` always set |
| L3 | `open` | `O_RDONLY\|O_CLOEXEC\|O_DIRECTORY` for `ls`; `getdents64` with a 2 KiB buffer |
| L3 | `sendfile` | `cat` copies with `sendfile(1, fd, NULL, 16 MiB)` until it returns 0 |
| L3 | `fcntl` | `F_SETFD` |
| L4, L5, L6 | `mmap` | per library: the whole file `PROT_READ` private, then `MAP_FIXED` private file mappings `PROT_READ`, `PROT_READ\|PROT_EXEC` and `PROT_READ\|PROT_WRITE` over it, and a fixed anonymous `PROT_READ\|PROT_WRITE` tail where `.bss` needs one |
| L4, L5, L6 | `mprotect` | `PROT_READ` only, on relocated data (RELRO). Never executable, never writable and executable |
| L4 | `readlink` | on `$HOME` and `$HOME/.jq`; `EINVAL` for an existing path, `ENOENT` for a missing one |
| L4 | `getrandom` | 4 bytes, flags 0 |
| L5, L6 | `socket`, `connect` | `AF_UNIX`, `SOCK_STREAM\|SOCK_CLOEXEC`: first the abstract name `@/tmp/.X11-unix/X1`, refused, then the path `/tmp/.X11-unix/X1` |
| L5, L6 | `getsockopt` | `SOL_SOCKET`, `SO_SNDBUF` |
| L5, L6 | `fcntl` | `F_SETFD`, `F_GETFL`, then `F_SETFL` with `O_NONBLOCK` |
| L5, L6 | `poll` | one descriptor, `POLLIN` or `POLLIN\|POLLOUT` |
| L5, L6 | `recvmsg` | flags 0; about two thirds return `EAGAIN` |
| L5, L6 | `access` | `R_OK` only: `~/.Xauthority` (both clients), `XLC_LOCALE` and the app-defaults paths (Xeyes) |
| L6 | `lseek` | `SEEK_CUR` backwards while libX11 reads `locale.dir` |

No mapping anywhere asks for `MAP_SHARED`, and none for writable and
executable memory at once. The largest process, `jq`, made 342 `mmap` and 324
`munmap` calls, most of them heap blocks. Xeyes made 89 `mmap` calls and
unmapped two. Fifty-eight of them map at a fixed address over part of an earlier
mapping, which section views cannot express at any budget.

### What each call meets in the kernel today

The Linux layer runs in the kernel, so it calls kernel mechanisms directly.
The second column names what already exists, from the POSIX half or the
native API. "Subsystem state" means data the Linux layer keeps per process
in its own private state.

| Call | Kernel today | Gate | Missing |
| --- | --- | --- | --- |
| `write`, `read`, `writev`, `close` | VFS and handle I/O, pipes | L1, L2 | the Linux descriptor table (subsystem state) |
| `exit`, `exit_group` | process exit; one thread, so both end the process | L1 | — |
| `arch_prctl(ARCH_SET_FS)` | per-thread `%fs` base (slice 24) | L2 | — |
| `set_tid_address` | the thread id | L2 | keeping the pointer; clearing it on exit matters only with threads |
| `brk`, anonymous `mmap`, `munmap`, `mremap` | none: section views cannot split or move | L2 | **S2a, region list.** `mremap` is new to the plan |
| `open`, `stat`, `fstat`, `lstat`, `unlink`, `mkdir` | VFS by path and by node, with credentials | L2, L3 | flag and `struct stat` translation; `lstat` is `stat`, as the file system has no links |
| `lseek` | file offsets in handles | L2 | — |
| `ioctl(TCGETS, TIOCGWINSZ)` | — | L2 | `ENOTTY` on files and pipes; a console answer when a fixture runs on one |
| `nanosleep` | timed waits | L2 | — |
| `clock_gettime` (not traced, see above) | `SXN_SYS_CLOCK`, the monotonic and real-time clocks | L2 | — |
| `getdents64` | directory cursors | L3 | the `linux_dirent64` layout |
| `getcwd` | none in the kernel: the working directory left it (isolation B) | L3 | the working directory and its resolution (subsystem state) |
| `umask` | — | L3 | the mask (subsystem state), applied on create |
| `getuid` | credentials | L3 | — |
| `sendfile` | read and write | L3 | a loop of both inside the layer |
| `fcntl(F_SETFD, F_GETFL, F_SETFL)` | `SXN_SYS_SET_NONBLOCKING` for `O_NONBLOCK` | L3, L5 | close-on-exec as a descriptor flag (subsystem state) |
| private file `mmap`, `MAP_FIXED` replacement, `mprotect` | the loader's segment mapping, W^X | L4 | **S2b**; the kernel loading `PT_INTERP` itself |
| `readlink` | — | L4 | `EINVAL` for any existing path, `ENOENT` otherwise; `/proc/self/exe` when a fixture asks |
| `getrandom` | **none: the kernel has no random source a caller can read** | L4 | a kernel entropy source, a neutral slice with its own consumer check |
| `access` | `vfs::may_access` | L5 | — |
| `uname` | `SXN_SYS_SYSTEM_INFO` | L5 | the `utsname` layout; `sysname` is `Linux` |
| `socket(AF_UNIX)`, `connect`, `getpeername`, `getsockopt`, `shutdown` | named ports, two pipes per connection | L5 | the subsystem's address table and socket descriptors; abstract names are refused |
| `poll` | `SXN_SYS_WAIT_MANY` on streams | L5 | — |
| `recvmsg`, `recvfrom` | pipe reads, non-blocking | L5 | no control messages: Xlib passes none |

## X requests

From the proxy logs, with every extension answered absent. "Start" ends with
the first `MapWindow`.

| Request | xclient start | xclient after map | xeyes start | xeyes after map |
| --- | ---: | ---: | ---: | ---: |
| `QueryExtension` | 2 |  | 9 |  |
| `InternAtom` | 2 |  | 6 | 1 |
| `GetProperty` | 1 |  | 1 |  |
| `CreateGC` | 2 |  | 5 |  |
| `FreeGC` |  | 2 | 2 |  |
| `CreatePixmap` |  |  | 2 |  |
| `PutImage` |  |  | 2 |  |
| `QueryColors` |  |  | 3 |  |
| `CreateWindow` | 1 |  | 2 |  |
| `ChangeWindowAttributes` | 1 |  |  |  |
| `ChangeProperty` | 2 |  | 10 | 1 |
| `MapSubwindows` |  |  | 1 |  |
| `MapWindow` | 1 |  | 1 |  |
| `ClearArea` |  |  | 1 |  |
| `PolyFillRectangle` |  | 1 |  | 16 |
| `PolyFillArc` |  | 1 |  | 21 |
| `QueryPointer` |  | 101 |  | 30 |
| `GetInputFocus` |  | 1 |  |  |
| `DestroyWindow` |  | 1 |  |  |

**Nineteen core requests and no extension.** The details `x11d` needs:

- **Extensions asked for and answered absent:** `BIG-REQUESTS` and
  `XKEYBOARD` (libX11, both clients); `SHAPE`, `XInputExtension` (twice),
  `RENDER`, `XFIXES`, `DAMAGE` and `Present` (Xeyes). Both clients carried
  on. Xeyes prints the one stderr line above for XInput.
- **Xeyes's windows:** a 150x100 top-level child of the root and a 150x100
  child of it, which is where it draws. `ClearArea` and `MapSubwindows`
  come before `MapWindow`.
- **Properties set:** `WM_NAME`, `WM_ICON_NAME`, `WM_CLASS`, `WM_COMMAND`,
  `WM_CLIENT_MACHINE`, `WM_HINTS`, `WM_NORMAL_HINTS`, `WM_LOCALE_NAME`,
  `WM_CLIENT_LEADER` and `WM_PROTOCOLS`. The first one sets the caption.
- **Properties read:** `RESOURCE_MANAGER` on the root, as `STRING`. An empty
  answer is enough.
- **Pixmaps:** two 48x32 depth-1 pixmaps, each filled by one `PutImage` in
  `XYPixmap` format before `WM_HINTS` is set; 48x32 is the size of Xeyes's
  icon. Nothing draws them into a window, so `x11d` must accept them and may
  keep them unused.
- **Colors:** `QueryColors` on the default colormap, three times.
- **Drawing:** after mapping, only `PolyFillRectangle` and `PolyFillArc`:
  five arcs for the first frame, then a rectangle and an arc per eye each
  time the pupils move.
- **Events the clients received:** `Expose`, `MapNotify`, `PropertyNotify`
  (11), `ClientMessage` for `WM_DELETE_WINDOW` (sent, not generated),
  `UnmapNotify` and `DestroyNotify` (the X client's own destruction). No
  error reached either client.
- **The pointer:** Xeyes calls `QueryPointer` on a timer and follows the
  pointer anywhere on the screen, including outside its window. This is what
  P1 must provide.
- **Connection:** byte order LSB, protocol 11, no authorization data.
  Closing: Xeyes ends with `exit_group` and never sends `DestroyWindow`, so
  `x11d` must clean up on the connection's end alone.

## What the inventory changes in the plan

- **S2a gains `mremap`** with `MREMAP_MAYMOVE`, which musl's `realloc` uses
  for large blocks.
- **L2 gains `clock_gettime`**, which the host trace hides behind the vDSO.
- **L4 needs a random source**: `jq` calls `getrandom`. The kernel has none a
  caller can read, so it is a neutral slice, not something to invent in the
  Linux layer.
- **L3 needs `sendfile`, `getdents64`, `umask` and the working directory**,
  all of which are small. BusyBox's `cat` never calls `read` on the file.
- **The guest tree needs three locale files** besides the executable and
  its 19 objects, installed under the names the closure uses.
- **L5 needs no request beyond what L6 uses, plus `GetInputFocus`,
  `ChangeWindowAttributes` and `DestroyWindow`.** Xeyes adds `CreatePixmap`,
  `PutImage`, `QueryColors`, `ClearArea` and `MapSubwindows` at L6.
