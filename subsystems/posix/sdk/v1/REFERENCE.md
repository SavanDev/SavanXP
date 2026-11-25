# SDK 2.0 Reference

## Versioning

The SDK version is `SAVANXP_SDK_VERSION_MAJOR.SAVANXP_SDK_VERSION_MINOR`
(`include/abi/savanxp_abi.h`), today **2.0**. About and `sdk/statusdemo`
print it.

- **Minor version: additions.** A new header, function, flag, ioctl,
  device node or runtime group, or a documented limit that is raised. A
  program built against 2.n still builds and runs against 2.n+1.
- **Major version: anything that breaks a program.** A rename, a removal,
  a changed return convention, a lowered limit, or a call that starts to
  require something it did not.
- **The version steps once per release, not once per change.** Like the OS
  version, `master` carries the version the next release will ship, at most
  one step above the last released one. Whether it steps is decided by
  everything that changes until that release. Under that version:
  - The first change since the release that adds API raises the minor
    (2.0 → 2.1). Further additions before the release leave it at 2.1.
  - The first change that breaks a program raises the major (2.0 or 2.1 →
    3.0). Further changes of either kind before the release leave it at
    3.0.
  - A release with no SDK change keeps the number it had.
- A change that touches the SDK says so in its changelog entry, naming the
  version it lands in. The release checks the number against those entries
  ([RELEASING.md](../../../../docs/build/RELEASING.md#1-the-release-commit)).
- SUN's surface counts the same as POSIX's once the SDK carries it
  ([SDK_V2_ROADMAP.md](../../../../docs/roadmaps/done/SDK_V2_ROADMAP.md),
  phase 3).
- The SDK version is not the soname version: shared libraries are
  `lib*.so.0.4`, tracking the OS
  ([SHARED_LIBRARIES.md](../../../../docs/kernel/SHARED_LIBRARIES.md)).

What changed from 1.2 to 2.0 is listed in [`README.md`](README.md#breaking-changes-since-12).

## What the contract covers

- `include/savanxp/*.h` and the standard headers under `include/`, and the
  headers `public-headers.txt` adds from the repository's `include/`
- the runtime under `runtime/`, as grouped by `runtime/units.txt`
- `linker.ld`
- `runtime/libraries.txt`: the system libraries a PIE program may link
  against, and the runtime units each replaces
- the sysroot `build/sdk/2.0` that `tools/make-sdk.sh` assembles from them,
  its layout included ([`README.md`](README.md#the-sysroot))
- `tools/build-user.sh`, the copy in the sysroot's `share/sdk/`, and
  `tools/new-user-app.sh`
- `share/sdk/sxcc`, its options and profiles, the archive names it links
  with `-l`, and `share/sdk/savanxp-toolchain.cmake`
  ([`README.md`](README.md#other-build-systems-sxcc-and-cmake))

## Visible limits

- `SAVANXP_MAX_ARGC = 15`
- `SAVANXP_MAX_ARG_LENGTH = 63`
- `SAVANXP_PROC_NAME_CAPACITY = 32`
- standard descriptors: `SAVANXP_STDIN_FILENO`, `SAVANXP_STDOUT_FILENO`,
  `SAVANXP_STDERR_FILENO`
- `waitpid(SAVANXP_WAIT_ANY, &status)` is supported

## Conventions

- `savanxp/*` calls return `>= 0` on success and `-errno` on failure. Decode
  them with `result_is_error`, `result_error_code` and `result_error_string`.
- Standard-header calls return `-1` and set `errno`, as POSIX says.
- Process states: `process_state_string`.
- System calls use `int 0x80`.
- `tools/build-user.sh` produces static `ELF64` images for `x86_64`.

## Low-layer calls (`savanxp/libc.h`)

- I/O: `savanxp_read`, `savanxp_write`, `savanxp_open`, `savanxp_open_mode`,
  `savanxp_close`, `savanxp_readdir`, `savanxp_ioctl`
- sockets: `savanxp_socket`, `savanxp_bind`, `savanxp_sendto`,
  `savanxp_recvfrom`, `savanxp_connect`
- base: `savanxp_getpid`, `savanxp_stat`, `savanxp_fstat`, `savanxp_chdir`,
  `savanxp_getcwd`
- processes: `spawn`, `spawn_fd`, `spawn_fds`, `exec`, `waitpid`, `exit`,
  `fork`, `kill`
- descriptors: `pipe`, `dup`, `dup2`, `seek`, `fcntl`
- filesystem: `unlink`, `mkdir`, `rmdir`, `truncate`, `rename`
- utilities: `yield`, `sleep_ms`, `uptime_ms`, `clear_screen`, `proc_info`,
  `system_info`, `poll`, `select`, `raise`
- real time: `realtime`, `set_realtime` (sets the RTC in UTC, years
  2000..2099)
- randomness: `savanxp_getrandom`, `random_status` (the kernel CSPRNG's bytes
  and state, `docs/kernel/RANDOM.md`)
- the C library's platform hooks: `sx_clock_epoch_seconds`,
  `sx_clock_monotonic_ns`, `sx_clock_sleep_ms`, `sx_entropy_fill`,
  `sx_process_id`, `sx_identity_query` -- what `time`, `gettimeofday`,
  `clock_gettime`, `nanosleep`, `getentropy`, `getrandom`, `getpid` and
  `getuid` stand on, POSIX here and native in SUN's runtime
  (`docs/userland/NATIVE_HEAP.md`). `gettimeofday` and `CLOCK_REALTIME` are
  the date; `atexit` takes up to 32 handlers
- clock validity: `clock_status` (whether the date can be believed, where it
  came from and why not, `struct savanxp_clock_status`; `docs/kernel/TIME.md`)
- storage devices: `storage_snapshot` (the index-th disk, partition or memory
  volume and where it is mounted, `struct savanxp_storage_device`;
  `docs/userland/DISK_MANAGEMENT.md`)
- hardware inventory: `device_snapshot` (the computer, PCI functions,
  platform devices and USB devices and interfaces with their driver, state
  and recorded resources, `struct savanxp_device_info`; a resource its driver
  claimed carries `SAVANXP_DEVICE_RESOURCE_CLAIMED` and its length, and
  `SAVANXP_DEVICE_RESOURCE_SHARED` when other devices use it too;
  `docs/kernel/DEVICE_INVENTORY.md`)
- consistency: `fscheck` (a read-only report of the volume; see
  `docs/fs/SXFS_CHECK.md`)

## Visible errors

`SAVANXP_EPERM`, `SAVANXP_EIO`, `SAVANXP_ENOEXEC`, `SAVANXP_EACCES`,
`SAVANXP_EAGAIN`, `SAVANXP_EINVAL`, `SAVANXP_EBADF`, `SAVANXP_ENOENT`,
`SAVANXP_ENOMEM`, `SAVANXP_EBUSY`, `SAVANXP_EEXIST`, `SAVANXP_ENODEV`,
`SAVANXP_ENOTDIR`, `SAVANXP_EISDIR`, `SAVANXP_ENOSPC`, `SAVANXP_EPIPE`,
`SAVANXP_ENOTTY`, `SAVANXP_ENOSYS`, `SAVANXP_ENOTEMPTY`, `SAVANXP_ECHILD`,
`SAVANXP_EXDEV`, `SAVANXP_EROFS`, `SAVANXP_ECONNRESET`, `SAVANXP_ETIMEDOUT`,
`SAVANXP_ECONNREFUSED`, `SAVANXP_EINTR`, `SAVANXP_ECANCELED`,
`SAVANXP_ENAMETOOLONG`

## Devices

Device nodes:

- `/dev/gpu0`
- `/dev/input0`
- `/dev/mouse0`
- `/dev/pointer` (the pointer speed: `input_get_pointer_speed`,
  `input_set_pointer_speed`, 1..20, 10 = one to one)
- `/dev/net0`
- `/dev/pcspk`
- `/dev/audio0`

Ioctl groups:

- gpu: `GPU_IOC_GET_INFO`, `GPU_IOC_ACQUIRE`, `GPU_IOC_RELEASE`, `GPU_IOC_PRESENT`, `GPU_IOC_PRESENT_REGION`, `GPU_IOC_SET_MODE`, `GPU_IOC_IMPORT_SECTION`, `GPU_IOC_RELEASE_SURFACE`, `GPU_IOC_PRESENT_SURFACE_REGION`, `GPU_IOC_WAIT_IDLE`, `GPU_IOC_GET_STATS`, `GPU_IOC_GET_SCANOUTS`, `GPU_IOC_REFRESH_SCANOUTS`, `GPU_IOC_SET_CURSOR`, `GPU_IOC_MOVE_CURSOR`, `GPU_IOC_GET_PRESENT_TIMELINE`, `GPU_IOC_WAIT_PRESENT`
- network: `NET_IOC_GET_INFO`, `NET_IOC_UP`, `NET_IOC_PING`
- PC speaker: `PCSPK_IOC_BEEP`, `PCSPK_IOC_STOP`
- audio: `AUDIO_IOC_GET_INFO`
- input: `INPUT_IOC_GET_POINTER_SPEED`, `INPUT_IOC_SET_POINTER_SPEED`

Shared types:

- `struct savanxp_fb_info` (the surface description inside
  `struct savanxp_gfx_context`; there is no `/dev/fb0`)
- `enum savanxp_gpu_backend` (`NONE`, `VIRTIO`, `FRAMEBUFFER`)
- `struct savanxp_gpu_stats`
- `struct savanxp_gpu_scanout_state`
- `struct savanxp_gpu_cursor_image`
- `struct savanxp_gpu_cursor_position`
- `struct savanxp_gpu_present_timeline`
- `struct savanxp_gpu_present_wait`
- `struct savanxp_input_event`
- `struct savanxp_mouse_event`
- `struct savanxp_net_info`
- `struct savanxp_system_info`
- `struct savanxp_fscheck_report`
- `struct savanxp_realtime`
- `enum savanxp_net_status`
- `enum savanxp_timer_backend`
- `struct savanxp_net_ping_request`
- `struct savanxp_net_ping_result`
- `struct savanxp_pcspk_beep`
- `struct savanxp_audio_info`
- `struct savanxp_sockaddr_in`

## Sockets

- domain: `SAVANXP_AF_INET`
- UDP: `SAVANXP_SOCK_DGRAM` with `SAVANXP_IPPROTO_UDP`. `recvfrom(...,
  timeout_ms)` takes milliseconds; `0` waits forever.
- `savanxp_sockaddr_in.ipv4` and `.port` are in host byte order.
- A TCP client: `socket(SAVANXP_AF_INET, SAVANXP_SOCK_STREAM,
  SAVANXP_IPPROTO_TCP)`, `connect(fd, &addr, timeout_ms)`, then `write` to
  send and `read` to receive until EOF or timeout.
- `poll` and `select` cover sockets, pipes, TTY/input and regular files.
- `fcntl(fd, F_GETFL/F_SETFL)` exposes `O_NONBLOCK`.
- There is no TCP server: no `listen` and no `accept`.

Network diagnostics:

- `savanxp_net_info.last_status` uses `enum savanxp_net_status`.
- `savanxp_net_info` exposes `tx_frames`, `rx_frames`, `tx_errors`,
  `rx_errors`, `arp_requests`, `arp_timeouts`, `ping_requests` and
  `ping_timeouts`.

## Graphics runtime

Public helpers:

- window: `gfx_open`, `gfx_close`, `gfx_acquire`, `gfx_release`,
  `gfx_present`, `gfx_present_region`, `gfx_present_rects`,
  `gfx_should_close`, `gfx_set_title`
- input: `gfx_poll_event`, `gfx_pointer_open`, `gfx_poll_pointer`,
  `gfx_set_pointer_capture`, `gfx_poll_window_motion`, `mouse_open`,
  `mouse_poll_event`
- software primitives: `gfx_rgb`, `gfx_stride_pixels`, `gfx_buffer_pixels`,
  `gfx_buffer_bytes`, `gfx_clear`, `gfx_pixel`, `gfx_hline`, `gfx_vline`,
  `gfx_rect`, `gfx_frame`, `gfx_text_width`, `gfx_text_height`,
  `gfx_blit_text`
- `sx_scaled_presenter`: integer pixel-art scaling, centered layout and row
  damage

The contract:

- `gfx_*` is the supported API for graphical programs.
- `gfx_open` opens only the compositor client mode, using the descriptors
  `fork()` and `exec()` inherit as `fd 3..6`. Without them it fails; there is
  no direct full-screen fallback.
- `gfx_present` and `gfx_present_region` take 32-bpp pixels from the program
  and report the damaged regions to the compositor.
- `gfx_poll_event` (keyboard) and `gfx_poll_pointer` (pointer, with the
  descriptor from `gfx_pointer_open`) read the window manager's event channel,
  which carries both kinds. `mouse_open` and `mouse_poll_event` always read
  `/dev/mouse0`, which with `/dev/input0` is for low-level diagnostics.
- `gfx_set_pointer_capture` requests relative mouse input for the main window.
  The window manager hides the cursor and delivers deltas while that window
  is active. Read them with `gfx_poll_window_motion`, which returns 1 for an
  event, 0 when empty or a negative error. The keyboard and absolute pointer
  readers save captured motion for this call. A full queue combines additional
  deltas and wheel ticks into its last event and keeps the latest button state.
- `gfx_should_close` returns 1 once the window manager asked the window to
  close.
- Under QEMU the kernel may back `/dev/mouse0` with an absolute
  `virtio-tablet-pci`, translated to relative motion so the mouse event
  format does not change.
- `/dev/gpu0` is the low-level device for `compositord`, `gputest` and
  diagnostic tools, `GPU_IOC_PRESENT*` included.
- The back buffer belongs to the program; `gfx_buffer_pixels` and
  `gfx_buffer_bytes` help validate it.
- `sx_scaled_presenter` is the path for low-resolution BGRX frames: it scales
  by an integer factor, centers the image, caches the expanded row, and
  presents only the smallest changed source band. `retarget` forces a full
  frame after a resize.

## PCM audio

- `audio_open` opens `/dev/audio0` for playback. The device also reserves
  the independent capture direction when the kernel supports it.
- `AUDIO_IOC_GET_INFO` exposes the active sample rate, channel count, sample
  width, and period/buffer sizes.
- `write(fd, pcm, bytes)` accepts interleaved `S16LE stereo` and requires a
  whole number of `frame_bytes`.
- The optional `savanxp/audio.h` module adds `sx_audio_mixer`: a wall-clock
  frame sink with a growable stereo buffer and unsigned-8 mono voices, source
  rate/pitch conversion, and classic volume/separation panning. Link it with
  `./tools/build-user.sh --audio`; the kernel remains a raw PCM transport.

## POSIX layer

Standard headers in `include/`:

- `unistd.h`, `fcntl.h`, `poll.h`, `sys/select.h`, `signal.h`, `errno.h`
- `stdio.h`, `stdlib.h`, `string.h`, `strings.h`, `ctype.h`, `math.h`, `time.h`
- `dirent.h`, `sys/types.h`, `sys/stat.h`, `sys/wait.h`, `termios.h`
- `sys/socket.h`, `sys/mman.h`, `sys/random.h`, `netinet/in.h`, `arpa/inet.h`
- `pthread.h`, `sched.h`, `dlfcn.h`, `syslog.h`

The layer lives in `runtime/posix.c` (threads in `runtime/pthread.c`) and is
reached through the standard headers. Its practical limits:

- A fixed heap allocator inside the runtime; `free()` recycles blocks and
  `realloc()` frees the old block when it has to move it.
- `DIR->d_type` is filled from `stat()`, best effort.
- `setsockopt` and `getsockopt` cover only timeouts and basic client flags.
- `mmap` and `munmap` cover only anonymous mappings: no file-backed mappings,
  arbitrary offsets or real copy-on-write.
- `getrandom` returns at most 4096 bytes per call and `getentropy` at most
  256; both fail with `EAGAIN` while the kernel generator is unseeded, and
  never block.
- **Signals keep their default action.** The kernel delivers no signals.
  `signal`, `sigaction` and `sigprocmask` exist and record what they are
  given, but no handler is ever called. `sigsuspend` returns `-1` with
  `EINTR` at once.
- `termios`: `tcgetattr` reports a fixed canonical mode with echo, and
  `tcsetattr` is accepted and ignored. There is no job control.
- `chmod`, `fchmod`, `chown` and `umask` exist.
- `realpath` is lexical: SxFS has no symbolic links and the VFS resolves
  `.` and `..` by the text of the path, so the result is the canonical name.
  The path must exist.
- `freopen` needs a path: changing the mode of the open file (a null path)
  fails with `EINVAL`. A reopened stdio stream keeps its descriptor number.
- `strtold` parses with `strtod`'s double precision; `ldexpl` is exact in
  the extended format. Both, like `strtod`, need `--sse`.
- `math.h` works with a compiler without the GNU builtins (TCC): the
  classification macros read the value's bits instead.
- `dlfcn.h` is implemented by the loader, which only PIE programs carry
  (`build-user.sh --pie`). In a static program, using `dlopen` is a link
  error.
- `select` and `poll` aim for consistent use rather than exhaustive POSIX
  compatibility; not every descriptor type has full Unix semantics.
- `fcntl` takes `F_DUPFD`, `F_GETFL`/`F_SETFL` and the record locks
  `F_GETLK`, `F_SETLK` and `F_SETLKW` with `struct flock`. `F_GETFD` and
  `F_SETFD` are defined but refused with `EINVAL`.
- Record locks are advisory and belong to the process, as POSIX has them:
  closing any descriptor of the file releases all of the process's locks on
  it, a child of `fork` holds none, and exit releases them all. A conflict is
  `EAGAIN` under `F_SETLK`; `F_SETLKW` waits, and fails with `EDEADLK` when
  waiting would close a cycle of processes waiting on each other. Only files
  have locks: a pipe, a device or a socket is `EINVAL`. The table holds 128
  ranges for the whole system, then `ENOLCK`
  ([FILE_LOCKS.md](../../../../docs/kernel/FILE_LOCKS.md)). SUN has the
  same locks as `sxn_lock_file`, owned by the open file as `LockFileEx`'s.
- Threads (`pthread.h`): create, join, detach, exit, mutexes (normal,
  recursive, error-checking), condition variables, once and keys, plus
  `__thread` variables in the executable. No cancellation, read-write locks,
  barriers or process-shared objects, and a shared library cannot have TLS.
  Stacks are committed up front (256 KiB unless the attributes ask for
  more), and each thread holds one of the process's 64 handles while it
  exists. `errno` is per thread; the heap and stdio take a lock once a second
  thread exists. `fork` copies only the calling thread and `exec` ends the
  others. See `docs/roadmaps/done/THREADS_ROADMAP.md`.

## Not included

- full POSIX compatibility
- `listen` and `accept`
- file-backed `mmap`, and partial `munmap` by range
- signal delivery to handlers
- a resolver API (`getaddrinfo`, `gethostbyname`)
- job control and a configurable line discipline
- `link`, `symlink`, `readlink`, `utime*`
- mandatory locks, `flock` and `lockf` (record locks are `fcntl`'s, advisory)
- audio capture and `mmap`/shared buffers for audio
