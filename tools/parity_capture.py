#!/usr/bin/env python3
"""Run one visual scenario on the v0.4.0 reference and on i386, then compare.

Both guests boot from BIOS ISOs on the same `qemu-system-x86_64`, machine,
memory and devices (docs/I386_PORT_STUDY.md, section 25). Each guest writes
into a snapshot overlay, so its base disk is never modified. The shared
`shoot_session` scenario drives both sessions with the same input, reading
the font tables from each build's own `libsxgfx.so.0.4`.

Captures with the same name are compared pixel by pixel. The edition label
that the wallpaper draws in the lower-right corner is the documented,
deliberate difference ("Alpha" versus "Legacy Edition"); `--mask-edition`
excludes that rectangle. Any other difference is reported with its bounding
box and a diff image, and makes the command fail unless `--allow-diff`.
"""
from __future__ import annotations

import argparse
import json
import os
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path

TOOLS = Path(__file__).resolve().parent
sys.path.insert(0, str(TOOLS))
from run_qemu import build_qemu_args  # noqa: E402

HANDOFF = "handoff: starting /bin/init"
# The wallpaper's version block sits above the taskbar at the right edge
# (desktop_wallpaper.c). Generous on purpose: it only has to contain the label.
EDITION_BOX = (1080, 690, 1280, 772)


def boot_and_run(version, iso, disk, root, scenario, out, profile, boot_wait, cpu, smp):
    out.mkdir(parents=True, exist_ok=True)
    for stale in out.glob("*.png"):
        stale.unlink()
    serial = out / "serial.log"
    if serial.exists():
        serial.unlink()
    # A Unix socket path must stay below 108 bytes, whatever the output path is.
    socket_dir = Path(tempfile.mkdtemp(prefix="sx-parity."))
    qmp = socket_dir / "qmp"
    virtio = profile == "virtio"
    args = build_qemu_args(
        iso=iso, disk_image=disk, debug_log=out / "debugcon.log", accel="tcg",
        smp=smp, headless=True, virtio=virtio, snapshot=True,
        serial_path=serial, qmp_path=qmp, cpu=cpu,
    )
    args[0] = "qemu-system-x86_64"
    qemu = subprocess.Popen(args, stdout=subprocess.DEVNULL, stderr=(out / "qemu-err.log").open("w"))
    try:
        deadline = time.time() + 240
        while time.time() < deadline:
            if serial.exists() and HANDOFF in serial.read_text(errors="replace"):
                break
            if qemu.poll() is not None:
                raise RuntimeError(f"{version}: QEMU exited before the init handoff")
            time.sleep(0.5)
        else:
            raise RuntimeError(f"{version}: no '{HANDOFF}' within 240 s")
        # A separate interpreter per guest: shoot_session caches the font tables
        # of the first build it reads.
        env = dict(os.environ, SAVANXP_OUTPUT_ROOT=str(root))
        code = (
            "import sys; sys.path.insert(0, %r)\n"
            "import shoot_session as s\n"
            "try:\n"
            "    s.run_scenario(%r, %r, %r, boot_wait=%r, abs_pointer=%r)\n"
            "except s.Failure as f:\n"
            "    print('FAILURE:', f); sys.exit(1)\n"
        ) % (str(TOOLS), scenario, str(qmp), str(out), boot_wait, virtio)
        result = subprocess.run([sys.executable, "-c", code], env=env)
        return result.returncode
    finally:
        qemu.terminate()
        try:
            qemu.wait(10)
        except subprocess.TimeoutExpired:
            qemu.kill()
        shutil.rmtree(socket_dir, ignore_errors=True)


BANNER = "SavanXP v0.4.0 booting"


