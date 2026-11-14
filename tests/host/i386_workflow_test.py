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

        for mode in ("run", "debug"):
            (temp / "calls").unlink(missing_ok=True)
            run(mode, "--headless")
            calls = [json.loads(line) for line in (temp / "calls").read_text().splitlines()]
            assert "-DSAVANXP_ARCH=i386" in calls[0], calls
            assert any(call[-1] == "savanxp_disk" for call in calls), calls
            assert any(call[-1] == "savanxp_iso" for call in calls), calls
            launch = calls[-1]
            assert launch[launch.index("-cpu") + 1] == "qemu32", launch
            assert launch[launch.index("-machine") + 1] == "pc", launch
            assert any(str(disk) in argument and "if=ide,index=0" in argument for argument in launch)
            assert any("SavanXP.iso" in argument and "media=cdrom" in argument for argument in launch)
            assert all("pflash" not in argument and "fat:rw:" not in argument for argument in launch)
            assert ("-S" in launch) == (mode == "debug"), launch
            assert disk.read_bytes() == b"persistent data"
        bad_arch = dict(env, SAVANXP_ARCH="x86_64")
        assert "master" in run("build", success=False, environment=bad_arch).stderr
        assert "not ported" in run("run", "--virtio", success=False).stderr
        assert "one CPU" in run("run", "--smp", "2", success=False).stderr
        assert "not ported" in run("smoke", "sxgui-smoke", success=False).stderr
        assert "processes" in run("smoke", "--list").stdout
    print("i386 default build/run workflow: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
