#!/usr/bin/env python3
"""Run a v0.4.0 init automation spec on the i386 build.

The reference smoke runner plants `/SMOKE` in the boot root; `init` then runs
the named selftest instead of the desktop and prints `<LABEL> PASS` or
`<LABEL> FAIL`. This harness derives a disposable ISO whose CPIO carries that
file, boots it with the persistent disk in snapshot mode and waits for the
reference tokens.
"""
from __future__ import annotations

from dataclasses import dataclass
import fcntl
import io
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import time

from mkinitramfs import write_entry
import i386_graphics_test
from qmp_client import QmpClient, send_kbd_smoke_actions, send_pointer_smoke_actions
from run_qemu import build_qemu_args, find_qemu, sound_device
from sxfs_sync import copy_preserving_holes

ROOT = Path(__file__).resolve().parent.parent
FFMPEG_PREPARE = ("./ports/ffmpeg/install.sh --with-test-media",)
FFMPEG_LIBRARY = "lib/libffmpeg.so.0.4"


@dataclass(frozen=True)
class SpecScenario:
    command: str
    success: str
    failure: str
    timeout: float = 180.0
    prepare: tuple[str, ...] = ()
    wav_name: str = ""
    # Like v0.4.0, a driver-specific scenario forces its sound device.
    audio_device: str = ""
    requires_virtio: bool = False
    ready: str = ""
    qmp_driver: str = ""
    isolated_disk: bool = False
    remove_paths: tuple[str, ...] = ()
    required_output: str = ""
    visual_scenario: str = ""
    # Deliberate user faults, such as stacktest's overflow into the stack guard.
    allowed_faults: int = 0


