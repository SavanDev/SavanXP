#!/usr/bin/env python3
"""Exercise the reference desktop and SxGUI on the actual i386 boot artifacts."""
from __future__ import annotations
import argparse
import os
from pathlib import Path
import shutil
import signal
import subprocess
import tempfile
import time

import shoot_session


def run(iso: Path, disk: Path, artifacts: Path, scenario: str, accel: str, timeout: float) -> int:
    artifacts.mkdir(parents=True, exist_ok=True)
    serial = artifacts / "serial.log"
    serial.unlink(missing_ok=True)
    with tempfile.TemporaryDirectory(prefix="sx-i386-graphics.") as tmp:
        socket = Path(tmp) / "qmp"
        command = [os.environ.get("SAVANXP_QEMU", shutil.which("qemu-system-i386") or "qemu-system-i386"),
                   "-machine", "pc", "-cpu", "qemu32", "-accel", accel,
                   "-m", "1024M", "-smp", "1", "-vga", "std", "-nic", "none",
                   "-display", "none", "-boot", "d", "-cdrom", str(iso),
                   "-drive", f"file={disk},format=raw,if=ide,snapshot=on",
                   "-serial", f"file:{serial}", "-qmp", f"unix:{socket},server=on,wait=off",
                   "-no-reboot", "-no-shutdown"]
        with (artifacts / "qemu.log").open("w") as log:
            process = subprocess.Popen(command, stdout=log, stderr=log)
            try:
                deadline = time.monotonic() + min(timeout, 90)
                while time.monotonic() < deadline:
                    text = serial.read_text(errors="replace") if serial.exists() else ""
                    if "user: /bin/progman loaded" in text:
                        break
                    if process.poll() is not None or "BOOT FAIL" in text:
                        raise RuntimeError("graphical boot failed; see serial.log")
                    time.sleep(.2)
                else:
                    raise RuntimeError("graphical init did not start Program Manager")
                def expired(_signum, _frame):
                    raise TimeoutError(f"{scenario} exceeded its {timeout:g}s deadline")
                previous_alarm = signal.signal(signal.SIGALRM, expired)
                signal.setitimer(signal.ITIMER_REAL, max(1, timeout))
                # Exercise the real reference UI and its built font tables.
                shots = shoot_session.run_scenario(
                    "notepadwheel" if scenario == "sxgui-smoke" else scenario,
                    socket, artifacts, boot_wait=5)
                if scenario == "desktop":
                    from PIL import Image
                    with Image.open(shots[-1]) as screen:
                        image = screen.convert("RGB")
                    bx, by, bw, bh = shoot_session.button_rect(image, 0)
                    shoot_session.expect_text(image, (bx, by, bx + bw, by + bh),
                        "Program Manager", shoot_session.TEXT, shoot_session.FACE,
                        "the default session taskbar")
                text = serial.read_text(errors="replace")
                if "exception vector" in text or "user: exception #" in text or "stack overflow" in text or "compositor startup failed" in text:
                    raise RuntimeError("a graphical process faulted; see serial.log")
                print(f"i386 {scenario}: PASS ({len(shots)} screen assertions)", flush=True)
                return 0
            finally:
                signal.setitimer(signal.ITIMER_REAL, 0)
                if "previous_alarm" in locals():
                    signal.signal(signal.SIGALRM, previous_alarm)
                process.terminate()
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--iso", type=Path, required=True)
    parser.add_argument("--disk-image", type=Path, required=True)
    parser.add_argument("--artifacts", type=Path, required=True)
    parser.add_argument("--scenario", default="sxgui-smoke")
    parser.add_argument("--accel", default="tcg")
    parser.add_argument("--timeout", type=float, default=360)
    args = parser.parse_args()
    return run(args.iso.resolve(), args.disk_image.resolve(), args.artifacts.resolve(), args.scenario, args.accel, args.timeout)


if __name__ == "__main__":
    raise SystemExit(main())
