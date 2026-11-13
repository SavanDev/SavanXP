#!/usr/bin/env python3
"""Host test of the Win32 loader, gate and DLLs: run the PE32 fixtures
through win32-harness and compare their output bytes and exit status.

The expected bytes are the ones Windows XP produced for the same files,
kept in subsystems/win32/fixtures/expected/ (docs/userland/WIN32.md,
"Fixtures"). The harness prints the full 32-bit status on standard error and
exits with its low byte. A failed load must also name its cause; 124 means it
left a mapping or a file behind. startup.exe runs twice on one registry file:
the second run reads back what the first wrote. window.exe plays its input
script (fixtures/window/input.txt); the client area it dumps must match the
golden image, which equals Windows XP's outside the text box
(docs/userland/WIN32.md, "Fixtures").
"""

import argparse
import gzip
import pathlib
import subprocess
import sys
import tempfile

STARTUP_ARGUMENTS = ["alpha", "two words", "3"]

# (fixture, arguments, expected output file or None for no output, status,
#  a line standard error must hold, uses the registry, input script or None)
CASES = [
    ("hello.exe", [], "hello.out", 0x2A, None, False, None),
    ("initorder.exe", [], "initorder.out", 0x7, None, False, None),
    ("missingdll.exe", [], None, 0xC0000135, "win32: missingdll.exe: missing DLL nosuch.dll", False, None),
    (
        "missingsym.exe",
        [],
        None,
        0xC0000139,
        "win32: missingsym.exe: kernel32.dll!MissingFunction is not exported",
        False,
        None,
    ),
    ("startup.exe", STARTUP_ARGUMENTS, "startup-1.out", 0x5, None, True, None),
    ("startup.exe", STARTUP_ARGUMENTS, "startup-2.out", 0x5, None, True, None),
    ("window.exe", [], "window.out", 0x3, None, False, "window/input.txt"),
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
        for name, arguments, expected_file, status, stderr_line, uses_registry, script in CASES:
            command = [args.harness, "--dlls", args.system32]
            if uses_registry:
                command += ["--registry", registry]
            if script:
                command += ["--input", str(expected_dir.parent / script)]
            command += [f"{args.fixtures}/{name}"] + arguments
            result = subprocess.run(command, capture_output=True, timeout=30, cwd=scratch)
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
            if script:
                stem = pathlib.Path(name).stem
                dump = pathlib.Path(scratch) / f"{stem}.ppm"
                golden = gzip.decompress((expected_dir / f"{stem}.ppm.gz").read_bytes())
                if not dump.exists():
                    problems.append(f"{dump.name} was not written")
                elif dump.read_bytes() != golden:
                    kept = pathlib.Path.cwd() / f"win32-{stem}-actual.ppm"
                    kept.write_bytes(dump.read_bytes())
                    problems.append(f"{dump.name} differs from the golden image; it is kept as {kept}")
            label = f"{name} -> {expected_file}" if expected_file else name
            if problems:
                failures += 1
                print(f"FAIL {label}: " + "; ".join(problems))
            else:
                print(f"ok   {label}")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