# Commands, tokens, timeouts and preparation follow the v0.4.0 catalog.
SPEC_SCENARIOS: dict[str, SpecScenario] = {
    # `./build.sh smoke smoke` chains this core spec after the i386 console checks.
    "smoke": SpecScenario("smoke", "SMOKE PASS", "SMOKE FAIL", 120.0, allowed_faults=1),
    "needstest-missing": SpecScenario("needstest", "NEEDSTEST SURVIVED", "NEEDSTEST OK",
                                      remove_paths=("lib/libneeded.so.0.4",)),
    "brokentest": SpecScenario("brokentest", "BROKENTEST DIAGNOSED", "BROKENTEST OK"),
    "missingtest": SpecScenario("missingtest", "MISSINGTEST DIAGNOSED", "MISSINGTEST OK"),
    "slottest": SpecScenario("slottest", "SLOTTEST CAPPED", "SLOTTEST OK"),
    "diamondtest": SpecScenario("diamondtest", "DIAMONDTEST ONCED", "DIAMONDTEST OK"),
    "ffmpegload": SpecScenario("ffmpegload", "FFMPEGLOAD OK", "FFMPEGLOAD FAIL", 240.0,
                               prepare=FFMPEG_PREPARE, isolated_disk=True),
    "ccleste-selftest": SpecScenario("ccleste-selftest", "CCLESTE SELFTEST PASS", "CCLESTE SELFTEST FAIL",
                                     prepare=("./ports/ccleste/build.sh",), isolated_disk=True),
    "native-hello": SpecScenario("nativehello", "NATIVE HELLO PASS", "NATIVE HELLO FAIL", prepare=(
        "./subsystems/native/build.sh --name nativehello --source haxe --install",), isolated_disk=True),
    "progman-smoke": SpecScenario("progman-selftest", "PROGMAN SMOKE PASS", "PROGMAN SMOKE FAIL",
                                  900.0, prepare=("./tools/restore_doom.sh",)),
    "appwiz-smoke": SpecScenario("appwiz-selftest", "APPWIZ SMOKE PASS", "APPWIZ SMOKE FAIL",
                                 prepare=("./tools/restore_doom.sh",)),
    "taskmgr-smoke": SpecScenario("taskmgr-selftest", "TASKMGR SMOKE PASS", "TASKMGR SMOKE FAIL"),
    "mines-smoke": SpecScenario("mines-selftest", "MINES SMOKE PASS", "MINES SMOKE FAIL"),
    "filesapp-smoke": SpecScenario("filesapp-selftest", "FILESAPP SMOKE PASS", "FILESAPP SMOKE FAIL"),
    "sxe-smoke": SpecScenario("sxe-selftest", "SXE SMOKE PASS", "SXE SMOKE FAIL"),
    "cursor-repro": SpecScenario("windowd-cursor-repro", "CURSOR REPRO PASS", "CURSOR REPRO FAIL"),
    "gpu-soak": SpecScenario("gputest --soak 96", "SOAK PASS", "SOAK FAIL", 360.0),
    "audiod-smoke": SpecScenario("audiodselftest", "AUDIOD SELFTEST PASS", "AUDIOD SELFTEST FAIL",
                                 120.0, wav_name="audiod-smoke.wav", audio_device="ac97"),
    "volume-smoke": SpecScenario("volumesmoke", "SMOKE PASS", "SMOKE FAIL", 120.0, audio_device="ac97"),
    "ac97-stream": SpecScenario("audiostream", "AUDIO STREAM PASS", "AUDIO STREAM FAIL", 120.0,
                                wav_name="ac97-stream.wav", audio_device="ac97"),
    "ac97-stream-quiet": SpecScenario("audiostream-quiet", "AUDIO STREAM QUIET PASS", "AUDIO STREAM FAIL",
                                      120.0, wav_name="ac97-stream-quiet.wav", audio_device="ac97"),
    "ac97-count": SpecScenario("audiostream", "AUDIO STREAM PASS", "AUDIO STREAM FAIL", 120.0,
                               audio_device="ac97"),
    "virtio-count": SpecScenario("audiostream", "AUDIO STREAM PASS", "AUDIO STREAM FAIL", 120.0,
                                 audio_device="virtio"),
    "virtio-stream": SpecScenario("audiostream", "AUDIO STREAM PASS", "AUDIO STREAM FAIL", 120.0,
                                  wav_name="virtio-stream.wav", audio_device="virtio"),
    "virtio-record": SpecScenario("audiorecord", "AUDIO RECORD PASS", "AUDIO RECORD FAIL", 120.0,
                                  audio_device="virtio"),
    "kbd-smoke": SpecScenario("kbdtest", "KBD SMOKE PASS", "KBD SMOKE FAIL", ready="KBD SMOKE READY",
                              qmp_driver="kbd"),
    "pointer-smoke": SpecScenario("mousetest --selftest", "POINTER SMOKE PASS", "POINTER SMOKE FAIL",
                                  120.0, ready="POINTER SMOKE READY", qmp_driver="pointer",
                                  requires_virtio=True),
    "float-smoke": SpecScenario("floatsmoke", "FLOAT SMOKE PASS", "FLOAT SMOKE FAIL", prepare=(
        "./tools/build-user.sh --source sdk/floatsmoke --name floatsmoke --sse",)),
    "mediaplayer-selftest": SpecScenario("mediaplayer", "MEDIAPLAYER SMOKE PASS", "MEDIAPLAYER SMOKE FAIL",
                                        300.0, prepare=FFMPEG_PREPARE, isolated_disk=True),
    "mediaplayer-display": SpecScenario("mediaplayer-show", "MEDIAPLAYER DISPLAY PASS", "MEDIAPLAYER DISPLAY FAIL",
                                       300.0, prepare=FFMPEG_PREPARE, isolated_disk=True,
                                       ready="MEDIAPLAYER DISPLAY READY", qmp_driver="media"),
    "mediaplayer-nolib": SpecScenario("mediaplayer", "MEDIAPLAYER SMOKE FAIL status=2", "MEDIAPLAYER SMOKE FAIL",
                                     prepare=FFMPEG_PREPARE, isolated_disk=True, remove_paths=(FFMPEG_LIBRARY,),
                                     required_output="mediaplayer: falta libffmpeg.so.0.4"),
    "mediaplayer-noport": SpecScenario("", "", "", 300.0, prepare=FFMPEG_PREPARE,
                                      isolated_disk=True, remove_paths=(FFMPEG_LIBRARY,),
                                      visual_scenario="mediaplayer_unavailable"),
    "native-guihost": SpecScenario("guihost", "NATIVEGUI HOST PASS", "NATIVEGUI HOST FAIL", prepare=(
        "./subsystems/native/build.sh --name nativegui --source haxe-gui --install",
        "./tools/build-user.sh --source subsystems/native/test/guihost.c --name nativeguihost",
    ), isolated_disk=True),
    "native-sxgui": SpecScenario("sxguihost", "SXGUI HOST PASS", "SXGUI HOST FAIL", prepare=(
        "./subsystems/native/build.sh --name sxguiapp --source haxe-sxgui --install",
        "./tools/build-user.sh --source subsystems/native/test/sxguihost.c --name sxguihost",
    ), isolated_disk=True),
}


