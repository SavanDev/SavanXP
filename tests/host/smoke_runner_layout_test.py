#!/usr/bin/env python3
"""Static checks for the Linux smoke runner and initramfs handoff."""

from __future__ import annotations

import argparse
import ast
import subprocess
import tempfile
from pathlib import Path


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", required=True, type=Path)
    args = parser.parse_args()
    root = args.root.resolve()

    build = (root / "build.sh").read_text(encoding="utf-8")
    assert "|smoke|" in build
    assert "tools/run_smoke.py" in build
    assert "--sxfs-cli" in build
    assert "SAVANXP_SMOKE_COMMAND" in build
    cmake = (root / "CMakeLists.txt").read_text(encoding="utf-8")
    assert 'set(SAVANXP_SMOKE_COMMAND ""' in cmake
    assert "write_smoke_spec.py" in cmake
    assert (root / "tools" / "write_smoke_spec.py").is_file()
    assert (root / "tools" / "run_smoke.py").is_file()
    assert (root / "tools" / "qmp_client.py").is_file()
    assert (root / "tools" / "taskbar_smoke.py").is_file()
    assert (root / "tools" / "tcp_echo_server.py").is_file()
    assert (root / "tools" / "build-user.sh").is_file()
    assert (root / "tools" / "build-user.sh").stat().st_mode & 0o111
    assert (root / "tools" / "run-user.sh").is_file()
    assert (root / "tools" / "run-user.sh").stat().st_mode & 0o111
    assert (root / "tools" / "new-user-app.sh").is_file()
    assert (root / "tools" / "new-user-app.sh").stat().st_mode & 0o111
    assert (root / "tools" / "shoot.sh").is_file()
    assert (root / "tools" / "shoot.sh").stat().st_mode & 0o111
    assert (root / "tools" / "verify_doom_persistence.sh").is_file()
    assert (root / "tools" / "verify_doom_persistence.sh").stat().st_mode & 0o111
    assert not (root / "sdk" / "doomgeneric").exists()
    assert not (root / "sdk" / "ffmpeg").exists()
    assert (root / "tools" / "iso_boot_test.py").is_file()
    assert (root / "tools" / "iso_boot_test.py").stat().st_mode & 0o111
    shoot_script = (root / "tools" / "shoot.sh").read_text(encoding="utf-8")
    assert "shoot_session.py" in shoot_script
    assert "run_qemu.py" in shoot_script
    build_user = (root / "tools" / "build-user.sh").read_text(encoding="utf-8")
    assert "--destination" in build_user
    assert "DESTINATION_RELATIVE" in build_user
    assert 'SAVANXP_CLANG' in build_user
    assert 'SAVANXP_LD' in build_user
    assert '-I "$SOURCE_ROOT"' in build_user
    run_user = (root / "tools" / "run-user.sh").read_text(encoding="utf-8")
    assert "build-user.sh" in run_user
    assert "build.sh" in run_user
    assert (root / "tools" / "smoke_catalog.py").is_file()
    startup = (root / "boot" / "startup.nsh").read_text(encoding="utf-8")
    assert startup == "fs0:\\EFI\\BOOT\\BOOTX64.EFI\n"
    cmake = (root / "CMakeLists.txt").read_text(encoding="utf-8")
    assert "startup.nsh" in cmake
    build_script = (root / "build.sh").read_text(encoding="utf-8")
    assert "one-word smoke targets" in build_script
    assert "--gpu-soak-iterations" in build_script
    assert "between 1 and 32" in build_script
    runner = (root / "tools" / "run_smoke.py").read_text(encoding="utf-8")
    assert "make_smoke_copy" in runner
    assert "sxfs-cli" in runner
    assert "qmp_driver" in runner
    assert "remove_path" in runner
    run_qemu = (root / "tools" / "run_qemu.py").read_text(encoding="utf-8")
    assert "--qmp-path" in run_qemu
    assert "--serial-log" in run_qemu
    assert "SAVANXP_QEMU" in run_qemu
    assert "run_qmp_callback" in runner
    assert "qmp_callback" in runner
    catalog = (root / "tools" / "smoke_catalog.py").read_text(encoding="utf-8")
    assert '"smoke"' in catalog
    assert '"windowd-smoke"' in catalog
    assert '"native-hello"' in catalog
    assert '"sxfs-smoke"' in catalog
    assert '"kbd-smoke"' in catalog
    assert '"ac97-stream"' in catalog
    assert '"tcp-smoke"' in catalog
    assert '"mediaplayer-availability"' in catalog
    assert '"mediaplayer-selftest"' in catalog
    assert '"mediaplayer-display"' in catalog
    assert '"mediaplayer-missing"' in catalog
    assert 'remove_paths=("bin/mediaplayer-ffmpeg",)' in catalog
    qmp_display = (root / "ports" / "ffmpeg" / "tests" / "qmp_display.py").read_text(encoding="utf-8")
    assert "--socket" in qmp_display
    assert 'qmp_callback="./ports/ffmpeg/tests/qmp_display.py"' in catalog
    assert 'port_command="./ports/ffmpeg/smoke.sh"' in catalog
    assert '"gputest --soak 96"' in catalog
    assert "./tools/restore_doom.sh" in catalog
    assert "build-user.sh" in catalog
    assert "taskbar-smoke" in catalog

    for path in (
        root / "tools" / "run_qemu.py",
        root / "tools" / "run_smoke.py",
        root / "tools" / "qmp_client.py",
        root / "tools" / "taskbar_smoke.py",
        root / "tools" / "tcp_echo_server.py",
        root / "tools" / "smoke_catalog.py",
        root / "tools" / "write_smoke_spec.py",
        root / "tests" / "host" / "smoke_runner_layout_test.py",
    ):
        ast.parse(path.read_text(encoding="utf-8"), filename=str(path))
    subprocess.run(["bash", "-n", str(root / "tools" / "build-user.sh")], check=True)

    result = subprocess.run(
        ["python3", str(root / "tools" / "smoke_catalog.py"), "smoke", "--field", "success"],
        check=True,
        text=True,
        capture_output=True,
    )
    assert result.stdout.strip() == "SMOKE PASS"

    with tempfile.TemporaryDirectory() as directory:
        output = Path(directory) / "SMOKE"
        subprocess.run(
            [
                "python3",
                str(root / "tools" / "write_smoke_spec.py"),
                "--output",
                str(output),
                "--command",
                "smoke",
            ],
            check=True,
        )
        assert output.read_text(encoding="ascii") == "smoke\n"
        too_long = subprocess.run(
            [
                "python3",
                str(root / "tools" / "write_smoke_spec.py"),
                "--output",
                str(output),
                "--command",
                "x" * 64,
            ],
            capture_output=True,
        )
        assert too_long.returncode != 0
    print("smoke runner layout: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
