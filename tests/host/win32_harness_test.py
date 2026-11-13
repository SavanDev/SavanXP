#!/usr/bin/env python3
"""Host test of the Win32 loader, gate and DLLs: run the PE32 fixtures
through win32-harness and compare their output bytes and exit status.

The expected bytes are the ones Windows XP produced for the same files,
kept in subsystems/win32/fixtures/expected/ (docs/userland/WIN32.md,
"Fixtures"). The harness prints the full 32-bit status on standard error and
exits with its low byte. A failed load must also name its cause; 124 means it
left a mapping or a file behind. startup.exe runs twice on one registry file:
the second run reads back what the first wrote.
"""

import argparse
import pathlib
import subprocess
import sys
import tempfile

STARTUP_ARGUMENTS = ["alpha", "two words", "3"]

# (fixture, arguments, expected output file or None for no output, status,
#  a line standard error must hold, uses the registry)
CASES = [
    ("hello.exe", [], "hello.out", 0x2A, None, False),
    ("initorder.exe", [], "initorder.out", 0x7, None, False),
    ("missingdll.exe", [], None, 0xC0000135, "win32: missingdll.exe: missing DLL nosuch.dll", False),
    (
        "missingsym.exe",
        [],
        None,
        0xC0000139,
        "win32: missingsym.exe: kernel32.dll!MissingFunction is not exported",
        False,
    ),
    ("startup.exe", STARTUP_ARGUMENTS, "startup-1.out", 0x5, None, True),
    ("startup.exe", STARTUP_ARGUMENTS, "startup-2.out", 0x5, None, True),
]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--harness", required=True)
    parser.add_argument("--system32", required=True)
    parser.add_argument("--fixtures", required=True)
    parser.add_argument("--expected", required=True)
    args = parser.parse_args()
    expected_dir = pathlib.Path(args.expected)

    failures = 0
    with tempfile.TemporaryDirectory() as scratch:
        registry = str(pathlib.Path(scratch) / "hkcu.reg")
        for name, arguments, expected_file, status, stderr_line, uses_registry in CASES:
            command = [args.harness, "--dlls", args.system32]
            if uses_registry:
                command += ["--registry", registry]
            command += [f"{args.fixtures}/{name}"] + arguments
            result = subprocess.run(command, capture_output=True, timeout=30)
            stdout = (expected_dir / expected_file).read_bytes() if expected_file else b""
            errors = result.stderr.decode(errors="replace").splitlines()
            problems = []
            if result.stdout != stdout:
                problems.append(f"stdout {result.stdout!r}, expected {stdout!r}")
            status_line = f"win32: exit status 0x{status:08x}"
            if result.returncode != status & 0xFF or status_line not in errors:
                problems.append(f"status {result.returncode} {errors!r}, expected {status_line}")
            if stderr_line is not None and stderr_line not in errors:
                problems.append(f"stderr {errors!r} lacks {stderr_line!r}")
            label = f"{name} -> {expected_file}" if expected_file else name
            if problems:
                failures += 1
                print(f"FAIL {label}: " + "; ".join(problems))
            else:
                print(f"ok   {label}")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
