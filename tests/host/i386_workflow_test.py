#!/usr/bin/env python3
"""Exercise the default build/run routing without launching a guest."""
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", required=True, type=Path)
    root = parser.parse_args().root.resolve()
    with tempfile.TemporaryDirectory(prefix="savanxp-i386-workflow.") as temporary:
        temp = Path(temporary)
        tools = temp / "tools"
        tools.mkdir()
        recorder = tools / "recorder"
        recorder.write_text(
            f"#!{sys.executable}\nimport json, os, sys\n"
            "with open(os.environ['WORKFLOW_LOG'], 'a') as log:\n"
            "    log.write(json.dumps(sys.argv) + '\\n')\n"
        )
        recorder.chmod(0o755)
        (tools / "cmake").symlink_to(recorder)
        output = temp / "output"
        output.mkdir()
        (output / "SavanXP.iso").touch()
        disk = output / "disk.img"
        disk.write_bytes(b"persistent data")
        env = {key: value for key, value in os.environ.items() if not key.startswith("SAVANXP_")}
        env.update(PATH=f"{tools}:{env['PATH']}", WORKFLOW_LOG=str(temp / "calls"),
                   SAVANXP_OUTPUT_ROOT=str(output), SAVANXP_BUILD_DIR=str(temp / "cmake"),
                   SAVANXP_QEMU=str(recorder), SAVANXP_PYTHON=sys.executable)

        def run(*arguments: str, success: bool = True, environment=None):
            result = subprocess.run([str(root / "build.sh"), *arguments], env=environment or env,
                                    text=True, capture_output=True)
            assert (result.returncode == 0) == success, result.stdout + result.stderr
            return result

        for mode, virtio, cpus in (("run", False, 1), ("debug", False, 1), ("run", True, 1), ("run", True, 4)):
            (temp / "calls").unlink(missing_ok=True)
            run(mode, "--headless", "--smp", str(cpus), *(("--virtio",) if virtio else ()))
            calls = [json.loads(line) for line in (temp / "calls").read_text().splitlines()]
            assert "-DSAVANXP_ARCH=i386" in calls[0], calls
            assert any(call[-1] == "savanxp_disk" for call in calls), calls
            assert any(call[-1] == "savanxp_iso" for call in calls), calls
            launch = calls[-1]
            assert launch[launch.index("-cpu") + 1] == "qemu32", launch
            assert launch[launch.index("-machine") + 1] == "pc", launch
            assert launch[launch.index("-smp") + 1] == str(cpus), launch
            assert f"-DSAVANXP_SMP={cpus}" in calls[0], calls
            disk_interface = "if=none,id=svdisk" if virtio else "if=ide,index=0"
            assert any(str(disk) in argument and disk_interface in argument for argument in launch)
            assert any("SavanXP.iso" in argument and "media=cdrom" in argument for argument in launch)
            if virtio:
                assert "-DSAVANXP_VIRTIO=ON" in calls[0], calls
                assert "virtio-blk-pci,drive=svdisk,disable-legacy=on" in launch
                assert "virtio-net-pci,netdev=net0,disable-legacy=on" in launch
                assert "virtio-sound-pci,audiodev=audio0,streams=2" in launch
                assert "virtio-vga,xres=1280,yres=800" in launch
                assert "virtio-keyboard-pci" in launch and "virtio-tablet-pci" in launch
                assert not any("if=ide,index=0" in argument for argument in launch)
            else:
                assert "rtl8139,netdev=net0" in launch and "AC97,audiodev=audio0" in launch
            assert all("pflash" not in argument and "fat:rw:" not in argument for argument in launch)
            assert ("-S" in launch) == (mode == "debug"), launch
            assert disk.read_bytes() == b"persistent data"
        bad_arch = dict(env, SAVANXP_ARCH="x86_64")
        assert "master" in run("build", success=False, environment=bad_arch).stderr
        for invalid in ("0", "33", "invalid"):
            assert "between 1 and 32" in run("run", "--smp", invalid, success=False).stderr
        assert "not ported" in run("smoke", "unknown-smoke", success=False).stderr
        catalog = run("smoke", "--list").stdout
        assert "processes" in catalog and "sxgui-smoke" in catalog and "services" in catalog
    print("i386 default build/run workflow: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
