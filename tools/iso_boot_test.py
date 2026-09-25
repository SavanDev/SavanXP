#!/usr/bin/env python3
"""Check ISO El Torito metadata and boot the image through BIOS and UEFI."""

from __future__ import annotations

import argparse
import os
import shutil
import signal
import subprocess
import tempfile
import time
from pathlib import Path


def find_ovmf(name: str) -> Path | None:
    candidates = [
        os.environ.get(f"OVMF_{name.upper()}"),
        f"/usr/share/edk2/x64/OVMF_{name.upper()}.4m.fd",
        f"/usr/share/edk2-ovmf/x64/OVMF_{name.upper()}.fd",
        f"/usr/share/OVMF/OVMF_{name.upper()}.fd",
    ]
    for candidate in candidates:
        if candidate and Path(candidate).is_file():
            return Path(candidate)
    return None


def terminate(process: subprocess.Popen[bytes]) -> None:
    if process.poll() is not None:
        return
    try:
        os.killpg(process.pid, signal.SIGTERM)
    except ProcessLookupError:
        return
    try:
        process.wait(timeout=3)
    except subprocess.TimeoutExpired:
        try:
            os.killpg(process.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
        process.wait()


def run_boot(qemu: str, label: str, args: list[str], serial: Path, timeout: float) -> None:
    serial.unlink(missing_ok=True)
    process = subprocess.Popen(
        args,
        stdin=subprocess.DEVNULL,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
        start_new_session=True,
    )
    deadline = time.monotonic() + timeout
    try:
        while time.monotonic() < deadline:
            text = serial.read_text(encoding="utf-8", errors="replace") if serial.exists() else ""
            if "handoff: starting /bin/init" in text or "SMOKE PASS" in text:
                print(f"{label}: PASS")
                return
            if process.poll() is not None:
                raise RuntimeError(f"{label}: QEMU exited with {process.returncode}")
            time.sleep(0.2)
        raise RuntimeError(f"{label}: boot token not observed within {timeout:g}s")
    finally:
        terminate(process)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--iso", required=True, type=Path)
    parser.add_argument("--timeout", type=float, default=90.0)
    parser.add_argument("--accel", choices=("tcg", "kvm"), default="tcg")
    parser.add_argument("--layout-only", action="store_true")
    args = parser.parse_args()
    iso = args.iso.resolve()
    if not iso.is_file():
        raise SystemExit(f"ISO does not exist: {iso}")
    xorriso = shutil.which("xorriso")
    if not xorriso:
        raise SystemExit("xorriso was not found in PATH")
    report = subprocess.run(
        [xorriso, "-indev", str(iso), "-report_el_torito", "plain"],
        text=True,
        capture_output=True,
        check=False,
    )
    if report.returncode != 0 or "El Torito" not in report.stdout:
        raise SystemExit("ISO has no inspectable El Torito catalog")
    print("ISO El Torito catalog: PASS")
    if args.layout_only:
        return 0
    qemu_override = os.environ.get("SAVANXP_QEMU")
    qemu = shutil.which(qemu_override or "qemu-system-x86_64")
    if qemu_override and not qemu:
        candidate = Path(qemu_override)
        if candidate.is_file() and os.access(candidate, os.X_OK):
            qemu = str(candidate)
    if not qemu:
        raise SystemExit("qemu-system-x86_64 was not found in PATH or SAVANXP_QEMU")
    with tempfile.TemporaryDirectory(prefix="savanxp-iso-boot.") as temporary:
        root = Path(temporary)
        serial_bios = root / "bios.serial.log"
        serial_uefi = root / "uefi.serial.log"
        common = [
            qemu,
            "-machine",
            "q35",
            "-accel",
            args.accel,
            "-m",
            "256M",
            "-cpu",
            "host" if args.accel == "kvm" else "max",
            "-display",
            "none",
            "-boot",
            "d",
            "-cdrom",
            str(iso),
            "-serial",
            f"file:{serial_bios}",
            "-no-reboot",
            "-no-shutdown",
        ]
        run_boot(qemu, "BIOS", common, serial_bios, args.timeout)
        code = find_ovmf("CODE")
        variables = find_ovmf("VARS")
        if not code or not variables:
            raise SystemExit("OVMF_CODE/OVMF_VARS were not found")
        vars_copy = root / "OVMF_VARS.fd"
        shutil.copyfile(variables, vars_copy)
        uefi = [
            qemu,
            "-machine",
            "q35",
            "-accel",
            args.accel,
            "-m",
            "256M",
            "-cpu",
            "host" if args.accel == "kvm" else "max",
            "-display",
            "none",
            "-drive",
            f"if=pflash,format=raw,readonly=on,file={code}",
            "-drive",
            f"if=pflash,format=raw,file={vars_copy}",
            "-boot",
            "d",
            "-cdrom",
            str(iso),
            "-serial",
            f"file:{serial_uefi}",
            "-no-reboot",
            "-no-shutdown",
        ]
        run_boot(qemu, "UEFI", uefi, serial_uefi, args.timeout)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
