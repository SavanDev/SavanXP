#!/usr/bin/env python3
"""Declarative catalog shared by the Linux smoke runner and build.sh."""

from __future__ import annotations

import argparse
from dataclasses import dataclass


@dataclass(frozen=True)
class Scenario:
    command: str
    success: str
    failure: str
    timeout: float = 120.0
    description: str = ""
    prepare: tuple[str, ...] = ()
    ready: str = ""
    qmp_driver: str = ""
    audio_device: str = "auto"
    wav_name: str = ""
    completion: str = "serial"
    ready_wait: float = 0.0
    host_server: str = ""
    host_test: str = ""
    remove_paths: tuple[str, ...] = ()
    qmp_callback: str = ""
    port_command: str = ""
    unsupported: str = ""


def _qemu_scenario(
    command: str,
    success: str,
    failure: str,
    description: str,
    timeout: float = 180.0,
    ready: str = "",
    qmp_driver: str = "",
    audio_device: str = "auto",
    wav_name: str = "",
    completion: str = "serial",
    ready_wait: float = 0.0,
    host_server: str = "",
    prepare: tuple[str, ...] = (),
    remove_paths: tuple[str, ...] = (),
    qmp_callback: str = "",
) -> Scenario:
    return Scenario(
        command=command,
        success=success,
        failure=failure,
        timeout=timeout,
        description=description,
        ready=ready,
        qmp_driver=qmp_driver,
        audio_device=audio_device,
        wav_name=wav_name,
        completion=completion,
        ready_wait=ready_wait,
        host_server=host_server,
        prepare=prepare,
        remove_paths=remove_paths,
        qmp_callback=qmp_callback,
    )


def _host_scenario(test_name: str, description: str) -> Scenario:
    return Scenario(
        command="host",
        success="PASS",
        failure="FAIL",
        description=description,
        host_test=test_name,
    )


def _unsupported(description: str, reason: str) -> Scenario:
    return Scenario(
        command="unsupported",
        success="UNSUPPORTED PASS",
        failure="UNSUPPORTED FAIL",
        description=description,
        unsupported=reason,
    )


