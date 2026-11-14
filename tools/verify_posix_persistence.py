#!/usr/bin/env python3
"""Verify guest POSIX writes survive a normal rebuild and a cold boot of a copy."""
from __future__ import annotations

import argparse
import fcntl
import hashlib
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile


def digest(path: Path) -> bytes:
    value = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            value.update(chunk)
    return value.digest()


def main() -> int:
    root = Path(__file__).resolve().parent.parent
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--iso", type=Path, default=root / "build/SavanXP.iso")
    parser.add_argument("--disk-image", type=Path, default=root / "build/disk.img")
    parser.add_argument("--jobs", type=int, default=4)
    parser.add_argument("--artifacts", type=Path)
    args = parser.parse_args()
    if not args.iso.is_file() or not args.disk_image.is_file():
        parser.error("build the ISO and disk first with ./build.sh iso")
    logs = root / "build/smoke-logs"
    logs.mkdir(parents=True, exist_ok=True)
    artifacts = args.artifacts or Path(tempfile.mkdtemp(prefix="posix-persistence.", dir=logs))
    artifacts.mkdir(parents=True, exist_ok=True)
    print(f"Persistence artifacts: {artifacts}", flush=True)
    with tempfile.TemporaryDirectory(prefix="savanxp-posix-persistence.") as temporary:
        output = Path(temporary)
        copy = output / "disk.img"
        with Path(str(args.disk_image) + ".lock").open("a+") as lock:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
            original = digest(args.disk_image)
            shutil.copyfile(args.disk_image, copy)
        for phase in ("write", "read"):
            command = [sys.executable, str(root / "tools/legacy_boot_test.py"),
                       "--iso", str(args.iso.resolve()), "--disk-image", str(copy),
                       "--expect-persistence", phase, "--artifacts", str(artifacts / phase)]
            if phase == "write":
                command += ["--writable-disk"]
            subprocess.run(command, cwd=root, check=True)
            if phase == "write":
                env = os.environ.copy()
                env.update(SAVANXP_OUTPUT_ROOT=str(output), SAVANXP_BUILD_DIR=str(output / "linux"),
                           SAVANXP_INCLUDE_TEST_APPS="ON", SAVANXP_PYTHON=sys.executable)
                # Use the same normal build path as the user's persistent image.
                with (artifacts / "rebuild.log").open("w") as log:
                    subprocess.run([str(root / "build.sh"), "build", "--jobs", str(args.jobs)],
                                   cwd=root, env=env, stdout=log, stderr=subprocess.STDOUT, check=True)
        if digest(args.disk_image) != original:
            raise RuntimeError("the source disk changed during the isolated check")
    print("POSIX guest writes, normal rebuild and cold-boot persistence: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
