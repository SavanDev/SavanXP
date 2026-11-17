#!/usr/bin/env python3
"""Exercise native device services through the reference console shell."""
from __future__ import annotations
import argparse
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import time
import struct
import uuid
import zlib
from qmp_client import QmpClient, QmpError

from i386_console_iso import make_console_iso
from legacy_boot_test import type_shell, wait_serial, REFERENCE_PROMPT
from run_qemu import storage_network_args, sound_device, graphics_input_args


def make_partition_image(source: Path, destination: Path, gpt: bool) -> None:
    """Wrap the valid volume in a sparse MBR/GPT fixture, preserving the source."""
    start_lba = 2048
    sectors = source.stat().st_size // 512
    total = start_lba + sectors + (34 if gpt else 0)
    with source.open("rb") as reader, destination.open("wb") as writer:
        writer.truncate(total * 512)
        offset = start_lba * 512
        while chunk := reader.read(1 << 20):
            if chunk.strip(b"\0"):
                writer.seek(offset); writer.write(chunk)
            offset += len(chunk)
        mbr = bytearray(512)
        mbr[450] = 0xee if gpt else 0x83
        struct.pack_into("<II", mbr, 454, 1 if gpt else start_lba, total - 1 if gpt else sectors)
        mbr[510:512] = b"\x55\xaa"
        writer.seek(0); writer.write(mbr)
        if gpt:
            entries = bytearray(128 * 128)
            entries[:16] = uuid.UUID("0fc63daf-8483-4772-8e79-3d69d8477de4").bytes_le
            entries[16:32] = uuid.UUID(int=1).bytes_le
            struct.pack_into("<QQQ", entries, 32, start_lba, start_lba + sectors - 1, 0)
            for current, alternate, table in ((1, total - 1, 2), (total - 1, 1, total - 33)):
                header = bytearray(512)
                struct.pack_into("<8sIIIIQQQQ16sQIII", header, 0,
                    b"EFI PART", 0x10000, 92, 0, 0, current, alternate,
                    34, total - 34, uuid.UUID(int=2).bytes_le, table,
                    128, 128, zlib.crc32(entries))
                struct.pack_into("<I", header, 16, zlib.crc32(header[:92]))
                writer.seek(current * 512); writer.write(header)
                writer.seek(table * 512); writer.write(entries)