SCENARIOS: dict[str, Scenario] = {
    "smoke": _qemu_scenario(
        "smoke", "SMOKE PASS", "SMOKE FAIL", "Core POSIX userland smoke test", 120.0
    ),
    "windowd-smoke": _qemu_scenario(
        "windowd-selftest",
        "WINDOWD SMOKE PASS",
        "WINDOWD SMOKE FAIL",
        "Window manager and compositor self-test",
    ),
    "progman-smoke": _qemu_scenario(
        "progman-selftest",
        "PROGMAN SMOKE PASS",
        "PROGMAN SMOKE FAIL",
        "Program Manager registry self-test",
        prepare=("./tools/restore_doom.sh",),
    ),
    "appwiz-smoke": _qemu_scenario(
        "appwiz-selftest",
        "APPWIZ SMOKE PASS",
        "APPWIZ SMOKE FAIL",
        "Application Wizard self-test",
        prepare=("./tools/restore_doom.sh",),
    ),
    "taskmgr-smoke": _qemu_scenario(
        "taskmgr-selftest",
        "TASKMGR SMOKE PASS",
        "TASKMGR SMOKE FAIL",
        "Task Manager process and CPU self-test",
    ),
    "mines-smoke": _qemu_scenario(
        "mines-selftest",
        "MINES SMOKE PASS",
        "MINES SMOKE FAIL",
        "Mines game and persistent settings self-test",
    ),
    "calc-smoke": _qemu_scenario(
        "calc-selftest",
        "CALC SMOKE PASS",
        "CALC SMOKE FAIL",
        "Calculator state-machine self-test",
    ),
    "clock-smoke": _qemu_scenario(
        "clocktest",
        "CLOCK SMOKE PASS",
        "CLOCK SMOKE FAIL",
        "Clock and RTC self-test",
    ),
    "sxe-smoke": _qemu_scenario(
        "sxe-selftest",
        "SXE SMOKE PASS",
        "SXE SMOKE FAIL",
        "SXE resource parser and metadata self-test",
    ),
    "filesapp-smoke": _qemu_scenario(
        "filesapp-selftest",
        "FILESAPP SMOKE PASS",
        "FILESAPP SMOKE FAIL",
        "File manager associations and icon self-test",
    ),
    "net-smoke": _qemu_scenario(
        "netsmoke",
        "NET SMOKE PASS",
        "NET SMOKE FAIL",
        "User-network ICMP and ARP self-test",
    ),
    "cursor-repro": _qemu_scenario(
        "windowd-cursor-repro",
        "CURSOR REPRO PASS",
        "CURSOR REPRO FAIL",
        "Window cursor presentation regression",
    ),
    "gpu-soak": _qemu_scenario(
        "gputest --soak 96",
        "SOAK PASS",
        "SOAK FAIL",
        "GPU presentation and mode-switch soak",
        360.0,
    ),
    "ac97-stream": _qemu_scenario(
        "audiostream",
        "AUDIO STREAM PASS",
        "AUDIO STREAM FAIL",
        "AC'97 streaming audio",
        120.0,
        audio_device="ac97",
        wav_name="ac97-stream.wav",
    ),
    "ac97-count": _qemu_scenario(
        "audiostream",
        "AUDIO STREAM PASS",
        "AUDIO STREAM FAIL",
        "AC'97 stream counter",
        120.0,
        audio_device="ac97",
    ),
    "virtio-count": _qemu_scenario(
        "audiostream",
        "AUDIO STREAM PASS",
        "AUDIO STREAM FAIL",
        "Virtio sound counter",
        120.0,
        audio_device="virtio",
    ),
    "virtio-stream": _qemu_scenario(
        "audiostream",
        "AUDIO STREAM PASS",
        "AUDIO STREAM FAIL",
        "Virtio sound streaming audio",
        120.0,
        audio_device="virtio",
        wav_name="virtio-stream.wav",
    ),
    "virtio-record": _qemu_scenario(
        "audiorecord",
        "AUDIO RECORD PASS",
        "AUDIO RECORD FAIL",
        "Virtio sound recording",
        120.0,
        audio_device="virtio",
    ),
    "kbd-smoke": _qemu_scenario(
        "kbdtest",
        "KBD SMOKE PASS",
        "KBD SMOKE FAIL",
        "Keyboard input smoke test",
        180.0,
        "KBD SMOKE READY",
        "kbd",
    ),
    "taskbar-smoke": _qemu_scenario(
        "",
        "TASKBAR SMOKE PASS",
        "TASKBAR SMOKE FAIL",
        "Real desktop taskbar smoke test",
        300.0,
        "handoff: starting /bin/init",
        "taskbar",
        completion="host",
        ready_wait=45.0,
    ),
    "tcp-smoke": _qemu_scenario(
        "tcptest {port}",
        "TCP SMOKE PASS",
        "TCP SMOKE FAIL",
        "TCP loss and reordering smoke test",
        300.0,
        host_server="tcp",
    ),
    "float-smoke": _qemu_scenario(
        "floatsmoke",
        "FLOAT SMOKE PASS",
        "FLOAT SMOKE FAIL",
        "SSE floating-point smoke test",
        prepare=(
            "./tools/build-user.sh --source sdk/floatsmoke --name floatsmoke --sse",
        ),
    ),
    "mediaplayer-availability": _qemu_scenario(
        "mediaplayer-availability",
        "MEDIAPLAYER AVAILABLE",
        "MEDIAPLAYER UNAVAILABLE",
        "System Media Player optional-backend detection",
        180.0,
    ),
    "mediaplayer-missing": _qemu_scenario(
        "mediaplayer-availability",
        "MEDIAPLAYER UNAVAILABLE",
        "MEDIAPLAYER AVAILABLE",
        "System Media Player fallback detection when the FFmpeg backend is absent",
        180.0,
        remove_paths=("bin/mediaplayer-ffmpeg",),
    ),
    "mediaplayer-selftest": _qemu_scenario(
        "mediaplayer",
        "MEDIAPLAYER PASS",
        "MEDIAPLAYER FAIL",
        "System Media Player delegation to the installed FFmpeg backend",
        300.0,
    ),
    "mediaplayer-display": _qemu_scenario(
        "mediaplayer-show",
        "MEDIAPLAYER PASS",
        "MEDIAPLAYER FAIL",
        "FFmpeg Media Player display frame capture",
        300.0,
        "MEDIAPLAYER DISPLAY READY",
        qmp_callback="./ports/ffmpeg/tests/qmp_display.py",
    ),
    "ffmpeg-smoke": Scenario(
        command="port",
        success="FFMPEG SMOKE PASS",
        failure="FFMPEG SMOKE FAIL",
        description="FFmpeg Media Player two-phase selftest and display smoke",
        port_command="./ports/ffmpeg/smoke.sh",
    ),
    "native-guihost": _qemu_scenario(
        "guihost",
        "NATIVEGUI HOST PASS",
        "NATIVEGUI HOST FAIL",
        "Native GUI host smoke test",
        prepare=(
            "./subsystems/native/build.sh --name nativegui --source haxe-gui --install",
            "./tools/build-user.sh --source subsystems/native/test/guihost.c --name nativeguihost",
        ),
    ),
    "native-hello": _qemu_scenario(
        "nativehello",
        "NATIVE HELLO PASS",
        "NATIVE HELLO FAIL",
        "Native Haxe hello smoke test",
        prepare=(
            "./subsystems/native/build.sh --name nativehello --source haxe --install",
        ),
    ),
    "native-sxgui": _qemu_scenario(
        "sxguihost",
        "SXGUI HOST PASS",
        "SXGUI HOST FAIL",
        "Native SXGUI host smoke test",
        prepare=(
            "./subsystems/native/build.sh --name sxguiapp --source haxe-sxgui --install",
            "./tools/build-user.sh --source subsystems/native/test/sxguihost.c --name sxguihost",
        ),
    ),
    "gfx2d-test": _host_scenario("gfx2d-test", "Host GFX2D test"),
    "audio-test": _host_scenario("audio-test", "Host audio mixer test"),
    "partition-smoke": _host_scenario("partition-test", "Host partition table test"),
    "sxfs-smoke": _host_scenario("sxfs-volume-test", "Host SxFS volume test"),
}


def get_scenario(name: str) -> Scenario:
    try:
        return SCENARIOS[name]
    except KeyError as exc:
        choices = ", ".join(sorted(SCENARIOS)) or "(none)"
        raise SystemExit(f"unknown smoke scenario '{name}'; available: {choices}") from exc


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("name", nargs="?")
    parser.add_argument("--list", action="store_true")
    parser.add_argument(
        "--field",
        choices=(
            "command",
            "success",
            "failure",
            "timeout",
            "description",
            "prepare",
            "ready",
            "qmp_driver",
            "audio_device",
            "wav_name",
            "completion",
            "ready_wait",
            "host_server",
            "host_test",
            "remove_paths",
            "qmp_callback",
            "port_command",
            "unsupported",
        ),
        required=False,
    )
    args = parser.parse_args()
    if args.list:
        for name, scenario in sorted(SCENARIOS.items()):
            print(f"{name}\t{scenario.description}")
        return 0
    if not args.name or not args.field:
        parser.error("name and --field are required unless --list is used")
    value = getattr(get_scenario(args.name), args.field)
    if args.field == "prepare":
        print("\n".join(value))
    elif args.field == "remove_paths":
        print("\n".join(value))
    else:
        print(value)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
