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
import io
import os
from pathlib import Path
import re
import subprocess
import tempfile
import time

from mkinitramfs import write_entry
from qmp_client import QmpClient, send_kbd_smoke_actions, send_pointer_smoke_actions
from run_qemu import build_qemu_args, find_qemu, sound_device

ROOT = Path(__file__).resolve().parent.parent


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


# Commands, tokens, timeouts and preparation follow the v0.4.0 catalog.
SPEC_SCENARIOS: dict[str, SpecScenario] = {
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


def run(name: str, iso: Path, disk: Path, artifacts: Path, accel: str,
        timeout: float | None, virtio: bool, smp: int) -> int:
    scenario = SPEC_SCENARIOS[name]
    if scenario.requires_virtio and not virtio:
        raise ValueError(f"{name} requires --virtio, as in v0.4.0")
    artifacts.mkdir(parents=True, exist_ok=True)
    for command in scenario.prepare:
        with (artifacts / "prepare.log").open("a") as log:
            subprocess.run(command, shell=True, cwd=ROOT, stdout=log, stderr=log, check=True)
    serial = artifacts / "serial.log"
    serial.write_text("")
    deadline_seconds = timeout if timeout is not None else scenario.timeout
    with tempfile.TemporaryDirectory(prefix="sx-i386-spec.") as temporary:
        qmp = Path(temporary) / "qmp"
        boot_iso = make_spec_iso(iso.resolve(), Path(temporary), scenario.command)
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
        pattern = re.compile(f"({re.escape(scenario.success)}|{re.escape(scenario.failure)})")
        with (artifacts / "qemu.log").open("w") as log:
            guest = subprocess.Popen(command, stdout=log, stderr=log)
            try:
                deadline = time.monotonic() + deadline_seconds
                driven = not scenario.qmp_driver
                while time.monotonic() < deadline:
                    text = serial.read_text(errors="replace")
                    match = pattern.search(text)
                    if match:
                        break
                    if not driven and scenario.ready in text:
                        with QmpClient(qmp) as client:
                            if scenario.qmp_driver == "kbd":
                                send_kbd_smoke_actions(client)
                            else:
                                send_pointer_smoke_actions(client)
                        driven = True
                    if "kernel: fatal" in text or "BOOT FAIL" in text:
                        raise RuntimeError("the kernel stopped before the spec finished; see serial.log")
                    if guest.poll() is not None:
                        raise RuntimeError(f"QEMU exited with {guest.returncode}; see qemu.log")
                    time.sleep(0.25)
                else:
                    raise RuntimeError(f"no {scenario.success!r} within {deadline_seconds:.0f} s")
                if match[1] == scenario.failure:
                    raise RuntimeError(f"{scenario.failure}; see {serial}")
                if "user: exception #" in text:
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