def run(iso: Path, disk: Path, artifacts: Path, scenario: str, timeout: float = 360, accel: str = "tcg", virtio: bool = False) -> int:
    if scenario == "virtio-smoke":
        run(iso, disk, artifacts / "services", "virtio-services", timeout, accel, True)
        return run(iso, disk, artifacts / "record", "audio-record", timeout, accel, True)
    virtio = virtio or scenario == "audio-record"
    if virtio and scenario == "storage-smoke":
        raise ValueError("storage-smoke is diskless; use virtio-smoke to check virtio storage")
    if scenario == "partition-smoke":
        for mode in ("partition-mbr", "partition-gpt"):
            run(iso, disk, artifacts / mode, mode, timeout, accel, virtio)
        return 0
    if scenario == "power-smoke":
        for mode in ("power-reboot", "power-shutdown", "power-button"):
            run(iso, disk, artifacts / mode, mode, timeout, accel, virtio)
        return 0
    artifacts.mkdir(parents=True, exist_ok=True)
    serial = artifacts / "serial.log"
    serial.write_text("")
    with tempfile.TemporaryDirectory(prefix="sx-i386-services.") as tmp:
        tmp = Path(tmp)
        console_iso = make_console_iso(iso, tmp)
        if scenario.startswith("partition-"):
            partitioned = tmp / "partitioned.img"
            make_partition_image(disk, partitioned, scenario == "partition-gpt")
            disk = partitioned
        qmp = tmp / "qmp"
        command = [os.environ.get("SAVANXP_QEMU", shutil.which("qemu-system-i386") or "qemu-system-i386"),
                   "-machine", "pc", "-cpu", "qemu32", "-accel", accel, "-m", "256M",
                   "-display", "none", "-boot", "d", "-cdrom", str(console_iso),
                   "-serial", f"file:{serial}", "-qmp", f"unix:{qmp},server=on,wait=off",
                   "-netdev", "user,id=net0",
                   "-audiodev", f"wav,id=audio0,path={artifacts / 'audio.wav'}",
                   "-device", sound_device(virtio), "-no-reboot", "-no-shutdown"]
        if scenario == "audio-record":
            # QEMU's WAV backend only implements playback. Its null backend
            # provides a deterministic silent capture source for RX testing.
            command[command.index("-audiodev") + 1] = "none,id=audio0"
        command += storage_network_args(disk, virtio=virtio, snapshot=True)
        command += graphics_input_args(virtio)
        if scenario == "storage-smoke":
            drive = command.index("-drive")
            del command[drive:drive + 2]
        if scenario == "power-reboot":
            command.remove("-no-reboot")
        echo_server = None
        echo_log = None
        if scenario in ("services", "tcp-smoke", "virtio-services"):
            port_file = tmp / "tcp-port"
            echo_log = (artifacts / "tcp-server.log").open("w")
            echo_server = subprocess.Popen([shutil.which("python3"), str(Path(__file__).with_name("tcp_echo_server.py")),
                "--port-file", str(port_file)], stdout=echo_log, stderr=echo_log)
            deadline = time.monotonic() + 10
            while not port_file.exists():
                if echo_server.poll() is not None or time.monotonic() > deadline:
                    raise RuntimeError("TCP echo server failed to start")
                time.sleep(.05)
            tcp_port = int(port_file.read_text())
        with (artifacts / "qemu.log").open("w") as log:
            process = subprocess.Popen(command, stdout=log, stderr=log)
            try:
                wait_serial(serial, re.escape(REFERENCE_PROMPT), timeout=min(timeout, 90))
                if virtio:
                    boot_log = serial.read_text(errors="replace")
                    for marker in ("virtio-blk: ready", "virtio-sound: ready", "capture=on", "vblk0:",
                                   "virtio-gpu: ready", "virtio-input: tablet ready", "virtio-input: keyboard ready"):
                        if marker not in boot_log:
                            raise RuntimeError(f"virtio backend missing: {marker}")
                    if "ata0:" in boot_log or "rtl8139: ready" in boot_log:
                        raise RuntimeError("virtio smoke fell back to a legacy disk/NIC")
                if scenario.startswith("power-"):
                    if "acpi: FADT ok" not in serial.read_text():
                        raise RuntimeError("the ACPI tables were not parsed")
                    offset = len(serial.read_text(errors="replace"))
                    if scenario == "power-button":
                        with QmpClient(qmp) as client:
                            client.execute("system_powerdown")
                    else:
                        mode = "reboot" if scenario == "power-reboot" else "shutdown"
                        try:
                            type_shell(qmp, (f"devtest32 {mode}",))
                        except QmpError as error:
                            # The Enter-down event can power off the guest
                            # before the matching key-up is sent.
                            if "VM not running" not in str(error): raise
                        wait_serial(serial, "DEV32 " + mode.upper() + " REQUEST", offset)
                    if scenario == "power-reboot":
                        wait_serial(serial, "I386 BOOT OK.*?" + re.escape(REFERENCE_PROMPT), offset, timeout=90)
                    else:
                        deadline = time.monotonic() + 15
                        while True:
                            with QmpClient(qmp) as client:
                                status = client.execute("query-status")["return"]["status"]
                            if status == "shutdown": break
                            if time.monotonic() > deadline:
                                raise RuntimeError("the guest did not power off")
                            time.sleep(.1)
                    print(f"i386 {scenario}: PASS", flush=True)
                    return 0
                if scenario == "storage-smoke":
                    boot_log = serial.read_text(errors="replace")
                    if "live0:" not in boot_log or "ata0:" in boot_log:
                        raise RuntimeError("the diskless boot did not select the LiveCD ramdisk")
                partition = "vblk0p1:" if virtio else "ata0p1:"
                if scenario.startswith("partition-") and partition not in serial.read_text(errors="replace"):
                    raise RuntimeError("the disk partition was not enumerated")
                commands = []
                if scenario in ("storage-smoke", "virtio-services") or scenario.startswith("partition-"):
                    commands += [("fscheck", "result: clean"),
                                 ("echo live-ramdisk > /disk/live-test.txt", "process exited with status 0"),
                                 ("cat /disk/live-test.txt", "live-ramdisk")]
                if scenario in ("services", "device-smoke", "virtio-services"):
                    commands += [("devtest32 virtio" if virtio else "devtest32", "DEV32 PASS"), ("cliptest", "cliptest: PASS"), ("beep 440 100", "process exited with status 0"), ("fscheck", "result: clean")]
                if scenario in ("services", "net-smoke", "virtio-services"):
                    commands += [("nettest", "NET SMOKE PASS"), ("udptest", "udp ok from")]
                if scenario in ("services", "audio-smoke", "virtio-services"):
                    commands += [("audiod --selftest", "AUDIOD SELFTEST PASS"),
                                 ("audiotest --stream", "AUDIO STREAM PASS"),
                                 ("audiotest --stream-quiet", "AUDIO STREAM QUIET PASS")]
                if scenario == "audio-record":
                    commands += [("audiotest --record", "AUDIO RECORD PASS")]
                if scenario in ("services", "tcp-smoke", "virtio-services"):
                    commands += [(f"tcptest {tcp_port}", "TCP SMOKE PASS")]
                if scenario in ("services", "media-smoke"):
                    commands += [("mediaplayer --selftest /disk/media/tono.wav", "MEDIAPLAYER PASS"),
                                 ("mediaplayer --selftest /disk/media/clip.mjpeg", "MEDIAPLAYER PASS"),
                                 ("mediaplayer --selftest --sync /disk/media/avsync.avi", "MEDIAPLAYER PASS"),
                                 ("ccleste --selftest", "CCLESTE SELFTEST PASS")]
                if scenario == "native-smoke":
                    commands += [("nativehello", "NATIVE HELLO PASS"),
                                 ("nativeguihost", "NATIVEGUI HOST PASS"),
                                 ("nativesxguihost", "SXGUI HOST PASS")]
                stream_peaks = {}
                for command, token in commands:
                    offset = len(serial.read_text(errors="replace"))
                    capture = artifacts / "audio.wav"
                    capture_start = capture.stat().st_size if capture.exists() else 44
                    type_shell(qmp, (command,))
                    wait_serial(serial, re.escape(REFERENCE_PROMPT), offset, timeout=timeout)
                    text = serial.read_text(errors="replace")[offset:]
                    if token not in text or not re.search(r"process exited with status 0,", text):
                        raise RuntimeError(f"{command} failed: {text[-3000:]}")
                    if "exception vector" in text or "kernel: fatal" in text:
                        raise RuntimeError(f"{command} faulted: {text[-3000:]}")
                    if virtio and command == "nettest" and "virtio-net: up" not in text:
                        raise RuntimeError("nettest did not bring up the virtio NIC")
                    if command.startswith("audiotest --stream"):
                        pcm = capture.read_bytes()[capture_start:]
                        pcm = pcm[:len(pcm) // 2 * 2]
                        # The host sink may flush a previous stream's tail
                        # only when playback resumes. Use the steady tone's
                        # 90th percentile, excluding brief boundary transients;
                        # an unscaled quiet stream still fails the ratio gate.
                        levels = sorted(abs(value[0]) for value in struct.iter_unpack("<h", pcm) if value[0])
                        stream_peaks[command] = levels[len(levels) * 9 // 10] if levels else 0
                        (artifacts / ("quiet.pcm" if command.endswith("-quiet") else "normal.pcm")).write_bytes(pcm)
                    print(f"i386 {command}: PASS", flush=True)
                if scenario in ("services", "media-smoke"):
                    offset = len(serial.read_text(errors="replace"))
                    type_shell(qmp, ("mediaplayer --gpu-hold 10000 /disk/media/avsync.avi",))
                    wait_serial(serial, "MEDIAPLAYER DISPLAY READY", offset, timeout=timeout)
                    frame = artifacts / "media-frame.ppm"
                    with QmpClient(qmp) as client:
                        client.screenshot(frame)
                    from PIL import Image
                    with Image.open(frame) as image:
                        image.save(artifacts / "media-frame.png")
                        colors = image.convert("RGB").crop((320, 200, 960, 600)).getcolors(1 << 20)
                        if len(colors or []) < 16:
                            raise RuntimeError("Media Player did not draw the decoded frame")
                    wait_serial(serial, "MEDIAPLAYER PASS.*?" + re.escape(REFERENCE_PROMPT), offset, timeout=timeout)
                    print("i386 Media Player decoded display: PASS", flush=True)
                with QmpClient(qmp) as client:
                    client.execute("quit")
                process.wait(timeout=10)
                if scenario in ("services", "audio-smoke", "virtio-services"):
                    normal = stream_peaks.get("audiotest --stream", 0)
                    quiet = stream_peaks.get("audiotest --stream-quiet", 0)
                    if not normal or not .4 * normal <= quiet <= .6 * normal:
                        raise RuntimeError(f"master gain did not halve PCM amplitude: {normal} -> {quiet}")
                    print(f"i386 master gain PCM: PASS ({normal} -> {quiet})", flush=True)
                    pcm = (artifacts / "audio.wav").read_bytes()
                    # QEMU's WAV backend can leave both RIFF/data sizes zero
                    # even after QMP quit. Validate the fixed PCM header and
                    # inspect the bytes actually captured, rather than trusting
                    # an advertised frame count of zero.
                    if len(pcm) < 44 or pcm[:4] != b"RIFF" or pcm[8:16] != b"WAVEfmt " or pcm[36:40] != b"data":
                        raise RuntimeError("unexpected captured WAV header")
                    encoding, channels, rate, byte_rate, align, bits = struct.unpack_from("<HHIIHH", pcm, 20)
                    if (encoding, channels, bits, align) != (1, 2, 16, 4) or byte_rate != rate * align:
                        raise RuntimeError("unexpected captured PCM format")
                    samples = struct.unpack("<" + "h" * ((len(pcm) - 44) // 2), pcm[44:])
                    active = sum(value != 0 for value in samples)
                    if active < rate or min(samples) >= -1000 or max(samples) <= 1000:
                        raise RuntimeError("playback capture did not contain the test tone")
                    print(f"i386 {'virtio' if virtio else 'AC97'} captured PCM: PASS ({active} nonzero samples)", flush=True)
                return 0
            finally:
                if echo_server:
                    echo_server.terminate()
                    try: echo_server.wait(timeout=5)
                    except subprocess.TimeoutExpired:
                        echo_server.kill(); echo_server.wait()
                    echo_log.close()
                process.terminate()
                try: process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill(); process.wait()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--iso", type=Path, required=True)
    parser.add_argument("--disk-image", type=Path, required=True)
    parser.add_argument("--artifacts", type=Path, required=True)
    parser.add_argument("--scenario", choices=("services", "net-smoke", "audio-smoke", "audio-record", "tcp-smoke", "media-smoke", "device-smoke", "power-smoke", "native-smoke", "storage-smoke", "partition-smoke", "virtio-smoke"), default="services")
    parser.add_argument("--virtio", action="store_true")
    args = parser.parse_args()
    return run(args.iso.resolve(), args.disk_image.resolve(), args.artifacts.resolve(), args.scenario, virtio=args.virtio)

if __name__ == "__main__":
    raise SystemExit(main())
