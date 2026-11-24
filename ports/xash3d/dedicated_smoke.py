#!/usr/bin/env python3
"""Boot the dedicated server with the user's Half-Life data on a disposable disk."""

from __future__ import annotations

import fcntl
import os
from pathlib import Path
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import time

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))

from run_qemu import build_qemu_args, find_qemu, prepare_image
from run_smoke import make_smoke_copy, stop_process, tail

GUEST_PORT = 27015
INFO_REQUEST = b"\xff\xff\xff\xffTSource Engine Query\0"
INFO_REPLY = b"\xff\xff\xff\xffI"
INFO_CHALLENGE = b"\xff\xff\xff\xffA"


def local_files(directory: Path):
    for path in sorted(directory.rglob("*")):
        if path.is_file() and not path.is_symlink():
            yield path


def pack_sounds(valve: Path, archive: Path) -> int:
    records = []
    with archive.open("wb") as output:
        output.write(b"PACK" + bytes(8))
        for asset in local_files(valve / "sound"):
            name = os.fsencode(asset.relative_to(valve).as_posix())
            if len(name) >= 56:
                raise ValueError(f"PAK name is too long: {asset.relative_to(valve)}")
            offset = output.tell()
            with asset.open("rb") as source:
                shutil.copyfileobj(source, output)
            records.append(struct.pack("<56sII", name, offset, output.tell() - offset))
        if not 0 < len(records) <= 65536:
            raise ValueError(f"expected 1 to 65536 sound files, found {len(records)}")
        offset = output.tell()
        if offset + 64 * len(records) > 0x7fffffff:
            raise ValueError("sound PAK exceeds the engine's signed 32-bit format")
        output.writelines(records)
        output.seek(4)
        output.write(struct.pack("<II", offset, 64 * len(records)))
    return len(records)


def stage_valve(valve: Path, target: Path, module: Path) -> None:
    for name in ("maps/c0a0.bsp", "delta.lst", "skill.cfg"):
        source = valve / name
        if not source.is_file() or source.is_symlink():
            raise ValueError(f"missing Half-Life fixture: {source}")
    if not any((valve / name).is_file() for name in ("liblist.gam", "gameinfo.txt")):
        raise ValueError(f"{valve} has no Half-Life game metadata")

    sources = [valve / name for name in ("maps/c0a0.bsp", "delta.lst", "skill.cfg",
                                          "liblist.gam", "gameinfo.txt")]
    sources.extend(sorted(valve.glob("*.wad")))
    for name in ("models", "sprites", "events", "scripts"):
        sources.extend(local_files(valve / name))
    for source in sources:
        if source.is_file() and not source.is_symlink():
            destination = target / source.relative_to(valve)
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(source, destination)

    (target / "server.cfg").write_text("sv_lan 1\n", encoding="ascii")
    dlls = target / "dlls"
    dlls.mkdir()
    shutil.copy2(module, dlls / module.name)
    entries = pack_sounds(valve, target / "sounds.pak")
    print(f"xash3d-dedicated: packed {entries} sound files", flush=True)


def check_server(process: subprocess.Popen, client: socket.socket, timeout: float) -> None:
    request = INFO_REQUEST
    last_reply = b""
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline and process.poll() is None:
        try:
            client.send(request)
            reply = client.recv(2048)
        except ConnectionRefusedError:
            time.sleep(0.1)
            continue
        except TimeoutError:
            continue

        last_reply = reply
        if reply.startswith(INFO_CHALLENGE) and len(reply) >= 9:
            request = INFO_REQUEST + reply[5:9]
            continue
        if reply.startswith(INFO_REPLY):
            fields = reply[6:].split(b"\0", 4)
            if (len(fields) == 5 and fields[1:3] == [b"c0a0", b"valve"]
                    and fields[3] and fields[4][5:6] == b"d"):
                print("xash3d-dedicated: UDP reply reports c0a0, valve and dedicated", flush=True)
                return
    raise RuntimeError(f"no dedicated server info for c0a0 (last reply {last_reply!r})")


