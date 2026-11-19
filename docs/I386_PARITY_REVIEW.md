# v0.4.0 parity review on i386

This document is the feature-by-feature checklist that closes the v0.4.0
target defined in [I386_PORT.md](I386_PORT.md#reaching-a-parity-target). Each
row names a capability of the pinned reference (`v0.4.0`, commit `8ece07f4`),
the i386 evidence that exists today, and what the side-by-side comparison
still has to establish. The target is met only when every row is **Matched**
or **Justified**.

The comparison environment is the matched pair of QEMU boots in
[the study](I386_PORT_STUDY.md#25-matched-qemu-boots-for-the-v040-review):
the same QEMU, the `pc` and `virtio` profiles, snapshot overlays and QMP
captures for both builds.

## Status legend

| Status | Meaning |
| --- | --- |
| **Matched** | Compared against the reference in the matched environment; evidence recorded here. |
| **Ported** | Runs on i386 with its own regression; the side-by-side comparison is still pending. |
| **Partial** | Part of the reference behavior is present; the missing part is named. |
| **Gap** | Reference behavior that i386 does not provide yet. |
| **Justified** | An architectural difference accepted for the target, with its reason. |
| **Deferred** | Outside v0.4.0 by the [version policy](I386_PORT.md#version-parity-is-the-objective). |

When a row changes status, update it in the same commit as the change or the
evidence, and link the scenario, capture or measurement that supports it.

## Boot and session

| Capability | Status | i386 evidence | Comparison still required |
| --- | --- | --- | --- |
| Splash: logo, fade-in, marquee, status line | Ported | `smoke boot-screen` | Frame captures against the reference timing |
| Handoff from splash to compositor | Ported | `smoke boot-screen` | Same capture sequence on both builds |
| Desktop reached on `pc` and `virtio` | Matched | Study §25 | — |
| Edition string (System Properties, desktop corner) | Justified | `SAVANXP_EDITION` | Reference reads "Alpha"; the branch deliberately reads "Legacy Edition" |
| Loader: GRUB2/Multiboot2 + BIOS instead of Limine + UEFI | Justified | `./build.sh iso` | Architectural; keep loader time separate in measurements |

## Desktop and applications

| Capability | Status | i386 evidence | Comparison still required |
| --- | --- | --- | --- |
| `windowd` compositor, WM protocol v4, 12 windows | Ported | `windowd-smoke` | Damage region and frame statistics against the reference |
| Program Manager, auto-listed programs | Ported | `desktop`, `live-desktop`, `progman-smoke` | Layout capture |
| Taskbar: window list, activation, minimize | Ported | `taskbar` | Capture comparison |
| Taskbar ES/EN layout selector and popup | Partial | `kbd-smoke` covers the keyboard path | Popup interaction scenario |
| Taskbar volume button, per-app sliders | Partial | `audio-smoke`, `audiod-smoke`, `volume-smoke` | Popup and slider capture |
| Task List (Ctrl+Esc), Alt+Tab, Alt+F4 | Gap in evidence | — | Input scenario on both builds |
| Dialogs as windows, fixed-size windows | Gap in evidence | — | Interaction scenario |
| SxGUI: selection, clipboard, tabs, wheel/scrollbar | Partial | `sxgui-smoke` (glyphs, wheel) | Selection/Cut/Copy/Paste and tab captures |
| Files, with per-type icons and associations | Ported | `filesapp-smoke` | `shoot.sh files` capture |
| Notepad | Partial | `sxgui-smoke` editor | Wheel/scrollbar and editing capture |
| Shell window | Gap in evidence | — | Command with arguments, repaint timing |
| Calculator (`libmath.so.0.4`) | Ported | `calc-smoke` | Capture comparison |
| Minesweeper | Ported | `mines-smoke` | Capture |
| Gears | Gap in evidence | — | Capture and frame rate |
| Add/Remove Programs | Ported | `appwiz-smoke` with Doom restored | Capture |
| System Properties | Gap in evidence | — | Capture (edition difference is justified above) |
| Task Manager: live CPU/memory per process | Ported | `idle`, `taskmgr-smoke` | Capture |
| Media Player (FFmpeg), icon and file associations | Ported | `media-smoke`, `filesapp-smoke` | `mediaplayer-display`, `-nolib`, `-noport` scenarios |
| Gfx Demo, widgets demo, diagnostics | Gap in evidence | — | Captures |

## Ports and external applications

| Capability | Status | i386 evidence | Comparison still required |
| --- | --- | --- | --- |
| DoomGeneric with `GENMIDI` music | Ported | `doom`, `verify_doom_persistence.sh` | Music output and frame pacing |
| Celeste Classic with music | Ported | `ccleste`, `media-smoke` | Capture and audio comparison |
| FFmpeg as `libffmpeg.so.0.4` | Ported | `media-smoke` | Load time and decode throughput |
| Optional Haxe native subsystem | Ported | `native-smoke` | `native-guihost`/`native-sxgui` counterparts |
| External SDK apps via `build-user.sh` | Ported | `I386_SDK.md` | Static, as in v0.4.0; in-tree programs and ports keep the reference PIE profiles |

## Shell, POSIX and SDK

| Capability | Status | i386 evidence | Comparison still required |
| --- | --- | --- | --- |
| Reference `sh`, eleven BusyBox applets, pipes, redirection | Ported | `posix` | Transcript comparison |
| Descriptors, poll, canonical TTY, `exec` | Ported | `posix`, `wakeup` | — beyond transcripts |
| Signals: limited reference contract, `SIGCHLD` | Ported | [I386_POSIX.md](I386_POSIX.md) | Transcript comparison |
| libc/libm surface, `%f/%e/%g`, `strtod`, SSE | Ported | `smoke` (libctest), `float-smoke` | — |
| 128 arguments / 8 KiB for `spawn`/`exec` | Gap in evidence | — | Explicit limit check |
| Sockets: UDP, TCP with loss and reordering | Ported | `net-smoke`, `tcp-smoke` | Throughput comparison |
| Persistence of guest writes across rebuilds | Ported | `verify_posix_persistence.py` | — |

## Memory, executables and libraries

| Capability | Status | i386 evidence | Comparison still required |
| --- | --- | --- | --- |
| Sections, views, budgets (64 views, 256 sections) | Ported | `shared`, `shared-memory` | — |
| Randomized view placement | Ported | [I386_MEMORY.md](I386_MEMORY.md) | — |
| Growing heap, 1 MiB demand stack with guard | Ported | `shared`, `memory` | — |
| PIE, `PT_INTERP`, embedded loader, 32 libraries | Ported | `shared` | — |
| Loader diagnostics (`brokentest`, `missingtest`, …) | Ported | `legacy_boot_test.py` | — |
| SXE stamping and resources | Ported | `sxe-smoke`, `filesapp-smoke` | — |
| 64 process slots | Ported | `windowd-smoke`, `smp` | — |
| RAM allocation limited to the first GiB | Justified | [I386_MEMORY.md](I386_MEMORY.md) | 32-bit direct map; reference QEMU machine uses 1 GiB, so no visible loss |

## Filesystem and storage

| Capability | Status | i386 evidence | Comparison still required |
| --- | --- | --- | --- |
| SxFS v2 on ATA, MBR/GPT | Ported | `partition-smoke`, `sxfs-smoke` | — |
| `fscheck` and host `sxfs-cli check` | Ported | `storage-smoke` | — |
| `virtio-blk` | Ported | `virtio-smoke` | Throughput comparison |
| Persistent `/disk/bin` and `/disk/games` across rebuilds | Ported | `verify_doom_persistence.sh` | — |
| LiveCD without a persistent disk | Ported | `live-desktop`, `storage-smoke` | — |

## Devices

| Capability | Status | i386 evidence | Comparison still required |
| --- | --- | --- | --- |
| PS/2 keyboard and IntelliMouse wheel | Ported | `sxgui-smoke`, `kbd-smoke` | — |
| virtio keyboard and tablet | Ported | `virtio-smoke`, `pointer-smoke --virtio` | — |
| AC'97 playback, `audiod`, per-app volume | Ported | `audio-smoke`, `audiod-smoke`, `ac97-stream`, `ac97-stream-quiet`, `ac97-count` | Feed statistics against the reference |
| virtio-sound playback and capture | Ported | `virtio-smoke`, `audio-record`, `virtio-stream`, `virtio-count`, `virtio-record` | — |
| RTL8139 and virtio-net | Ported | `net-smoke`, `virtio-smoke` | — |
| Framebuffer GPU and virtio-gpu | Ported | `desktop`, `virtio-smoke`, `gpu-soak`, `cursor-repro` | — |
| Clipboard, PC speaker, device permissions | Ported | `device-smoke` | — |
| CMOS clock, ACPI PM timer, `time()` epoch | Ported | `clock-smoke` | Drift comparison |

## CPU, scheduling and power

| Capability | Status | i386 evidence | Comparison still required |
| --- | --- | --- | --- |
| SMP up to 32 CPUs, global kernel lock | Ported | `smp-boot`, `smp`, `kernel-preempt` | Scaling measurement |
| Per-CPU idle and CPU accounting | Ported | `idle` | Task Manager capture |
| 1000 Hz timer, monotonic deadlines | Ported | `clock-smoke` | — |
| MADT/IOAPIC routing, AML `_PRT` | Ported | `net-smoke`, `power-smoke` | — |
| xAPIC instead of x2APIC on `-cpu max` | Justified (pending) | Study §25 | Confirm the cause and the cost in interrupt delivery |
| ACPI shutdown, reboot, power button | Ported | `power-smoke` | Same sequence on the reference |
| Idle halt when nothing runs | Gap in evidence | — | Host CPU usage of an idle guest on both builds |

## Security

| Capability | Status | i386 evidence | Comparison still required |
| --- | --- | --- | --- |
| Random kernel stack canary at boot | Ported | `smoke hardening` (RDRAND line) | — |
| Random per-image user guard, inherited by fork | Ported | `guardtest32` in `smoke shared` and `smoke hardening` | — |
| SMEP on supported CPUs, APs match the BSP | Ported | `smoke hardening` reads CR4 on every CPU | The reference halts SMEP-less APs even without BSP SMEP; i386 follows the documented baseline ([study §26](I386_PORT_STUDY.md#26-stack-guards-and-smep)) |
| NX and full W^X | Justified | No PAE: 32-bit paging has no NX bit | W^X stays an API rule for views and ELF segments |
| Ring-3 port I/O denied | Ported | `multitest32 fault-io` | — |
| Malformed ELF, SxFS and initramfs rejection | Ported | `memory`, `storage-smoke` | — |

## Performance

None of these has been measured side by side yet. Use the matched boots and keep
loader time separate from kernel and desktop time.

| Measurement | Reference source |
| --- | --- |
| Boot: kernel entry to first composed frame | Serial timestamps |
| Compositor compose/sync/present per frame | `windowd` statistics, [GRAPHICS_PERF.md](GRAPHICS_PERF.md) |
| Application launch: Shell, Files, Media Player | Launch-to-first-frame |
| Doom and Celeste frame pacing | Port statistics |
| Disk throughput on ATA and `virtio-blk` | `fscheck` commit cost, large copies |
| TCP throughput on RTL8139 and virtio-net | `tcp-smoke` |
| SMP scaling, 1 to 4 vCPUs | `smptest` |

## Reference scenarios without an i386 counterpart

Most v0.4.0 scenarios now run on i386. Init automation specs boot with the
reference `/SMOKE` file and host scenarios alias the CTest targets; see
[study §27](I386_PORT_STUDY.md#27-reference-init-automation-specs-on-i386).
`mediaplayer-selftest` is covered by `media-smoke`, which runs the same
`mediaplayer --selftest` command. These remain:

- `mediaplayer-nolib` and `mediaplayer-noport` remove `libffmpeg.so.0.4` from a
  disposable disk copy before boot; the i386 harness has no removal step yet.
- `mediaplayer-display` needs the FFmpeg port's QMP display callback.
- `native-guihost` and `native-sxgui` build the optional Haxe clients first.

## Suggested order

1. Port the five remaining scenarios above and add interaction scenarios for
   the Task List, Alt+Tab, Alt+F4, dialogs and the taskbar popups.
2. Capture both builds with the same input scripts and record the visual
   comparison per application.
3. Run the performance table and the idle-halt check.
4. Resolve every Justified (pending) row, then review the table as a whole
   before moving the target to v0.4.1.