def make_spec_iso(source: Path, destination: Path, command: str) -> Path:
    """Copy `source` with `/SMOKE` added to its CPIO root."""
    if not command or len(command.encode("ascii")) > 63 or "\n" in command:
        raise ValueError("a spec is one ASCII line of at most 63 bytes, as init reads it")
    xorriso = os.environ.get("SAVANXP_XORRISO", "xorriso")
    cpio = destination / "initramfs.cpio"
    iso = destination / "spec.iso"
    log_path = destination / "spec-iso.log"
    with log_path.open("w") as log:
        subprocess.run([xorriso, "-osirrox", "on", "-indev", str(source),
                        "-extract", "/boot/initramfs.cpio", str(cpio)],
                       stdout=log, stderr=log, check=True)
        cpio.chmod(0o600)
        archive = cpio.read_bytes()
        trailer = archive.rfind(b"TRAILER!!!\0")
        if trailer < 110 or archive[trailer - 110:trailer - 104] != b"070701":
            raise ValueError("the supplied ISO has no newc CPIO trailer")
        entry = io.BytesIO()
        write_entry(entry, "SMOKE", (command + "\n").encode("ascii"), 0o100644)
        # Entries before the trailer end 4-byte aligned, so the new one stays aligned.
        cpio.write_bytes(archive[:trailer - 110] + entry.getvalue() + archive[trailer - 110:])
        result = subprocess.run([xorriso, "-indev", str(source), "-outdev", str(iso),
                                 "-boot_image", "any", "replay",
                                 "-map", str(cpio), "/boot/initramfs.cpio",
                                 "-commit", "-end"], stdout=log, stderr=log)
        if result.returncode:
            log.flush()
            raise RuntimeError(f"spec ISO failed: {log_path.read_text()[-3000:]}")
    return iso


def prepare_disk(scenario: SpecScenario, source: Path, build: Path,
                 temporary: Path, artifacts: Path) -> Path:
    disk = source
    env = os.environ.copy()
    cli = Path(env.get("SAVANXP_SXFS_CLI", build / "tools" / "sxfs-cli")).resolve()
    with (artifacts / "prepare.log").open("w") as log:
        if scenario.isolated_disk or scenario.remove_paths:
            disk = temporary / "disk.img"
            with source.with_name(source.name + ".lock").open("a+") as lock:
                fcntl.flock(lock.fileno(), fcntl.LOCK_EX)
                copy_preserving_holes(source, disk)
            subprocess.run([cli, "check", disk], stdout=log, stderr=subprocess.STDOUT, check=True)
            env.update(SAVANXP_DISK_IMAGE=str(disk), SAVANXP_SXFS_CLI=str(cli),
                       SAVANXP_OUTPUT_ROOT=str(build))
        for command in scenario.prepare:
            subprocess.run(command, shell=True, cwd=ROOT, env=env,
                           stdout=log, stderr=subprocess.STDOUT, check=True)
        for path in scenario.remove_paths:
            subprocess.run([cli, "rm", disk, path], stdout=log, stderr=subprocess.STDOUT, check=True)
        if scenario.remove_paths:
            subprocess.run([cli, "check", disk], stdout=log, stderr=subprocess.STDOUT, check=True)
    return disk


