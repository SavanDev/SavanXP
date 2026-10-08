#!/usr/bin/env python3
"""Assert the legacy i386 machine exposes the expected 32-bit CPU.

Boots no guest: it starts qemu-system-i386 paused with a QMP socket, asks
for a full expansion of the configured model, and fails unless long mode
and NX are both absent -- the two properties the whole legacy branch rests
on. PAE/SSE2/APIC are reported for context, not asserted.

Usage:  python3 tools/check_legacy_cpu.py [--cpu qemu32] [--accel tcg]
"""

from __future__ import annotations

import argparse
import shutil
import subprocess
import tempfile
from pathlib import Path

from qmp_client import QmpClient, QmpError


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--cpu", default="qemu32")
    parser.add_argument("--accel", choices=("tcg", "kvm"), default="tcg")
    args = parser.parse_args()

    binary = shutil.which("qemu-system-i386")
    if not binary:
        print("LEGACY CPU FAIL: qemu-system-i386 was not found in PATH")
        return 1

    with tempfile.TemporaryDirectory(prefix="legacy-cpu-") as tmp:
        qmp_path = Path(tmp) / "qmp.sock"
        process = subprocess.Popen(
            [
                binary,
                "-machine",
                "pc",
                "-cpu",
                args.cpu,
                "-accel",
                args.accel,
                "-m",
                "1024M",
                "-display",
                "none",
                "-serial",
                "none",
                "-monitor",
                "none",
                "-qmp",
                f"unix:{qmp_path},server=on,wait=off",
                "-S",
            ],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        try:
            try:
                with QmpClient(qmp_path, timeout=10.0) as client:
                    response = client.execute(
                        "query-cpu-model-expansion",
                        {"type": "full", "model": {"name": args.cpu}},
                    )
            except QmpError as exc:
                print(f"LEGACY CPU FAIL: QMP query failed: {exc}")
                return 1
            props = response.get("return", {}).get("model", {}).get("props", {})
            lm = bool(props.get("lm", False))
            nx = bool(props.get("nx", False))
            context = (
                f"pae={bool(props.get('pae', False))} "
                f"sse2={bool(props.get('sse2', False))} "
                f"apic={bool(props.get('apic', False))}"
            )
            if lm or nx:
                print(f"LEGACY CPU FAIL: cpu={args.cpu} lm={lm} nx={nx} ({context})")
                return 1
            print(f"LEGACY CPU OK: cpu={args.cpu} lm=False nx=False ({context})")
            return 0
        finally:
            try:
                with QmpClient(qmp_path, timeout=2.0) as client:
                    client.execute("quit")
            except QmpError:
                process.terminate()
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()


if __name__ == "__main__":
    raise SystemExit(main())
