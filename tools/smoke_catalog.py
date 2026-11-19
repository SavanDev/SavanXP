#!/usr/bin/env python3
"""List the smoke scenarios currently implemented by the i386 branch."""
import argparse

SCENARIOS = {
    "smp-boot": "AP startup (--smp 2..32), private TSS, cache controls, IPIs, TLB and scheduling",
    "smp": "parallel user execution, remote IPI termination, pipes, CPU/FPU and memory reclamation (--smp 2..32)",
    "idle": "per-CPU ring-3 idle, process protection, sleep/busy accounting and Task Manager (--smp 1..32)",
    "wakeup": "event/semaphore/pipe/poll syscall handoff, delivery and cleanup (--smp 1..32; order on 1 CPU)",
    "hardening": "SMEP on every CPU, RDRAND kernel guard and fresh per-image user guards on -cpu max (--smp 1..32)",
    "kernel-preempt": "real blocking CONNECT preemption, pinned ring-0 CPU/FPU state and deferred kills (--smp 1..8)",
    "smoke": "memory/ELF32 libraries, reference POSIX/shell, CPU/FPU and desktop graphics",
    "boot": "kernel, persistent root and init/shell startup",
    "boot-screen": "reference splash artwork, fade, marquee and desktop handoff",
    "processes": "concurrent ELF32 workers, contexts and spawn/wait/exit",
    "shared": "memory objects, heap, fork, SDK integer ABI and ELF32 library calls",
    "shared-memory": "memory objects and ELF32 libraries with 32 MiB RAM",
    "posix": "reference shell, BusyBox, file descriptors, pipes, inheritance and cleanup",
    "memory": "32 MiB RAM, user fault isolation, page protection and ENOMEM rollback",
    "desktop": "reference window manager, Program Manager and taskbar",
    "windowd-smoke": "reference WM/surface protocol, capacity, reconnect and fullscreen selftest",
    "sxgui-smoke": "built SxGUI editor glyphs, taskbar label and wheel/scrollbar interaction",
    "taskbar": "reference taskbar activation, minimization and window state",
    "calc-smoke": "reference shared-library calculator and visible result",
    "services": "native devices, AC97 PCM, RTL8139, UDP/TCP, installed FFmpeg and Ccleste",
    "virtio-smoke": "virtio disk I/O, UDP/TCP, captured playback/gain and capture-ring reads",
    "audio-record": "virtio capture-ring completion with a deterministic silent input source",
    "device-smoke": "device permissions, killed audio owner, clipboard and PC speaker",
    "clock-smoke": "1000 Hz local APIC timer and shared monotonic time under sleep and load",
    "audio-smoke": "audio daemon selftest and captured AC97 PCM",
    "net-smoke": "RTL8139 AML/IOAPIC interrupt delivery, gateway packets and UDP loopback",
    "tcp-smoke": "reference TCP loss, reordering, delayed reads and socket deadlines",
    "doom": "installed Doom display, FM music and silence after window closure",
    "ccleste": "installed Celeste title, level interaction and changing game surface",
    "native-smoke": "optional installed Haxe nativehello ABI, containers, float and display",
    "partition-smoke": "ATA disk with an MBR SxFS slice, consistency and guest file I/O",
    "live-desktop": "standalone LiveCD desktop without an ATA disk, including Program Manager glyphs",
    "storage-smoke": "diskless GRUB LiveCD, reference ramdisk, SxFS consistency and guest writes",
    "power-smoke": "ACPI shutdown, cold reboot and firmware power-button SCI via IOAPIC and PIC",
    "media-smoke": "installed FFmpeg audio/video/sync and Ccleste asset/decoder selftests",
    "legacy-cpu": "QMP assertion that qemu32 exposes no long mode or NX",
    # v0.4.0 init automation specs, booted with /SMOKE in the CPIO root.
    "progman-smoke": "reference Program Manager registry selftest, with Doom restored",
    "appwiz-smoke": "reference Add/Remove Programs catalog selftest, with Doom restored",
    "taskmgr-smoke": "reference Task Manager process and CPU selftest",
    "mines-smoke": "reference Minesweeper board and persistent settings selftest",
    "filesapp-smoke": "reference Files associations and icon selftest",
    "sxe-smoke": "reference SXE resource parser and metadata selftest",
    "cursor-repro": "reference windowd cursor presentation regression",
    "gpu-soak": "reference GPU presentation and mode-switch soak (96 iterations)",
    "audiod-smoke": "reference audio daemon mixing two loopback clients on AC97, captured to WAV",
    "volume-smoke": "reference volume CLI sets and persists master volume on AC97",
    "float-smoke": "external SSE floating-point program built with build-user.sh --sse",
    "ac97-stream": "reference audiotest --stream on AC97, captured to WAV",
    "ac97-stream-quiet": "reference audiotest --stream-quiet at master volume 50 on AC97, captured to WAV",
    "ac97-count": "reference audiotest --stream counter on AC97",
    "virtio-count": "reference audiotest --stream counter on virtio-sound",
    "virtio-stream": "reference audiotest --stream on virtio-sound, captured to WAV",
    "virtio-record": "reference audiotest --record on virtio-sound",
    "kbd-smoke": "reference kbdtest driven by QMP keys",
    "pointer-smoke": "reference mousetest driven by QMP virtio-tablet moves and clicks (--virtio)",
    # v0.4.0 host scenarios, run through the existing CTest targets.
    "sxfs-smoke": "host SxFS volume test (sxfs-volume-test)",
    "sxmidi-smoke": "host MIDI synthesizer test (sxmidi-test)",
    "gfx2d-test": "host GFX2D painter test",
    "audio-test": "host audio mixer test",
}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("scenario", nargs="?", choices=SCENARIOS)
    parser.add_argument("--list", action="store_true")
    args = parser.parse_args()
    if args.scenario:
        print(SCENARIOS[args.scenario])
    else:
        for name, description in SCENARIOS.items():
            print(f"{name}: {description}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