def run_smoke(valve: Path, output: Path) -> None:
    qemu = find_qemu()
    if not qemu:
        raise RuntimeError("qemu-system-x86_64 was not found")
    subprocess.run(["./build.sh", "build"], cwd=ROOT, check=True)
    cli = output / "tools/sxfs-cli"
    source_disk = output / "disk.img"
    log_dir = output / "smoke-logs"
    log_dir.mkdir(parents=True, exist_ok=True)
    log = f"xash3d-dedicated.{os.getpid()}"
    serial = log_dir / f"{log}.serial.log"
    qemu_log = log_dir / f"{log}.qemu.log"

    with tempfile.TemporaryDirectory(prefix="sxxash-", dir="/tmp") as directory:
        work = Path(directory)
        disk = work / "disk.img"
        with source_disk.with_name("disk.img.lock").open("a+") as lock:
            fcntl.flock(lock, fcntl.LOCK_EX)
            make_smoke_copy(source_disk, disk, cli)

        env = dict(os.environ, SAVANXP_DISK_IMAGE=str(disk), SAVANXP_SXFS_CLI=str(cli))
        subprocess.run(["./ports/xash3d/build.sh"], cwd=ROOT, env=env, check=True)
        stage = work / "stage"
        module = output / "ports/xash3d/hl_savanxp_amd64.so"
        stage_valve(valve, stage / "games/xash3d-smoke/valve", module)
        subprocess.run([sys.executable, "tools/sxfs_sync.py", "--target", disk,
                        "--source", stage, "--cli", cli], cwd=ROOT, check=True)

        image = work / "image"
        shutil.copytree(output / "image", image)
        rootfs = work / "rootfs"
        shutil.copytree(output / "rootfs", rootfs)
        (rootfs / "SMOKE").write_text("xash3d-dedicated\n", encoding="ascii")
        subprocess.run([sys.executable, "tools/mkinitramfs.py", "--source", rootfs,
                        "--output", image / "boot/initramfs.cpio"], cwd=ROOT, check=True)
        monitor = work / "monitor"
        debug = log_dir / f"{log}.debugcon.log"
        code, variables = prepare_image(image_root=image, disk_image=disk, ovmf_code=None,
                                        ovmf_vars=None, vars_copy=work / "vars.fd", debug_log=debug)
        args = build_qemu_args(
            image_root=image, disk_image=disk, code=code, vars_copy=variables, debug_log=debug,
            headless=True, debug=False, accel=os.environ.get("SAVANXP_ACCEL", "tcg"),
            smp=int(os.environ.get("SAVANXP_SMP", "2")),
            virtio=os.environ.get("SAVANXP_VIRTIO") == "ON",
            monitor_path=monitor, serial_path=serial,
        )
        args[0] = qemu
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as reserve:
            reserve.bind(("127.0.0.1", 0))
            host_port = reserve.getsockname()[1]
        args[args.index("user,id=net0")] += f",hostfwd=udp:127.0.0.1:{host_port}-:{GUEST_PORT}"

        timeout = float(os.environ.get("SAVANXP_SMOKE_TIMEOUT", "60"))
        with qemu_log.open("w") as output_log, socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as client:
            print(f"xash3d-dedicated: console log {serial}", flush=True)
            client.settimeout(0.4)
            client.connect(("127.0.0.1", host_port))
            process = subprocess.Popen(args, stdout=output_log, stderr=output_log, start_new_session=True)
            try:
                check_server(process, client, timeout)
            except RuntimeError:
                print(tail(qemu_log, 10), file=sys.stderr)
                print(tail(serial, 35), file=sys.stderr)
                raise
            finally:
                stop_process(process, monitor)


def main() -> int:
    value = os.environ.get("SAVANXP_XASH3D_VALVE")
    if not value:
        print("xash3d-dedicated: skipped (set SAVANXP_XASH3D_VALVE to your Half-Life valve directory)")
        return 77
    output = Path(os.environ.get("SAVANXP_OUTPUT_ROOT", ROOT / "build")).resolve()
    try:
        run_smoke(Path(value).resolve(), output)
    except (OSError, ValueError, RuntimeError, subprocess.CalledProcessError) as error:
        print(f"XASH3D DEDICATED FAIL: {error}", file=sys.stderr)
        return 1
    print("XASH3D DEDICATED PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