def measure_boot(version, iso, disk, out, profile, cpu, smp, runs):
    """Host-side boot milestones in seconds from QEMU start.

    `serial` is the first serial byte, before which only firmware and the
    loader run; `banner` is the kernel's version line; `handoff` precedes
    init; `desktop` is the first screendump whose first taskbar button is
    sunken, the focused Program Manager.
    """
    sys.path.insert(0, str(TOOLS))
    from PIL import Image
    import shoot_session
    results = []
    for run in range(runs):
        target = out / f"boot-{run}"
        target.mkdir(parents=True, exist_ok=True)
        serial = target / "serial.log"
        if serial.exists():
            serial.unlink()
        socket_dir = Path(tempfile.mkdtemp(prefix="sx-parity."))
        args = build_qemu_args(
            iso=iso, disk_image=disk, debug_log=target / "debugcon.log", accel="tcg",
            smp=smp, headless=True, virtio=profile == "virtio", snapshot=True,
            serial_path=serial, qmp_path=socket_dir / "qmp", cpu=cpu)
        args[0] = "qemu-system-x86_64"
        start = time.monotonic()
        qemu = subprocess.Popen(args, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        marks = {}
        try:
            qmp = shoot_session.Qmp(socket_dir / "qmp")
            frame = socket_dir / "frame.ppm"
            while time.monotonic() - start < 240 and "desktop" not in marks:
                text = serial.read_text(errors="replace") if serial.exists() else ""
                now = time.monotonic() - start
                if text and "serial" not in marks:
                    marks["serial"] = now
                for key, token in (("banner", BANNER), ("handoff", HANDOFF)):
                    if key not in marks and token in text:
                        marks[key] = now
                if "handoff" in marks:
                    qmp.cmd("screendump", filename=str(frame))
                    try:
                        image = Image.open(frame).convert("RGB")
                        shoot_session.expect_taskbar_present(image, "boot")
                        shoot_session.expect_button(image, 0, True, "boot")
                        marks["desktop"] = time.monotonic() - start
                    except (shoot_session.Failure, OSError):
                        pass
                time.sleep(0.1)
        finally:
            qemu.terminate()
            try:
                qemu.wait(10)
            except subprocess.TimeoutExpired:
                qemu.kill()
            shutil.rmtree(socket_dir, ignore_errors=True)
        entry = {"version": version, "run": run, **{k: round(v, 2) for k, v in marks.items()}}
        print(json.dumps(entry), flush=True)
        results.append(entry)
    return results


def compare(ref_dir, i386_dir, diff_dir, mask_edition):
    from PIL import Image, ImageChops
    diff_dir.mkdir(parents=True, exist_ok=True)
    report = []
    ref_shots = {p.name: p for p in sorted(ref_dir.glob("*.png"))}
    i386_shots = {p.name: p for p in sorted(i386_dir.glob("*.png"))}
    for name in sorted(set(ref_shots) | set(i386_shots)):
        if name not in ref_shots or name not in i386_shots:
            report.append({"shot": name, "status": "missing",
                           "reference": name in ref_shots, "i386": name in i386_shots})
            continue
        a = Image.open(ref_shots[name]).convert("RGB")
        b = Image.open(i386_shots[name]).convert("RGB")
        if a.size != b.size:
            report.append({"shot": name, "status": "size", "reference": a.size, "i386": b.size})
            continue
        delta = ImageChops.difference(a, b).convert("L").point(lambda v: 255 if v else 0)
        if mask_edition:
            delta.paste(0, EDITION_BOX)
        bbox = delta.getbbox()
        count = delta.histogram()[255]
        entry = {"shot": name, "status": "identical" if not bbox else "different",
                 "pixels": count, "bbox": bbox}
        if bbox:
            Image.composite(Image.new("RGB", a.size, (255, 0, 255)), b, delta).save(diff_dir / name)
        report.append(entry)
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--scenario")
    parser.add_argument("--measure-boot", type=int, metavar="RUNS",
                        help="time the boot milestones RUNS times per guest instead of a scenario")
    parser.add_argument("--reference-root", type=Path, required=True,
                        help="the reference tree's build/ (SavanXP-parity.iso, disk.img, diskfs)")
    parser.add_argument("--i386-root", type=Path, default=TOOLS.parent / "build")
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--profile", choices=("pc", "virtio"), default="pc")
    parser.add_argument("--cpu", default=None, help="QEMU CPU model (default: qemu64, or max with virtio)")
    parser.add_argument("--smp", type=int, default=1)
    parser.add_argument("--boot-wait", type=float, default=45.0)
    parser.add_argument("--mask-edition", action="store_true",
                        help="ignore the wallpaper edition label, the documented difference")
    parser.add_argument("--allow-diff", action="store_true")
    parser.add_argument("--only", choices=("reference", "i386"))
    opts = parser.parse_args()
    cpu = opts.cpu or ("max" if opts.profile == "virtio" else "qemu64")
    guests = {
        "reference": (opts.reference_root / "SavanXP-parity.iso", opts.reference_root),
        "i386": (opts.i386_root / "SavanXP.iso", opts.i386_root),
    }
    if opts.measure_boot:
        report = []
        for version, (iso, root) in guests.items():
            if not opts.only or opts.only == version:
                report += measure_boot(version, iso.resolve(), (root / "disk.img").resolve(),
                                       opts.out / version, opts.profile, cpu, opts.smp, opts.measure_boot)
        opts.out.mkdir(parents=True, exist_ok=True)
        (opts.out / "boot.json").write_text(json.dumps(report, indent=2))
        return 0
    if not opts.scenario:
        parser.error("--scenario or --measure-boot is required")
    status = 0
    for version, (iso, root) in guests.items():
        if opts.only and opts.only != version:
            continue
        print(f"== {version}: {opts.scenario} ({opts.profile}, {cpu}, {opts.smp} vCPU)", flush=True)
        code = boot_and_run(version, iso.resolve(), (root / "disk.img").resolve(), root.resolve(),
                            opts.scenario, opts.out / version, opts.profile, opts.boot_wait, cpu, opts.smp)
        print(f"== {version}: scenario exit {code}", flush=True)
        status |= code
    if opts.only:
        return status
    report = compare(opts.out / "reference", opts.out / "i386", opts.out / "diff", opts.mask_edition)
    (opts.out / "report.json").write_text(json.dumps(report, indent=2))
    for entry in report:
        print(json.dumps(entry))
    if any(e["status"] != "identical" for e in report) and not opts.allow_diff:
        status |= 2
    return status


if __name__ == "__main__":
    raise SystemExit(main())
