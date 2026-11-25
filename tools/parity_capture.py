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
    parser.add_argument("--scenario", required=True)
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
