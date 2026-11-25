# v0.4.0 parity review on i386

This document is the feature-by-feature checklist that closes the v0.4.0
target defined in [I386_PORT.md](I386_PORT.md#reaching-a-parity-target). Each
row names a capability of the pinned reference (`v0.4.0`, commit `8ece07f4`),
the i386 evidence, and any difference the side-by-side comparison left. The
target is met only when every row is **Matched** or **Justified**, as it now
is (see [Outcome](#outcome)).

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
| Splash: logo, fade-in, marquee, status line | Matched | `smoke boot-screen`; `kernel/boot_screen.cpp` is the v0.4.0 file plus the edition line; stages in the reference order ([study §36](I386_PORT_STUDY.md#36-the-reference-boot-stages)) | The edition line follows the Edition row |
| Splash stage "Enabling virtual memory" | Justified | [Study §36](I386_PORT_STUDY.md#36-the-reference-boot-stages) | i386 enables paging before the framebuffer can be mapped, so the stage ends before the splash exists |
| Handoff from splash to compositor | Matched | `smoke boot-screen`; the reference `boot ready` and `handoff` lines; first composed Program Manager timed on both builds ([study §37](I386_PORT_STUDY.md#37-catalog-ports-and-performance-against-v040)) | — |
| Desktop reached on `pc` and `virtio` | Matched | Study §25; `parity_capture.py --scenario desktop` differs only in the edition label ([study §32](I386_PORT_STUDY.md#32-the-reference-core-smoke-and-the-remaining-scenario-names)) | — |
| Edition string (System Properties, desktop corner) | Justified | `SAVANXP_EDITION` | Reference reads "Alpha"; the branch deliberately reads "Legacy Edition" |
| Loader: GRUB2/Multiboot2 + BIOS instead of Limine + UEFI | Justified | `./build.sh iso` | Architectural; keep loader time separate in measurements |

## Desktop and applications

Captures from `tools/parity_capture.py` on the matched `pc` profile; the
per-scenario results are in [study §35](I386_PORT_STUDY.md#35-side-by-side-captures-of-the-desktop-and-applications). "Identical" means pixel-identical with the
edition label masked.

| Capability | Status | i386 evidence | Comparison still required |
| --- | --- | --- | --- |
| `windowd` compositor, WM protocol v4, 12 windows | Matched | `windowd-smoke`; every capture below composes identically | Frame statistics are in the performance table |
| Program Manager, auto-listed programs | Matched | `desktop`, `diagnostics`, `system` captures identical | — |
| Taskbar: window list, activation, minimize | Matched | `taskbar` captures identical | — |
| Taskbar ES/EN layout selector and popup | Matched | `kbdlayout` identical, [study §30](I386_PORT_STUDY.md#30-desktop-shortcuts-and-power-in-qemu) | — |
| Taskbar volume button, per-app sliders | Matched | `volume` captures identical (popup, 50 %, mute, close); `audiod-smoke`, `volume-smoke` | — |
| Task List (Ctrl+Esc), Alt+Tab, Alt+F4 | Matched | `window-keys`, `alttab` identical | — |
| Dialogs as windows, fixed-size windows | Matched | `dialogs` identical (Open, unsaved-changes, Save As) | — |
| SxGUI: selection, clipboard, tabs, wheel/scrollbar | Matched | `clipboard`, `cut`, `wheel`, `notepadwheel` and the `system` tabs identical | — |
| Files, with per-type icons and associations | Matched | `files` identical except the status-bar byte total | The total counts the English `/README` (661 bytes against 798), see the README row |
| Notepad | Matched | `notepadwheel`, `dialogs`, `clipboard`, `cut` identical | — |
| Shell window | Matched | `shell` identical, including `echo hola mundo`; only the caret blink phase differs | — |
| Calculator (`libmath.so.0.4`) | Matched | `calc` identical after the PS/2 keypad fix ([study §33](I386_PORT_STUDY.md#33-the-reference-ps2-driver-behind-the-i386-input-queues)) | — |
| Minesweeper | Matched | `mines` window, menus, dialogs and Alt+F4 identical | Boards differ by design: `mines_random_seed_from_clock` |
| Gears | Matched | `gears`, `diagnostics` | Rotation phase follows elapsed time; frame rate is in the performance table |
| Add/Remove Programs | Matched | `appwiz` identical | — |
| System Properties | Matched | `system` General and Hardware tabs identical | Install block differs only in justified rows: edition, loader name, RAM the loader leaves usable, disk contents |
| Task Manager: live CPU/memory per process | Matched | `system` Task Manager tabs identical apart from the System Properties window behind it; `idle`, `taskmgr-smoke` | — |
| Media Player decode selftest, GPU frame and missing-library feedback | Matched | [Study §28](I386_PORT_STUDY.md#28-port-specific-smoke-scenarios-and-the-reference-failure-paths) | — |
| Media Player icon, category and file associations | Matched | `filesapp-smoke` passes on both builds with the ports disk; the `mediaplayer` launch capture is identical ([study §37](I386_PORT_STUDY.md#37-catalog-ports-and-performance-against-v040)) | — |
| Gfx Demo, widgets demo, diagnostics | Matched | `diagnostics` identical (Gears, Gfx Demo, Key Test, Mouse Test, Widgets) | — |
| Root `/README` text | Justified | `rootfs/README` | English, as the repository language rule requires, and describes the GRUB/BIOS path |

## Ports and external applications

| Capability | Status | i386 evidence | Comparison still required |
| --- | --- | --- | --- |
| DoomGeneric with `GENMIDI` music | Matched | `doom` capture and audio on both builds, `verify_doom_persistence.sh` ([study §37](I386_PORT_STUDY.md#37-catalog-ports-and-performance-against-v040)) | Title frames differ by demo-loop phase |
| Celeste Classic with music | Matched | `ccleste` captures and audio, `ccleste-selftest` on both builds ([study §37](I386_PORT_STUDY.md#37-catalog-ports-and-performance-against-v040)) | Title and dash frames differ by animation phase |
| FFmpeg as `libffmpeg.so.0.4` | Matched | `ffmpegload`, `mediaplayer-selftest` on both builds ([study §37](I386_PORT_STUDY.md#37-catalog-ports-and-performance-against-v040)) | — |
| Optional Haxe native subsystem | Justified | `native-smoke`, `native-guihost`, `native-sxgui`, `native-hello` | The pinned reference's native build fails at the CRT link step, so it has no runnable counterpart; i386 runs the v0.4.0 scenarios |
| External SDK apps via `build-user.sh` | Matched | `I386_SDK.md`; `sdk/paritybench` built unchanged with both SDKs ([study §37](I386_PORT_STUDY.md#37-catalog-ports-and-performance-against-v040)) | Static, as in v0.4.0; in-tree programs and ports keep the reference PIE profiles |

## Shell, POSIX and SDK

| Capability | Status | i386 evidence | Comparison still required |
| --- | --- | --- | --- |
| Reference `sh`, eleven BusyBox applets, pipes, redirection | Matched | `posix`, `smoke`, the v0.4.0 catalog run on both builds ([study §37](I386_PORT_STUDY.md#37-catalog-ports-and-performance-against-v040)) | — |
| Descriptors, poll, canonical TTY, `exec` | Matched | `posix`, `wakeup`, `smoke` (`polltest`), the v0.4.0 catalog run on both builds ([study §37](I386_PORT_STUDY.md#37-catalog-ports-and-performance-against-v040)) | — |
| Signals: limited reference contract, `SIGCHLD` | Matched | [I386_POSIX.md](I386_POSIX.md), `smoke` (`forktest`), the v0.4.0 catalog run on both builds ([study §37](I386_PORT_STUDY.md#37-catalog-ports-and-performance-against-v040)) | — |
| libc/libm surface, `%f/%e/%g`, `strtod`, SSE | Matched | `smoke` (reference core spec with `libctest`, [study §32](I386_PORT_STUDY.md#32-the-reference-core-smoke-and-the-remaining-scenario-names)), `float-smoke`; the same core spec passes on the reference | — |
| 128 arguments and 8191 bytes of strings for `spawn`/`exec` | Matched | `processes`, [study §29](I386_PORT_STUDY.md#29-argument-budgets-and-adoption-of-orphaned-children) | — |
| Reaping children orphaned before or after their exit | Matched | [study §29](I386_PORT_STUDY.md#29-argument-budgets-and-adoption-of-orphaned-children) | — |
| Sockets: UDP, TCP with loss and reordering | Matched | `net-smoke`, `tcp-smoke`; throughput in the performance table | — |
| Persistence of guest writes across rebuilds | Matched | `verify_posix_persistence.py`, `verify_doom_persistence.sh` | — |

## Memory, executables and libraries

| Capability | Status | i386 evidence | Comparison still required |
| --- | --- | --- | --- |
| Sections, views, budgets (64 views, 256 sections) | Matched | `shared`, `shared-memory`, `smoke` (`sectiontest`), the v0.4.0 catalog run on both builds ([study §37](I386_PORT_STUDY.md#37-catalog-ports-and-performance-against-v040)) | — |
| Randomized view placement | Matched | [I386_MEMORY.md](I386_MEMORY.md), `shared-memory`, the v0.4.0 catalog run on both builds ([study §37](I386_PORT_STUDY.md#37-catalog-ports-and-performance-against-v040)) | — |
| Growing heap, 1 MiB demand stack with guard | Matched | `shared`, `memory`, `smoke` (`stacktest`), the v0.4.0 catalog run on both builds ([study §37](I386_PORT_STUDY.md#37-catalog-ports-and-performance-against-v040)) | — |
| PIE, `PT_INTERP`, embedded loader, 32 libraries | Matched | `shared`, `smoke` (`imagetest`), `diamondtest`, the v0.4.0 catalog run on both builds ([study §37](I386_PORT_STUDY.md#37-catalog-ports-and-performance-against-v040)) | — |
| Loader diagnostics (`brokentest`, `missingtest`, …) | Matched | `brokentest`, `missingtest`, `needstest-missing`, `slottest`, the v0.4.0 catalog run on both builds ([study §37](I386_PORT_STUDY.md#37-catalog-ports-and-performance-against-v040)) | — |
| SXE stamping and resources | Matched | `sxe-smoke`, `filesapp-smoke`, the v0.4.0 catalog run on both builds ([study §37](I386_PORT_STUDY.md#37-catalog-ports-and-performance-against-v040)) | — |
| 64 process slots | Matched | `windowd-smoke`, `smp`, `slottest`, the v0.4.0 catalog run on both builds ([study §37](I386_PORT_STUDY.md#37-catalog-ports-and-performance-against-v040)) | — |
| RAM allocation limited to the first GiB | Justified | [I386_MEMORY.md](I386_MEMORY.md) | 32-bit direct map; reference QEMU machine uses 1 GiB, so no visible loss |

## Filesystem and storage

| Capability | Status | i386 evidence | Comparison still required |
| --- | --- | --- | --- |
| SxFS v2 on ATA, MBR/GPT | Matched | `partition-smoke`, `sxfs-smoke`, the v0.4.0 catalog run on both builds ([study §37](I386_PORT_STUDY.md#37-catalog-ports-and-performance-against-v040)) | — |
| `fscheck` and host `sxfs-cli check` | Matched | `storage-smoke`, the v0.4.0 catalog run on both builds ([study §37](I386_PORT_STUDY.md#37-catalog-ports-and-performance-against-v040)) | — |
| `virtio-blk` | Matched | `virtio-smoke`, the v0.4.0 catalog run on both builds ([study §37](I386_PORT_STUDY.md#37-catalog-ports-and-performance-against-v040)) | — |
| Persistent `/disk/bin` and `/disk/games` across rebuilds | Matched | `verify_doom_persistence.sh` | — |
| LiveCD without a persistent disk | Matched | `live-desktop`, `storage-smoke` | — |

## Devices

| Capability | Status | i386 evidence | Comparison still required |
| --- | --- | --- | --- |
| PS/2 keyboard and IntelliMouse wheel | Matched | `sxgui-smoke`, `kbd-smoke`, the reference driver ([study §33](I386_PORT_STUDY.md#33-the-reference-ps2-driver-behind-the-i386-input-queues)), identical `wheel` captures | — |
| virtio keyboard and tablet | Matched | `virtio-smoke`, `pointer-smoke --virtio`, the v0.4.0 catalog run on both builds ([study §37](I386_PORT_STUDY.md#37-catalog-ports-and-performance-against-v040)) | — |
| AC'97 playback, `audiod`, per-app volume | Matched | `audio-smoke`, `audiod-smoke`, `ac97-stream`, `ac97-count`; `doom` and `ccleste` audio match ([study §37](I386_PORT_STUDY.md#37-catalog-ports-and-performance-against-v040)) | — |
| virtio-sound playback and capture | Matched | `virtio-smoke`, `audio-record`, `virtio-stream`, `virtio-record`, the v0.4.0 catalog run on both builds ([study §37](I386_PORT_STUDY.md#37-catalog-ports-and-performance-against-v040)) | — |
| RTL8139 and virtio-net | Matched | `net-smoke`, `virtio-smoke`, `paritybench` TCP figures ([study §37](I386_PORT_STUDY.md#37-catalog-ports-and-performance-against-v040)) | — |
| Framebuffer GPU and virtio-gpu | Matched | `desktop`, `virtio-smoke`, `gpu-soak`, `cursor-repro`, the v0.4.0 catalog run on both builds ([study §37](I386_PORT_STUDY.md#37-catalog-ports-and-performance-against-v040)) | — |
| Clipboard, PC speaker, device permissions | Matched | `device-smoke`, `clipboard` capture identical | — |
| CMOS clock, ACPI PM timer, `time()` epoch | Matched | `clock-smoke`, the v0.4.0 catalog run on both builds ([study §37](I386_PORT_STUDY.md#37-catalog-ports-and-performance-against-v040)) | — |

## CPU, scheduling and power

| Capability | Status | i386 evidence | Comparison still required |
| --- | --- | --- | --- |
| SMP up to 32 CPUs, global kernel lock | Matched | `smp-boot`, `smp`, `kernel-preempt`; scaling in the performance table | — |
| Per-CPU idle and CPU accounting | Matched | `idle`, `taskmgr-smoke`; Task Manager captures identical | — |
| 1000 Hz timer, monotonic deadlines | Matched | `clock-smoke`, the v0.4.0 catalog run on both builds ([study §37](I386_PORT_STUDY.md#37-catalog-ports-and-performance-against-v040)) | — |
| MADT/IOAPIC routing, AML `_PRT` | Matched | `net-smoke`, `power-smoke`; same IOAPIC overrides and `_PRT` links in both boot logs | — |
| x2APIC when CPUID advertises it, xAPIC otherwise | Matched | `hardening --smp 4` (x2APIC), `smp` (xAPIC), [study §34](I386_PORT_STUDY.md#34-x2apic-when-the-cpu-advertises-it) | — |
| ACPI shutdown, reboot, power button | Matched | `power-smoke`, [study §30](I386_PORT_STUDY.md#30-desktop-shortcuts-and-power-in-qemu) | — |
| Idle halt when nothing runs | Matched | QMP `HLT=1` on both builds, [study §31](I386_PORT_STUDY.md#31-timing-with-the-apic-mode-controlled) | — |

## Security

| Capability | Status | i386 evidence | Comparison still required |
| --- | --- | --- | --- |
| Random kernel stack canary at boot | Matched | `smoke hardening` (RDRAND line), the v0.4.0 catalog run on both builds ([study §37](I386_PORT_STUDY.md#37-catalog-ports-and-performance-against-v040)) | — |
| Random per-image user guard, inherited by fork | Matched | `guardtest32` in `smoke shared` and `smoke hardening` | — |
| SMEP on supported CPUs, APs match the BSP | Justified | `smoke hardening` reads CR4 on every CPU | The reference halts SMEP-less APs even without BSP SMEP, which stops its own four-vCPU `qemu64` boot; i386 follows the documented baseline ([study §26](I386_PORT_STUDY.md#26-stack-guards-and-smep), [§31](I386_PORT_STUDY.md#31-timing-with-the-apic-mode-controlled)) |
| NX and full W^X | Justified | No PAE: 32-bit paging has no NX bit | W^X stays an API rule for views and ELF segments |
| Ring-3 port I/O denied | Matched | `multitest32 fault-io` | — |
| Malformed ELF, SxFS and initramfs rejection | Matched | `memory`, `storage-smoke`, `brokentest` | — |

## Performance

Measured on the matched boots under QEMU TCG with one `qemu64` vCPU unless
noted; loader time is kept apart from kernel and desktop time. Ranges are
separate runs; the one slow i386 boot overlapped other host work. [Study §37](I386_PORT_STUDY.md#37-catalog-ports-and-performance-against-v040)
has the method and the raw figures; [§31](I386_PORT_STUDY.md#31-timing-with-the-apic-mode-controlled)
has the compositor, pipe and idle samples.

| Measurement | v0.4.0 | i386 | Status |
| --- | --- | --- | --- |
| Boot: kernel banner to `handoff` | 2.3–3.3 s | 2.3–3.2 s, one run 5.3 s | Matched |
| Boot: `handoff` to focused Program Manager | 3.6–5.3 s | 4.3–5.1 s, one run 10.9 s | Matched |
| Loader: QEMU start to kernel banner | 4.9–5.4 s (Limine) | 1.8–2.7 s (GRUB) | Justified: loader, see the Loader row |
| Launch to focused window: Shell, Files, Calculator | 0.31–0.41 s | 0.33–0.40 s | Matched |
| Doom frame rate, `GENMIDI` music | 13–23 fps; music RMS 0.123 | 12–19 fps; music RMS 0.132 | Matched |
| Celeste music over the same 78 s session | 27 s audible, RMS 0.036 | 27 s audible, RMS 0.036 | Matched |
| FFmpeg library load (`ffmpegload`) | 2202 ms; map without reading 3 ms | 2128 ms; map without reading 1 ms | Matched |
| ATA write + sync of 512 KiB (`paritybench`) | 358 s | 373–444 s | Matched: SxFS commit cost on PIO in both |
| ATA read of 512 KiB | 3160 KiB/s | 3261–3531 KiB/s | Matched |
| TCP receive on RTL8139, 4 MiB | 7049 KiB/s | 8393 KiB/s | Matched |
| TCP echo on RTL8139, 1 MiB | 507 KiB/s | 483 KiB/s | Matched |
| Compositor compose / sync / present medians | 538 / 470 / 1158 µs | 794 / 1374 / 1495 µs | Justified: 32-bit copies and arithmetic |
| Pipe round trips, 1 and 4 vCPUs (`smptest`) | 307–323 / 810–893 ms | 514–536 / 1881–1962 ms | Justified: 32-bit copies and arithmetic |
| Host CPU with the guest idle, headless / desktop | 21.7 % / 45.6 % | 34.8 % / 46.4 % | Justified: both halt; tick cost differs |

The justified rows share one cause: i386 has half the register width.
Framebuffer sync and pipe transfers move four bytes per instruction instead
of eight, and every 64-bit time or counter operation, including the
monotonic clock read twice per timer tick, goes through register pairs and
`libgcc` division helpers. Under TCG each guest instruction costs roughly
the same host time, so these paths take longer without any difference in
behavior. The disk, network, launch and boot figures, where device
emulation or I/O dominates, match. These rows remain the place to record
any later optimization.

## Reference scenarios

Every v0.4.0 smoke catalog name now runs on i386. The core `smoke` spec,
the loader diagnostics, `ffmpegload`, `ccleste-selftest`, `native-hello`
and three aliases were the last ones; see
[study §32](I386_PORT_STUDY.md#32-the-reference-core-smoke-and-the-remaining-scenario-names).

Before that, five scenarios still lacked i386 counterparts.
`mediaplayer-display` invokes the port's frame callback, `mediaplayer-nolib`
and `mediaplayer-noport` remove the engine from an isolated SxFS copy, and
`native-guihost` and `native-sxgui` build their optional clients. The
`mediaplayer-selftest` entry point also works directly. See
[study §28](I386_PORT_STUDY.md#28-port-specific-smoke-scenarios-and-the-reference-failure-paths).

## Outcome

Every row above is **Matched** or **Justified**. With this review the
v0.4.0 target is met in emulation, and the next target is v0.4.1 as
[I386_PORT.md](I386_PORT.md#version-parity-is-the-objective) defines.
