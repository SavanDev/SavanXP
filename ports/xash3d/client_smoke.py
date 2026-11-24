#!/usr/bin/env python3
"""Run the POSIX client and verify a Half-Life save round-trip in QEMU."""

from __future__ import annotations

import fcntl
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time

from dedicated_smoke import ROOT
from run_qemu import build_qemu_args, find_qemu, prepare_image
from run_smoke import make_smoke_copy, stop_process, tail
from shoot_session import Failure, Qmp, Session, wait_for_marker

GAME_PATH = Path("users/user/games/xash3d")
SAVE_NAME = "savanxp_client"


def run(command, **kwargs):
    subprocess.run(command, cwd=ROOT, check=True, **kwargs)


def require_marker(serial: Path, marker: str, timeout: float) -> None:
    if not wait_for_marker(serial, marker, timeout=timeout):
        raise RuntimeError(f"guest did not report {marker!r}")


def drive_client(session: Session, serial: Path, timeout: float) -> None:
    session.run("/bin/xash3d-client-smoke", wait=0)
    require_marker(serial, "client connected", timeout)
    require_marker(serial, "Spawn Server: c0a0", 1)
    time.sleep(4)
    session.shot("c0a0")
    session.qmp.tap("esc")
    time.sleep(2)
    session.shot("menu")
    session.qmp.tap("esc")
    session.qmp.tap("f6")
    require_marker(serial, f"Saving game to save/{SAVE_NAME}.sav", 60)
    session.qmp.tap("f7")
    require_marker(serial, f"Loading game from save/{SAVE_NAME}.sav", 60)
    require_marker(serial, "Restored server state for c0a0", 60)
    deadline = time.monotonic() + 60
    while time.monotonic() < deadline:
        lines = serial.read_text(errors="replace")
        restore = lines.rsplit(f"Loading game from save/{SAVE_NAME}.sav", 1)[-1]
        if "client connected" in restore:
            session.shot("c0a0-restored")
            print("xash3d-client: reconnected to c0a0 after loading the save", flush=True)
            return
        time.sleep(0.2)
    raise RuntimeError("client did not reconnect after loading the save")


def test_client(valve: Path, output: Path) -> None:
    qemu = find_qemu()
    if not qemu:
        raise RuntimeError("qemu-system-x86_64 was not found")
    run(["./build.sh", "build"])
    cli = output / "tools/sxfs-cli"
    source_disk = output / "disk.img"
    logs = output / "smoke-logs" / f"xash3d-client.{os.getpid()}"
    logs.mkdir(parents=True)
    serial = logs / "serial.log"

    with tempfile.TemporaryDirectory(prefix="sxclient-", dir="/tmp") as directory:
        work = Path(directory)
        disk = work / "disk.img"
        with source_disk.with_name("disk.img.lock").open("a+") as lock:
            fcntl.flock(lock, fcntl.LOCK_EX)
            make_smoke_copy(source_disk, disk, cli)
        stage = work / "stage"
        userconfig = stage / GAME_PATH / "valve/userconfig.cfg"
        bindings = f'bind "F6" "save {SAVE_NAME}"\nbind "F7" "load {SAVE_NAME}"\n'
        userconfig.parent.mkdir(parents=True)
        userconfig.write_text(bindings, encoding="ascii")
        launcher = stage / "bin/xash3d-client-smoke"
        launcher.parent.mkdir()
        run(["./tools/build-user.sh", "--source", "ports/xash3d/overlay/client_smoke_launcher.c",
             "--name", launcher.name, "--no-install", "--output", launcher])
        run([sys.executable, "tools/sxfs_sync.py", "--target", disk, "--source", stage, "--cli", cli])
        env = dict(os.environ, SAVANXP_DISK_IMAGE=str(disk), SAVANXP_SXFS_CLI=str(cli))
        run(["./ports/xash3d/build.sh", "--valve", valve], env=env)
        run([sys.executable, "tools/smoke_profile.py", "--sxfs-cli", cli, disk])

        monitor = work / "monitor"
        qmp_socket = work / "qmp"
        debug = logs / "debug.log"
        image = output / "image"
        code, variables = prepare_image(image_root=image, disk_image=disk, ovmf_code=None,
                                        ovmf_vars=None, vars_copy=work / "vars.fd", debug_log=debug)
        virtio = os.environ.get("SAVANXP_VIRTIO") == "ON"
        args = build_qemu_args(
            image_root=image, disk_image=disk, code=code, vars_copy=variables, debug_log=debug,
            headless=True, debug=False, accel=os.environ.get("SAVANXP_ACCEL", "tcg"),
            smp=int(os.environ.get("SAVANXP_SMP", "2")), virtio=virtio,
            monitor_path=monitor, qmp_path=qmp_socket, serial_path=serial,
        )
        args[0] = qemu
        with (logs / "qemu.log").open("w") as qemu_log:
            process = subprocess.Popen(args, stdout=qemu_log, stderr=qemu_log, start_new_session=True)
            try:
                print(f"xash3d-client: console and screenshots in {logs}", flush=True)
                require_marker(serial, "sessiond: session started", 90)
                session = Session(Qmp(qmp_socket, abs_pointer=virtio), str(logs), str(serial))
                drive_client(session, serial, float(os.environ.get("SAVANXP_SMOKE_TIMEOUT", "120")))
            except (Failure, RuntimeError):
                print(tail(serial, 28), file=sys.stderr)
                raise
            finally:
                stop_process(process, monitor)

        extracted = work / "extracted"
        run([cli, "extract", disk, extracted])
        if (extracted / GAME_PATH / "valve/userconfig.cfg").read_text(encoding="ascii") != bindings:
            raise RuntimeError("installing the game changed the existing user configuration")
        save = extracted / GAME_PATH / "valve/save" / f"{SAVE_NAME}.sav"
        with save.open("rb") as file:
            if file.read(4) != b"JSAV":
                raise RuntimeError(f"invalid save header in {save.name}")
        with save.with_suffix(".bmp").open("rb") as file:
            if file.read(2) != b"BM":
                raise RuntimeError("invalid save thumbnail")


def main() -> int:
    value = os.environ.get("SAVANXP_XASH3D_VALVE")
    if not value:
        print("xash3d-client: skipped (set SAVANXP_XASH3D_VALVE to your Half-Life valve directory)")
        return 77
    output = Path(os.environ.get("SAVANXP_OUTPUT_ROOT", ROOT / "build")).resolve()
    try:
        test_client(Path(value).resolve(), output)
    except (Failure, OSError, ValueError, RuntimeError, subprocess.CalledProcessError) as error:
        print(f"XASH3D CLIENT FAIL: {error}", file=sys.stderr)
        return 1
    print("XASH3D CLIENT PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
