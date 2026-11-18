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

from qmp_client import QmpClient
import shoot_session
from run_qemu import storage_network_args, sound_device, graphics_input_args


def check_boot_screen(socket: Path, serial: Path, artifacts: Path, timeout: float) -> None:
    """Check the reference artwork and two animation phases on the GRUB scanout."""
    from PIL import Image
    from gen_boot_logo import read_system_name, render_logo, render_wordmark

    root = Path(__file__).resolve().parents[1]
    system_name = read_system_name(root)
    logo = render_logo(root / "assets/brand/logo.png")
    mark = render_wordmark(root / "assets/desktop/fonts/NotoSans-Regular.ttf",
                           system_name)
    texture = Image.frombytes("P", (logo["width"], logo["height"]), bytes(logo["indices"]))
    texture.putpalette(bytes((colour >> shift) & 255 for colour in logo["palette"]
                             for shift in (16, 8, 0)))
    texture = texture.convert("RGB")
    alpha = Image.frombytes("L", (mark["width"], mark["height"]), bytes(mark["coverage"]))
    wordmark = Image.merge("RGB", tuple(alpha.point(lambda a: a * channel // 255)
                                        for channel in (242, 246, 249))).tobytes()
    # The default 1280x800 mode uses the reference's unit scale.
    logo_x = (1280 - logo["width"]) // 2
    logo_y = 800 * 46 // 100 - (logo["height"] + 24 + mark["height"] + 62 + 16) // 2
    mark_x = (1280 - mark["width"]) // 2
    mark_y = logo_y + logo["height"] + 24
    bar_x = (1280 - 240) // 2
    band_y = mark_y + mark["height"] + 62 + 3
    logo_box = (logo_x, logo_y, logo_x + logo["width"], logo_y + logo["height"])
    mark_box = (mark_x, mark_y, mark_x + mark["width"], mark_y + mark["height"])
    fade_frames = {}
    for step in range(1, 33):
        fade = step * 255 // 32
        ramp = [value * fade // 255 for value in range(256)]
        fade_frames[texture.point(ramp * 3).tobytes()] = fade

    artifacts.mkdir(parents=True, exist_ok=True)
    frame = socket.parent / "boot-screen.ppm"
    dim = opaque = False
    positions = set()
    deadline = time.monotonic() + min(timeout, 90)
    with QmpClient(socket, timeout=5) as qmp:
        while time.monotonic() < deadline:
            log = serial.read_text(errors="replace") if serial.exists() else ""
            if "user: /bin/progman loaded" in log or "BOOT FAIL" in log:
                break
            if any(line.startswith(f"{system_name} v") and line.endswith(" booting...")
                   for line in log.splitlines()):
                qmp.screenshot(frame)
                with Image.open(frame) as image:
                    if image.size == (1280, 800):
                        fade = fade_frames.get(image.crop(logo_box).tobytes())
                        if fade == 255 and image.crop(mark_box).tobytes() == wordmark and not opaque:
                            image.save(artifacts / "opaque.png")
                            opaque = True
                        elif fade and fade < 255 and not dim:
                            image.save(artifacts / "fade.png")
                            dim = True
                        if fade and len(positions) < 2:
                            highlight = tuple(channel * fade // 255 for channel in (127, 211, 245))
                            position = next((x for x in range(bar_x + 2, bar_x + 238)
                                             if image.getpixel((x, band_y)) == highlight), None)
                            if position is not None and position not in positions:
                                positions.add(position)
                                image.save(artifacts / f"marquee-{len(positions)}.png")
                if dim and opaque and len(positions) >= 2:
                    print("i386 splash: artwork, fade and marquee PASS", flush=True)
                    return
            time.sleep(.08)
    raise RuntimeError(f"reference boot screen missing: fade={dim}, artwork={opaque}, marquee={len(positions)} positions")


def run(iso: Path, disk: Path, artifacts: Path, scenario: str, accel: str, timeout: float, virtio: bool = False, smp: int = 1) -> int:
    report_scenario = scenario
    diskless = scenario == "live-desktop"
    splash = scenario == "boot-screen"
    if diskless or splash: scenario = "desktop"
    artifacts.mkdir(parents=True, exist_ok=True)
    serial = artifacts / "serial.log"
    serial.unlink(missing_ok=True)
    with tempfile.TemporaryDirectory(prefix="sx-i386-graphics.") as tmp:
        socket = Path(tmp) / "qmp"
        command = [os.environ.get("SAVANXP_QEMU", shutil.which("qemu-system-i386") or "qemu-system-i386"),
                   "-machine", "pc", "-cpu", "qemu32", "-accel", accel,
                   "-m", "1024M", "-smp", str(smp),
                   "-netdev", "user,id=net0",
                   "-audiodev", "none,id=audio0", "-device", sound_device(virtio),
                   "-display", "none", "-boot", "d", "-cdrom", str(iso),
                   "-serial", f"file:{serial}", "-qmp", f"unix:{socket},server=on,wait=off",
                   "-no-reboot", "-no-shutdown"]
        command += storage_network_args(disk, virtio=virtio, snapshot=True)
        command += graphics_input_args(virtio)
        if diskless:
            drive = command.index("-drive")
            del command[drive:drive + 2]
        audio_path = artifacts / "audio.wav"
        if scenario in ("doom", "ccleste"):
            command[command.index("none,id=audio0")] = f"wav,id=audio0,path={audio_path},out.frequency=48000"
        with (artifacts / "qemu.log").open("w") as log:
            process = subprocess.Popen(command, stdout=log, stderr=log)
            try:
                if splash:
                    check_boot_screen(socket, serial, artifacts / "splash", timeout)
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
                if diskless and ("live0:" not in text or "ata0:" in text):
                    raise RuntimeError("the diskless desktop did not use the LiveCD ramdisk")
                if virtio:
                    for marker in ("virtio-gpu: ready", "virtio-input: tablet ready", "virtio-input: keyboard ready", "vblk0:"):
                        if marker not in text:
                            raise RuntimeError(f"virtio graphical backend missing: {marker}")
                def expired(_signum, _frame):
                    raise TimeoutError(f"{scenario} exceeded its {timeout:g}s deadline")
                previous_alarm = signal.signal(signal.SIGALRM, expired)
                signal.setitimer(signal.ITIMER_REAL, max(1, timeout))
                # Exercise the real reference UI and its built font tables.
                shots = shoot_session.run_scenario(
                    "notepadwheel" if scenario == "sxgui-smoke" else scenario,
                    socket, artifacts, boot_wait=5, abs_pointer=virtio, audio_path=audio_path)
                if scenario == "desktop":
                    from PIL import Image
                    with Image.open(shots[-1]) as screen:
                        image = screen.convert("RGB")
                    bx, by, bw, bh = shoot_session.button_rect(image, 0)
                    shoot_session.expect_text(image, (bx, by, bx + bw, by + bh),
                        "Program Manager", shoot_session.TEXT, shoot_session.FACE,
                        "the default session taskbar")
                if scenario in ("doom", "ccleste"):
                    from PIL import Image, ImageChops
                    # Both ports request the reference 640x400 client surface.
                    # Restrict content checks to that surface, excluding the
                    # wallpaper, window chrome and taskbar.
                    with Image.open(shots[0]) as source:
                        first = source.convert("RGB").crop((348, 234, 988, 634))
                    with Image.open(shots[1] if scenario == "doom" else shots[-1]) as source:
                        game = source.convert("RGB").crop((348, 234, 988, 634))
                    minimum = 64 if scenario == "doom" else 8
                    if len(game.getcolors(1 << 20) or []) < minimum or not ImageChops.difference(first, game).getbbox():
                        raise RuntimeError("the game did not draw a changing scene")
                    log_text = serial.read_text(errors="replace")
                    expected = "CCLESTE DISPLAY READY" if scenario == "ccleste" else "doomgeneric: musica sonando:"
                    if expected not in log_text:
                        raise RuntimeError("the installed game did not reach its runtime checkpoint")
                text = serial.read_text(errors="replace")
                if "exception vector" in text or "user: exception #" in text or "stack overflow" in text or "compositor startup failed" in text:
                    raise RuntimeError("a graphical process faulted; see serial.log")
                print(f"i386 {report_scenario}{' virtio' if virtio else ''}: PASS ({len(shots)} captures)", flush=True)
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
    parser.add_argument("--virtio", action="store_true")
    parser.add_argument("--smp", type=int, choices=range(1, 33), default=1)
    args = parser.parse_args()
    return run(args.iso.resolve(), args.disk_image.resolve(), args.artifacts.resolve(), args.scenario, args.accel, args.timeout, args.virtio, args.smp)


if __name__ == "__main__":
    raise SystemExit(main())
