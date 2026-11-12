#!/usr/bin/env python3
"""Host test of the Win32 loader and gate: run the PE32 fixtures through
win32-harness and compare their output bytes and exit status.

The expected bytes and 32-bit exit statuses are the ones Windows XP produces
for the same files (docs/userland/WIN32.md, "Fixtures"): the harness prints
the full status on standard error and exits with its low byte. A failed load
must also name its cause; 124 means it left a mapping or a file behind.
"""

import argparse
import subprocess
import sys

CASES = [
    ("hello.exe", b"Hello from a PE32 executable.\r\n", 0x2A, None),
    (
        "initorder.exe",
        b"beta: attach, static load, pointers agree\r\n"
        b"alpha: attach, beta says 7, pointers agree\r\n"
        b"initorder: TLS callback 1, reason 1\r\n"
        b"initorder: TLS callback 2, reason 1\r\n"
        b"initorder: entry, thread value 7, alpha says 3, beta says 7\r\n"
        b"alpha: detach, process ending\r\n"
        b"beta: detach, process ending\r\n"
        b"initorder: TLS callback 1, reason 0\r\n"
        b"initorder: TLS callback 2, reason 0\r\n",
        0x7,
        None,
    ),
    (
        "missingdll.exe",
        b"",
        0xC0000135,
        "win32: missingdll.exe: missing DLL nosuch.dll",
    ),
    (
        "missingsym.exe",
        b"",
        0xC0000139,
        "win32: missingsym.exe: kernel32.dll!MissingFunction is not exported",
    ),
]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--harness", required=True)
    parser.add_argument("--system32", required=True)
    parser.add_argument("--fixtures", required=True)
    args = parser.parse_args()

    failures = 0
    for name, stdout, status, stderr_line in CASES:
        result = subprocess.run(
            [args.harness, "--dlls", args.system32, f"{args.fixtures}/{name}"],
            capture_output=True,
            timeout=30,
        )
        errors = result.stderr.decode(errors="replace").splitlines()
        problems = []
        if result.stdout != stdout:
            problems.append(f"stdout {result.stdout!r}, expected {stdout!r}")
        status_line = f"win32: exit status 0x{status:08x}"
        if result.returncode != status & 0xFF or status_line not in errors:
            problems.append(f"status {result.returncode} {errors!r}, expected {status_line}")
        if stderr_line is not None and stderr_line not in errors:
            problems.append(f"stderr {errors!r} lacks {stderr_line!r}")
        if problems:
            failures += 1
            print(f"FAIL {name}: " + "; ".join(problems))
        else:
            print(f"ok   {name}")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
