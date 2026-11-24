#!/usr/bin/env python3
"""tools/build_log_summary.py: what quiet mode shows for each kind of step."""

from __future__ import annotations

import argparse
import subprocess
import sys
import tempfile
from pathlib import Path


def summary(tool: Path, kind: str, status: int, log_text: str, root: str = "/src") -> list[str]:
    with tempfile.TemporaryDirectory() as tmp:
        log = Path(tmp) / "step.log"
        log.write_text(log_text, encoding="utf-8")
        result = subprocess.run(
            [sys.executable, str(tool), "--kind", kind, "--label", "step", "--status", str(status),
             "--seconds", "7", "--root", root, str(log)],
            capture_output=True, text=True, check=True)
    return result.stdout.splitlines()


NINJA_OK = """\
[1/3] Building C object a.o
/src/kernel/net.cpp:10:5: warning: unused variable 'x' [-Wunused-variable]
[2/3] Building C object b.o
/src/kernel/net.cpp:10:5: warning: unused variable 'x' [-Wunused-variable]
1 warning generated.
[3/3] Linking kernel
"""

NINJA_FAILED = """\
[1/3] Building C object a.o
FAILED: CMakeFiles/k.dir/kernel/foo.cpp.o
/usr/bin/clang++ -c /src/kernel/foo.cpp
/src/kernel/foo.cpp:3:6: error: expected ';' after top level declarator
1 error generated.
[2/3] Building C object b.o
ninja: build stopped: subcommand failed.
"""

CTEST_SKIPPED = """\
1/3 Test #1: gfx2d-test .......................   Passed    0.01 sec
2/3 Test #2: win32-sol-test ...................***Skipped   0.00 sec
3/3 Test #3: sxfs-volume-test .................   Passed    0.20 sec

100% tests passed, 0 tests failed out of 3

Total Test time (real) =   0.22 sec

The following tests did not run:
\t  2 - win32-sol-test (Skipped)
"""

CTEST_FAILED = """\
1/2 Test #1: gfx2d-test .......................   Passed    0.01 sec
2/2 Test #2: sxfs-volume-test .................***Failed    0.20 sec
Output:
----------------------------------------------------------
sxfs_volume_test.cpp:88: expected 4096, got 0
<end of output>

50% tests passed, 1 tests failed out of 2

The following tests FAILED:
\t  2 - sxfs-volume-test (Failed)
Errors while running CTest
"""

CONFIGURE_FAILED = """\
-- SavanXP output root: /src/build
CMake Error at CMakeLists.txt:12 (message):
  clang was not found
-- Configuring incomplete, errors occurred!
"""


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", required=True, type=Path)
    tool = parser.parse_args().root.resolve() / "tools" / "build_log_summary.py"

    out = summary(tool, "ninja", 0, NINJA_OK)
    assert out[0].startswith("==> step: OK, 3 steps, 1 warning, 7s"), out
    # One warning, once, with the source root dropped; no step lines.
    assert out[1:] == ["kernel/net.cpp:10:5: warning: unused variable 'x' [-Wunused-variable]"], out

    out = summary(tool, "ninja", 0, "ninja: no work to do.\n")
    assert out[0].startswith("==> step: OK, no work to do, 0 warnings"), out

    out = summary(tool, "ninja", 1, NINJA_FAILED)
    assert out[0].startswith("==> step: FAILED (exit 1)"), out
    assert "kernel/foo.cpp:3:6: error: expected ';' after top level declarator" in out, out
    assert out[-1] == "ninja: build stopped: subcommand failed.", out
    assert not any(line.startswith("[") for line in out), out

    out = summary(tool, "ctest", 0, CTEST_SKIPPED)
    assert out[0].startswith("==> step: OK, 100% tests passed, 0 tests failed out of 3"), out
    # A skipped test is always reported, even when the run passes.
    assert "The following tests did not run:" in out, out
    assert "\t  2 - win32-sol-test (Skipped)" in out, out
    assert not any("Passed" in line for line in out), out

    out = summary(tool, "ctest", 8, CTEST_FAILED)
    assert out[0].startswith("==> step: FAILED (exit 8), 50% tests passed"), out
    assert "\t  2 - sxfs-volume-test (Failed)" in out, out
    assert "  sxfs_volume_test.cpp:88: expected 4096, got 0" in out, out
    assert not any("<end of output>" in line or "gfx2d-test" in line for line in out), out

    out = summary(tool, "configure", 1, CONFIGURE_FAILED)
    assert out[0].startswith("==> step: FAILED (exit 1)"), out
    assert out[1] == "CMake Error at CMakeLists.txt:12 (message):", out
    assert "  clang was not found" in out, out

    print("build-log-summary-test: OK")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
