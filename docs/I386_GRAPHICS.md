# i386 graphics and the reference desktop

The normal GRUB/BIOS boot starts the v0.4.0 `init` and graphical session as
ELF32 programs. This is milestone 4 of [the parity roadmap](I386_PORT.md), not
completion of v0.4.0 parity. Device services and physical hardware validation
remain separate work.

## Reference ownership and rendering

The port builds the retained `windowd`, `compositord`, `shellui`, `taskbar`,
Program Manager, Start menu, popups and graphical applications. `libsxgfx.so.0.4`,
`libgfx2d.so.0.4` and `libsxgui.so.0.4` retain their reference source units,
fonts, chrome and client protocol. Applications depending on these libraries
exclude their runtime units from the executable; `check_shared_libs.py` rejects
private copies. SXE resources are stamped into ELF32 programs, with manifest,
icon and build-ID dependencies causing regeneration and relinking.

The reference `kernel/display.cpp`, `kernel/gpu_device.cpp` and
`kernel/fb_gpu.cpp` implement dispatch, session ownership, imported section
surfaces, damage batches, presentation statistics/timelines and the linear
framebuffer backend. The i386 boundary in `arch/x86/graphics.cpp` supplies user
copies, kernel mappings, console geometry and graphics-session ownership.
Opening the GPU does not acquire the session. Another process can query it,
but acquisition/presentation respects ownership. Exit or a fatal user fault
releases the owner's imported surfaces and section references.

On standard QEMU VGA, the retained VBE dispi path maps its VRAM aperture and
supports mode changes and double buffering. The native boot request is
1280x800x32. F11's 640x400 path, restoration and compositor restart use the
reference code. The framebuffer backend is synchronous and supplies a software
cursor through the compositor; asynchronous events and a hardware cursor are
not advertised for that backend. The optional virtio profile uses the reference
GPU, tablet and keyboard drivers, including GPU events and absolute pointer
records; see [I386_VIRTIO.md](I386_VIRTIO.md). The native adapter does not
configure PAT write combining.

## Address spaces and scheduling

| Range or limit | i386 contract |
| --- | --- |
| ELF image base | `0x08000000` |
| User stack | top `0x0f000000`, 1 MiB reserved, eight initial pages |
| Stack guard | `0x0eeff000` |
| Section allocation start | `0x10000000` |
| User mapping ceiling | `0x40000000` |
| Supervisor physical RAM aliases | `0x40000000..0x80000000` |
| Supervisor contiguous section aliases | `0x80000000..0xb0000000` |
| Process/descriptor/view capacities | 64 processes; 64 descriptors and 64 views per process |

The higher stack leaves room for the reference demo's two 1920x1080 buffers;
it changes no program's pixels or buffers. Syscall validation materializes
missing pages within the reserved stack, as the reference VMM does. Holes,
read-only permissions and the guard remain enforced.

Imported surfaces need contiguous kernel virtual addresses even when their
physical pages are scattered. Kernel page tables for those aliases are shared
by all process directories, including directories created after an import.
Unmapping invalidates the local TLB and releases the view without freeing the
section's borrowed pages. Kernel page-table pages remain a bounded cache.
Device identity mappings must avoid user RAM, RAM aliases and the kernel-view
window; unsupported overlapping firmware framebuffer addresses are rejected.
The existing one-CPU/1-GiB physical-memory limits remain.

Events, timers and semaphores reuse `kernel/object.cpp`. Native waits retain
objects while blocked, support wait-any/all and deadlines, and release their
references on completion or termination. Polling an event tests its signal
without consuming it. `monotonic_ns()` returns its full value in EDX:EAX, at PIT
resolution. Orphans are reparented to the recorded init PID so it can reap apps
after a window-manager failure.

## Input and boot policy

The PS/2 keyboard and mouse expose `/dev/input0` and `/dev/mouse0`. Reads drain
whole available records and return zero when empty; `poll` provides the wait.
This distinction matters because the reference WM drains input in a loop.
The queues follow the reference 512-record bounds and preserve wheel counts
when dropping an old mouse record. IntelliMouse negotiation enables four-byte
wheel packets where supported; standard three-byte mice remain usable.
Session acquisition/release resets raw queues as in the reference; graphics
ownership suppresses canonical TTY consumption and pauses the native console.
The prototype kernel cursor and full-screen RAM backing are retired: text
writes/scrolling use the native framebuffer directly, and clients draw cursors.
This releases 4,096,000 bytes of permanent kernel storage and preserves the
complete heap regression on the 32-MiB machine.

`/bin/init` retains its desktop restart/fallback policy. Milestone 5 supplies
RTL8139 sockets and AC'97 audio, so the reference `audiod` and volume controls
can serve desktop clients. Deliberate absence of a device still follows init's
fallback policy. Audio PCM, ownership and protocol checks are documented in
[I386_SERVICES.md](I386_SERVICES.md).

The console regression suite derives a disposable ISO from the supplied ISO,
replaying its boot records and changing only the GRUB command line to
`console-init`. That explicit diagnostic mode starts `init32`, which runs
`sysinfo`, supervises the console shell and reaps orphan diagnostics. Normal
boot continues to use graphical init. `sh32` and `desktop32` remain explicit
diagnostics; the latter now uses the reference GPU ABI and draws its own cursor.

## Reproducible checks

```sh
./build.sh smoke windowd-smoke
./build.sh smoke sxgui-smoke
./build.sh smoke taskbar
./build.sh smoke calc-smoke
./build.sh smoke
./build.sh smoke memory
./build.sh smoke shared-memory
./build.sh test
python3 tools/verify_posix_persistence.py
./tools/verify_doom_persistence.sh
```

`windowd-smoke` runs the reference selftest, including ownership, twelve overlay
slots, descriptor budgets, compositor reconnection and fullscreen restoration.
`sxgui-smoke` reads fonts from the built library and checks Notepad's actual
`linea38`/`linea39` glyphs and spacing, a taskbar label, wheel direction/return
and scrollbar clicks. `taskbar` checks activation/minimization and window-state
publication; `calc-smoke` checks the visible calculator result. The default
smoke includes the console/ELF32 regressions followed by the SxGUI visual check.
Visual scenarios use 1 GiB of guest RAM; the 32-MiB scenarios are console/memory
regressions, not certification of the full desktop at that capacity.
Screenshots and serial logs are retained under `build/smoke-logs`.

A successful build and matching installed binaries are prerequisites for any
visual claim. A stale disk executable can survive a failed build. Persistence
checks compare executable, WAD, configuration and save bytes on an isolated
image; their success does not establish that Doom runs on the new graphics
stack. Milestone 5 adds independent runtime evidence for the external games,
media and device services in [I386_SERVICES.md](I386_SERVICES.md). Hardware input
variants, optional virtio/SMP and physical performance remain later work.