def run(name: str, iso: Path, disk: Path, artifacts: Path, accel: str,
        timeout: float | None, virtio: bool, smp: int) -> int:
    scenario = SPEC_SCENARIOS[name]
    if scenario.requires_virtio and not virtio:
        raise ValueError(f"{name} requires --virtio, as in v0.4.0")
    artifacts.mkdir(parents=True, exist_ok=True)
    serial = artifacts / "serial.log"
    serial.write_text("")
    deadline_seconds = timeout if timeout is not None else scenario.timeout
    with tempfile.TemporaryDirectory(prefix="sx-i386-spec.") as temporary:
        temporary = Path(temporary)
        if scenario.prepare or scenario.isolated_disk or scenario.remove_paths:
            disk = prepare_disk(scenario, disk.resolve(), iso.resolve().parent, temporary, artifacts)
        if scenario.visual_scenario:
            return i386_graphics_test.run(iso, disk, artifacts, scenario.visual_scenario,
                                          accel, deadline_seconds, virtio, smp)
        qmp = temporary / "qmp"
        boot_iso = make_spec_iso(iso.resolve(), temporary, scenario.command)
        command = build_qemu_args(iso=boot_iso, disk_image=disk, debug_log=artifacts / "debug.log",
                                  accel=accel, smp=smp, headless=True, virtio=virtio,
                                  snapshot=True, serial_path=serial,
                                  qmp_path=qmp if scenario.qmp_driver else None)
        command[0] = find_qemu() or "qemu-system-i386"
        if scenario.audio_device:
            index = command.index(sound_device(virtio))
            command[index] = sound_device(scenario.audio_device == "virtio")
        wav = artifacts / scenario.wav_name if scenario.wav_name else None
        if wav is not None:
            wav.unlink(missing_ok=True)
            index = command.index("-audiodev") + 1
            command[index] = f"wav,id=audio0,path={wav}"
        pattern = re.compile(rf"{re.escape(scenario.success)}(?=\s|$)|{re.escape(scenario.failure)}")
        with (artifacts / "qemu.log").open("w") as log:
            guest = subprocess.Popen(command, stdout=log, stderr=log)
            try:
                deadline = time.monotonic() + deadline_seconds
                driven = not scenario.qmp_driver
                while time.monotonic() < deadline:
                    text = serial.read_text(errors="replace")
                    if not driven and scenario.ready in text:
                        if scenario.qmp_driver == "media":
                            subprocess.run([sys.executable, ROOT / "ports/ffmpeg/tests/qmp_display.py",
                                            "--socket", qmp, "--out-dir", artifacts], check=True)
                        else:
                            with QmpClient(qmp) as client:
                                if scenario.qmp_driver == "kbd":
                                    send_kbd_smoke_actions(client)
                                else:
                                    send_pointer_smoke_actions(client)
                        driven = True
                    match = pattern.search(text[:text.rfind("\n") + 1])
                    if match:
                        break
                    if "kernel: fatal" in text or "BOOT FAIL" in text:
                        raise RuntimeError("the kernel stopped before the spec finished; see serial.log")
                    if guest.poll() is not None:
                        raise RuntimeError(f"QEMU exited with {guest.returncode}; see qemu.log")
                    time.sleep(0.25)
                else:
                    raise RuntimeError(f"no {scenario.success!r} within {deadline_seconds:.0f} s")
                if match[0] != scenario.success:
                    raise RuntimeError(f"{match[0]}; see {serial}")
                if not driven or scenario.required_output not in text:
                    raise RuntimeError(f"missing expected scenario output or QMP action; see {serial}")
                if text.count("user: exception #") > scenario.allowed_faults:
                    raise RuntimeError("a process faulted during the spec; see serial.log")
            finally:
                guest.terminate()
                try:
                    guest.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    guest.kill()
                    guest.wait()
    # QEMU finalizes the WAV when it exits; v0.4.0 requires a non-empty file.
    if wav is not None and (not wav.is_file() or wav.stat().st_size <= 44):
        raise RuntimeError(f"WAV output missing or empty: {wav}")
    print(f"i386 {name}: PASS ({scenario.command})", flush=True)
    return 0
