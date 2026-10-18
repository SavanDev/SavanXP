#!/usr/bin/env python3
"""Boot the i386 GRUB2 ISO on the legacy machine and wait for its serial tokens."""

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
SXFS_TOKEN = "SXFS ROOT OK"
USER_LOADED_TOKEN = "user: /disk/bin/sysinfo cargado"
USER_OUTPUT_TOKEN = "sxfs_files:"
USER_EXIT_TOKEN = "proceso salio con codigo 0"


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


def run_boot(
    qemu: str,
    args: list[str],
    serial: Path,
    timeout: float,
    require: tuple[str, ...] = (),
    detail: str = "",
) -> None:
    """Boot the ISO and wait for the bootstrap token.

    The token is printed after paging is on, so it alone already implies the
    bootstrap ran to the end. The paging line is asserted too: a token that
    shows up before the page tables are enabled is exactly the failure a
    change to the entry path would cause. With a disk attached, the SxFS
    mount line is required as well.

    The required tokens are part of the wait, not a check after it: with
    --expect-user the userland output and its exit line arrive *after*
    "I386 BOOT OK", so returning on the bootstrap token alone would report a
    program that is still running.
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
    text = ""
    try:
        while time.monotonic() < deadline:
            text = serial.read_text(encoding="utf-8", errors="replace") if serial.exists() else ""
            if FAILURE_TOKEN in text:
                lines = [line for line in text.splitlines() if line.startswith("savanxp/i386")]
                reported = lines[-1] if lines else "no reason reported"
                raise RuntimeError(f"bootstrap halted: {reported}")
            if SUCCESS_TOKEN in text:
                if PAGING_TOKEN not in text:
                    raise RuntimeError(f"booted without enabling paging: {PAGING_TOKEN!r} missing")
                if not [token for token in require if token not in text]:
                    print(f"i386 bootstrap: PASS{detail}")
                    return
            if process.poll() is not None:
                raise RuntimeError(f"QEMU exited with {process.returncode} before the bootstrap token")
            time.sleep(0.2)
        missing = [token for token in (SUCCESS_TOKEN, *require) if token not in text]
        suffix = f"; missing {missing[0]!r}" if missing else ""
        raise RuntimeError(f"bootstrap did not reach its tokens within {timeout:g}s{suffix}")
    finally:
        terminate(process)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--iso", required=True, type=Path)
    parser.add_argument(
        "--disk-image",
        type=Path,
        default=None,
        help="attach this image as the primary IDE disk and require the SxFS mount",
    )
    parser.add_argument(
        "--expect-user",
        action="store_true",
        help="require that the kernel loaded the userland program, that it printed and that it exited",
    )
    parser.add_argument("--timeout", type=float, default=90.0)
    parser.add_argument("--accel", choices=("tcg", "kvm"), default="tcg")
    args = parser.parse_args()
    iso = args.iso.resolve()
    if not iso.is_file():
        raise SystemExit(f"ISO does not exist: {iso}")
    disk_image = args.disk_image.resolve() if args.disk_image is not None else None
    if disk_image is not None and not disk_image.is_file():
        raise SystemExit(f"disk image does not exist: {disk_image}")
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
        if disk_image is not None:
            # snapshot=on: el arranque nunca escribe en la imagen persistente.
            command += ["-drive", f"file={disk_image},format=raw,if=ide,snapshot=on"]
        require = (SXFS_TOKEN,) if disk_image is not None else ()
        detail = " (SxFS root mounted)" if disk_image is not None else ""
        if args.expect_user:
            # El programa del MVP imprime su informe y sale; el kernel informa
            # de la salida. Las dos cosas son el anillo 3 entero: cargar la
            # imagen, entrar, llamar al sistema y volver por EXIT.
            require += (USER_LOADED_TOKEN, USER_OUTPUT_TOKEN, USER_EXIT_TOKEN)
            detail = " (userland program ran)"
        run_boot(qemu, command, serial, args.timeout, require, detail)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
