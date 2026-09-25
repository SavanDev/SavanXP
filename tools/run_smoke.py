#!/usr/bin/env python3
"""Run one staged SavanXP smoke scenario and wait for its serial token."""

from __future__ import annotations

import argparse
import fcntl
import os
import shutil
import signal
import socket
import subprocess
import sys
import threading
import time
from pathlib import Path

from qmp_client import QmpClient, QmpError, send_kbd_smoke_actions
from taskbar_smoke import run_taskbar_actions
from run_qemu import build_qemu_args, find_qemu, prepare_image


def request_hmp_quit(path: Path) -> bool:
    try:
        with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as monitor:
            monitor.settimeout(0.5)
            monitor.connect(str(path))
            monitor.sendall(b"quit\n")
        return True
    except (FileNotFoundError, ConnectionRefusedError, OSError):
        return False


def stop_process(process: subprocess.Popen[bytes], monitor_path: Path) -> None:
    if process.poll() is not None:
        return
    if request_hmp_quit(monitor_path):
        try:
            process.wait(timeout=2)
            return
        except subprocess.TimeoutExpired:
            pass
    try:
        os.killpg(process.pid, signal.SIGTERM)
    except ProcessLookupError:
        return
    try:
        process.wait(timeout=2)
    except subprocess.TimeoutExpired:
        try:
            os.killpg(process.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
        process.wait()


def tail(path: Path, limit: int = 120) -> str:
    try:
        data = path.read_text(encoding="utf-8", errors="replace").splitlines()
    except OSError:
        return ""
    return "\n".join(data[-limit:])


def make_smoke_copy(source: Path, destination: Path, sxfs_cli: Path) -> None:
    check = subprocess.run(
        [str(sxfs_cli), "check", str(source)],
        text=True,
        capture_output=True,
        check=False,
    )
    if check.returncode != 0:
        detail = (check.stderr or check.stdout).strip()
        raise SystemExit(f"smoke: persistent image is not valid: {detail}")
    destination.unlink(missing_ok=True)
    try:
        shutil.copy2(source, destination)
        with destination.open("rb") as handle:
            os.fsync(handle.fileno())
        copied = subprocess.run(
            [str(sxfs_cli), "check", str(destination)],
            text=True,
            capture_output=True,
            check=False,
        )
        if copied.returncode != 0:
            detail = (copied.stderr or copied.stdout).strip()
            raise RuntimeError(f"smoke: copied image is not valid: {detail}")
    except (OSError, RuntimeError):
        destination.unlink(missing_ok=True)
        raise


def run_qmp_callback(callback: Path, qmp_path: Path, log_dir: Path, run_tag: str) -> None:
    """Run a port-owned one-shot QMP action at the guest ready token."""
    output_dir = log_dir / f"{run_tag}-callback"
    command = [
        sys.executable,
        str(callback),
        "--socket",
        str(qmp_path),
        "--out-dir",
        str(output_dir),
    ]
    try:
        result = subprocess.run(
            command,
            text=True,
            capture_output=True,
            check=False,
            timeout=8.0,
        )
    except subprocess.TimeoutExpired as exc:
        raise QmpError(f"QMP callback timed out: {callback}") from exc
    if result.stdout:
        print(result.stdout, end="", flush=True)
    if result.stderr:
        print(result.stderr, end="", file=sys.stderr, flush=True)
    if result.returncode != 0:
        raise QmpError(f"QMP callback failed with status {result.returncode}: {callback}")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--scenario", required=True)
    parser.add_argument("--command", required=True)
    parser.add_argument("--ready-token", default="")
    parser.add_argument("--qmp-driver", default="")
    parser.add_argument("--completion", choices=("serial", "host"), default="serial")
    parser.add_argument("--ready-wait", type=float, default=0.0)
    parser.add_argument("--audio-device", choices=("auto", "ac97", "virtio"), default="auto")
    parser.add_argument("--wav-path", type=Path)
    parser.add_argument(
        "--qmp-callback",
        type=Path,
        help="Python callback invoked once when the ready token appears",
    )
    parser.add_argument(
        "--remove-path",
        action="append",
        default=[],
        help="Remove this SxFS-root-relative path from the disposable image copy",
    )
    parser.add_argument("--image-root", required=True, type=Path)
    parser.add_argument("--disk-image", required=True, type=Path)
    parser.add_argument("--sxfs-cli", required=True, type=Path)
    parser.add_argument("--success-token", required=True)
    parser.add_argument("--failure-token", required=True)
    parser.add_argument("--timeout", type=float, default=120.0)
    parser.add_argument("--ovmf-code", type=Path)
    parser.add_argument("--ovmf-vars", type=Path)
    parser.add_argument("--accel", choices=("tcg", "kvm"), default="tcg")
    parser.add_argument("--smp", type=int, default=1)
    parser.add_argument("--virtio", action="store_true")
    parser.add_argument("--log-dir", type=Path)
    args = parser.parse_args()
    if not 1 <= args.smp <= 32:
        parser.error("--smp must be between 1 and 32")
    if args.qmp_driver not in ("", "kbd", "taskbar"):
        parser.error(f"unsupported QMP driver: {args.qmp_driver}")
    if args.completion == "host" and args.qmp_driver != "taskbar":
        parser.error("host completion requires the taskbar QMP driver")

    qemu = find_qemu()
    if not qemu:
        raise SystemExit("qemu-system-x86_64 was not found in PATH or SAVANXP_QEMU")
    image_root = args.image_root.resolve()
    source_disk = args.disk_image.resolve()
    # Smoke guests are allowed to create and remove files. Never let their
    # writes reach the persistent image; use a validated disposable sibling.
    log_dir = (args.log_dir or (image_root.parent / "smoke-logs")).resolve()
    log_dir.mkdir(parents=True, exist_ok=True)
    run_tag = f"{args.scenario}.{os.getpid()}"
    run_disk = log_dir / f"{run_tag}.disk.img"
    source_lock_path = source_disk.with_name(source_disk.name + ".lock")
    with source_lock_path.open("a+") as source_lock:
        fcntl.flock(source_lock.fileno(), fcntl.LOCK_EX)
        make_smoke_copy(source_disk, run_disk, args.sxfs_cli.resolve())
        for remove_path in args.remove_path:
            removed = subprocess.run(
                [str(args.sxfs_cli.resolve()), "rm", str(run_disk), remove_path],
                text=True,
                capture_output=True,
                check=False,
            )
            if removed.returncode != 0:
                detail = (removed.stderr or removed.stdout).strip()
                raise SystemExit(
                    f"smoke: could not remove {remove_path!r} from disposable image: {detail}"
                )

    vars_copy = log_dir / f"{run_tag}.OVMF_VARS.fd"
    debug_log = log_dir / f"{run_tag}.debugcon.log"
    monitor_path = log_dir / f"{run_tag}.hmp"
    qmp_path = log_dir / f"{run_tag}.qmp"
    monitor_path.unlink(missing_ok=True)
    qmp_path.unlink(missing_ok=True)
    log_path = log_dir / f"{run_tag}.serial.log"
    log_path.unlink(missing_ok=True)
    wav_path = args.wav_path.resolve() if args.wav_path is not None else None
    if wav_path is not None:
        wav_path.parent.mkdir(parents=True, exist_ok=True)
        wav_path.unlink(missing_ok=True)
    code, vars_copy = prepare_image(
        image_root=image_root,
        disk_image=run_disk,
        ovmf_code=args.ovmf_code,
        ovmf_vars=args.ovmf_vars,
        vars_copy=vars_copy,
        debug_log=debug_log,
    )
    qemu_args = build_qemu_args(
        image_root=image_root,
        disk_image=run_disk,
        code=code,
        vars_copy=vars_copy,
        debug_log=debug_log,
        accel=args.accel,
        smp=args.smp,
        virtio=args.virtio,
        headless=True,
        debug=False,
        monitor_path=monitor_path,
        serial_path=log_path,
        qmp_path=qmp_path if (args.qmp_driver or args.qmp_callback) else None,
        audio_device=args.audio_device,
        wav_path=wav_path,
    )
    qemu_args[0] = qemu

    command_path = log_dir / f"{run_tag}.qemu.log"
    command_path.write_text(
        "scenario=" + args.scenario + "\ncommand=" + args.command + "\n\n" + " ".join(qemu_args) + "\n",
        encoding="utf-8",
    )
    print(f"smoke: {args.scenario}: waiting for {args.success_token!r}", flush=True)
    print("qemu:", " ".join(qemu_args), flush=True)

    lock_path = run_disk.with_name(run_disk.name + ".lock")
    status = 1
    host_done = threading.Event()
    host_result: list[tuple[int, str]] = []
    host_thread: threading.Thread | None = None
    with lock_path.open("a+") as lock:
        fcntl.flock(lock.fileno(), fcntl.LOCK_EX)
        process = subprocess.Popen(
            qemu_args,
            stdin=subprocess.DEVNULL,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            start_new_session=True,
        )
        output = bytearray()
        offset = 0
        deadline = time.monotonic() + args.timeout
        settle_until: float | None = None
        qmp_sent = False
        qmp_error = False
        reason = "QEMU exited before the success token"

        def run_host_completion() -> None:
            try:
                paths = run_taskbar_actions(
                    qmp_path,
                    log_dir / f"{run_tag}-screenshots",
                    args.ready_wait,
                )
                host_result.append((0, f"host taskbar assertions passed ({len(paths)} screenshots)"))
            except Exception as exc:  # host driver errors are smoke failures
                host_result.append((1, f"host taskbar driver failed: {exc}"))
            finally:
                host_done.set()

        try:
            while True:
                if log_path.exists():
                    with log_path.open("rb") as serial_log:
                        serial_log.seek(offset)
                        chunk = serial_log.read()
                    if chunk:
                        offset += len(chunk)
                        output.extend(chunk)
                        text = output.decode("utf-8", errors="replace")
                        if not qmp_sent and args.ready_token and args.ready_token in text:
                            if args.completion == "host":
                                host_thread = threading.Thread(
                                    target=run_host_completion,
                                    name=f"{args.scenario}-host",
                                    daemon=True,
                                )
                                host_thread.start()
                                qmp_sent = True
                            else:
                                try:
                                    if args.qmp_callback:
                                        run_qmp_callback(
                                            args.qmp_callback.resolve(), qmp_path, log_dir, run_tag
                                        )
                                    else:
                                        with QmpClient(qmp_path) as client:
                                            if args.qmp_driver == "kbd":
                                                send_kbd_smoke_actions(client)
                                            else:
                                                raise QmpError(
                                                    f"unsupported QMP driver: {args.qmp_driver}"
                                                )
                                    qmp_sent = True
                                except (OSError, QmpError) as exc:
                                    qmp_error = True
                                    status = 1
                                    reason = f"QMP driver failed: {exc}"
                        if args.completion == "host":
                            if host_done.is_set() and host_result:
                                status, host_reason = host_result[0]
                                reason = host_reason
                                if status == 0:
                                    print("TASKBAR SMOKE PASS", flush=True)
                                    if settle_until is None:
                                        settle_until = time.monotonic() + 2.0
                        elif status == 0 or qmp_error:
                            pass
                        elif args.failure_token in text:
                            status = 1
                            reason = f"failure token {args.failure_token!r} observed"
                        elif args.success_token in text:
                            status = 0
                            reason = f"success token {args.success_token!r} observed"
                            settle_until = time.monotonic() + 2.0

                if args.completion == "host" and host_done.is_set() and host_result:
                    status, host_reason = host_result[0]
                    reason = host_reason
                    if status == 0 and settle_until is None:
                        print("TASKBAR SMOKE PASS", flush=True)
                        settle_until = time.monotonic() + 2.0

                now = time.monotonic()
                if now >= deadline:
                    if status != 0:
                        reason = f"timed out after {args.timeout:g}s"
                    break
                if settle_until is not None and now >= settle_until:
                    break
                if status == 1 and reason != "QEMU exited before the success token":
                    break
                if process.poll() is not None:
                    if status != 0:
                        reason = f"QEMU exited with status {process.returncode} before the success token"
                    break
                time.sleep(0.1)
        finally:
            stop_process(process, monitor_path)
            if host_thread is not None and host_thread.is_alive():
                host_thread.join(timeout=2.0)

    run_disk.unlink(missing_ok=True)
    lock_path.unlink(missing_ok=True)
    if status == 0 and wav_path is not None:
        if not wav_path.is_file() or wav_path.stat().st_size == 0:
            status = 1
            reason = f"WAV output missing or empty: {wav_path}"
    print(f"smoke: {args.scenario}: {reason}", flush=True)
    excerpt = tail(log_path)
    if excerpt:
        print("--- serial tail ---", flush=True)
        print(excerpt, flush=True)
    print(f"smoke: logs: {log_path}", flush=True)
    return status


if __name__ == "__main__":
    raise SystemExit(main())
