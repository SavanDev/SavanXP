#!/usr/bin/env python3
"""Boot the i386 GRUB2 ISO on the legacy machine and wait for the serial token."""

from __future__ import annotations

import argparse
import os
import shutil
import signal
import subprocess
import tempfile
import time
from pathlib import Path

SUCCESS_TOKEN = "I386 BOOT OK"
FAILURE_TOKEN = "I386 BOOT FAIL"
PAGING_TOKEN = "paginacion: identidad 0..1 GiB"


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


def run_boot(qemu: str, args: list[str], serial: Path, timeout: float) -> None:
    """Boot the ISO and wait for the bootstrap token.

    The token is printed after paging is on, so it alone already implies the
    bootstrap ran to the end. The paging line is asserted too: a token that
    shows up before the page tables are enabled is exactly the failure a
    change to the entry path would cause.
    """
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
            if SUCCESS_TOKEN in text:
                if PAGING_TOKEN not in text:
                    raise RuntimeError(f"booted without enabling paging: {PAGING_TOKEN!r} missing")
                print("i386 bootstrap: PASS")
                return
            if FAILURE_TOKEN in text:
                lines = [line for line in text.splitlines() if line.startswith("savanxp/i386")]
                detail = lines[-1] if lines else "no reason reported"
                raise RuntimeError(f"bootstrap halted: {detail}")
            if process.poll() is not None:
                raise RuntimeError(f"QEMU exited with {process.returncode} before the bootstrap token")
            time.sleep(0.2)
        raise RuntimeError(f"bootstrap token not observed within {timeout:g}s")
    finally:
        terminate(process)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--iso", required=True, type=Path)
    parser.add_argument("--timeout", type=float, default=90.0)
    parser.add_argument("--accel", choices=("tcg", "kvm"), default="tcg")
    args = parser.parse_args()
    iso = args.iso.resolve()
    if not iso.is_file():
        raise SystemExit(f"ISO does not exist: {iso}")
    qemu_override = os.environ.get("SAVANXP_QEMU")
    qemu = shutil.which(qemu_override or "qemu-system-i386")
    if qemu_override and not qemu:
        candidate = Path(qemu_override)
        if candidate.is_file() and os.access(candidate, os.X_OK):
            qemu = str(candidate)
    if not qemu:
        raise SystemExit("qemu-system-i386 was not found in PATH or SAVANXP_QEMU")
    with tempfile.TemporaryDirectory(prefix="savanxp-i386-boot.") as temporary:
        serial = Path(temporary) / "serial.log"
        command = [
            qemu,
            "-machine",
            "pc",
            "-accel",
            args.accel,
            "-m",
            "1024M",
            "-cpu",
            "qemu32",
            "-display",
            "none",
            "-boot",
            "d",
            "-cdrom",
            str(iso),
            "-serial",
            f"file:{serial}",
            "-no-reboot",
            "-no-shutdown",
        ]
        run_boot(qemu, command, serial, args.timeout)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
