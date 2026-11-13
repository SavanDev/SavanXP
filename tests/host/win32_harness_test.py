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
(docs/userland/WIN32.md, "Fixtures"). dialog.exe plays its own, and dumps
its window with a menu open and its Settings dialog.

With --cards FOLDER it runs only cards.exe, with the user's own cards.dll
from FOLDER copied next to it in a temporary folder; without the file it
exits 77, skipped.

With --solitaire FOLDER it runs the user's own sol.exe and cards.dll from
FOLDER twice on one fresh registry, with the clock fixed so the first deal
is always the same, playing fixtures/solitaire/input-1.txt and input-2.txt
(docs/userland/WIN32.md, "Solitaire"). The registry after the first run must
equal expected/solitaire.reg, and each image the scripts dump must have the
SHA-256 expected/solitaire.sha256 lists: the images show Microsoft's card
art, so only their hashes are kept. Without the files it exits 77.
"""

import argparse
import gzip
import pathlib
import shutil
import subprocess
import sys
import tempfile

import hashlib

STARTUP_ARGUMENTS = ["alpha", "two words", "3"]

# The clock sol.exe seeds its deal from (time() & 0x7fff), 2001-08-24 02:56 UTC.
SOLITAIRE_TIME = "998621760"

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
    ("resource.exe", [], "resource.out", 0x0, None, False, None),
    ("dialog.exe", [], "dialog.out", 0x5, None, False, "dialog/input.txt"),
]

# The images a scripted fixture dumps, each with a golden NAME.ppm.gz.
DUMPS = {
    "window.exe": ["window"],
    "dialog.exe": ["dialog-menu", "dialog-settings"],
}


def run_cards(args, expected_dir):
    folder = pathlib.Path(args.cards) if args.cards else None
    if folder is None or not (folder / "cards.dll").is_file():
        print("skipped: needs the user's cards.dll in SAVANXP_WIN32_FIXTURES")
        return 77
    with tempfile.TemporaryDirectory() as scratch:
        shutil.copyfile(pathlib.Path(args.fixtures) / "cards.exe", pathlib.Path(scratch) / "cards.exe")
        shutil.copyfile(folder / "cards.dll", pathlib.Path(scratch) / "cards.dll")
        result = subprocess.run([args.harness, "--dlls", args.system32, str(pathlib.Path(scratch) / "cards.exe")],
                                capture_output=True, timeout=120)
    want = (expected_dir / "cards.out").read_bytes()
    if result.stdout != want or result.returncode != 0:
        got = result.stdout.decode(errors="replace").splitlines()
        wanted = want.decode().splitlines()
        differ = [g for g, w in zip(got, wanted) if g != w]
        print(f"FAIL cards.exe: status {result.returncode}, {len(differ)} lines differ, first {differ[:3]!r}; "
              f"stderr {result.stderr.decode(errors='replace')!r}")
        return 1
    print("ok   cards.exe -> cards.out")
    return 0


def run_solitaire(args, expected_dir):
    folder = pathlib.Path(args.solitaire) if args.solitaire else None
    if folder is None or not all((folder / name).is_file() for name in ("sol.exe", "cards.dll")):
        print("skipped: needs the user's sol.exe and cards.dll in SAVANXP_WIN32_FIXTURES")
        return 77
    wanted = {}
    for line in (expected_dir / "solitaire.sha256").read_text().splitlines():
        digest, name = line.split()
        wanted[name] = digest
    problems = []
    with tempfile.TemporaryDirectory() as scratch:
        scratch = pathlib.Path(scratch)
        for name in ("sol.exe", "cards.dll"):
            shutil.copyfile(folder / name, scratch / name)
        registry = scratch / "hkcu.reg"
        for run in (1, 2):
            script = expected_dir.parent / f"solitaire/input-{run}.txt"
            result = subprocess.run([args.harness, "--dlls", args.system32, "--registry", str(registry),
                                     "--time", SOLITAIRE_TIME, "--input", str(script), str(scratch / "sol.exe")],
                                    capture_output=True, timeout=300, cwd=scratch)
            errors = result.stderr.decode(errors="replace").splitlines()
            if result.returncode != 0 or "win32: exit status 0x00000000" not in errors:
                problems.append(f"run {run}: status {result.returncode} {errors[-3:]!r}")
            if run == 1 and registry.read_bytes() != (expected_dir / "solitaire.reg").read_bytes():
                problems.append(f"the registry after run 1 is {registry.read_text()!r}")
        for name, digest in wanted.items():
            dump = scratch / name
            if not dump.exists():
                problems.append(f"{name} was not written")
            elif hashlib.sha256(dump.read_bytes()).hexdigest() != digest:
                kept = pathlib.Path.cwd() / f"win32-solitaire-{name}"
                kept.write_bytes(dump.read_bytes())
                problems.append(f"{name} differs; it is kept as {kept}")
    if problems:
        print("FAIL sol.exe: " + "; ".join(problems))
        return 1
    print(f"ok   sol.exe: {len(wanted)} images")
    return 0


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--harness", required=True)
    parser.add_argument("--system32", required=True)
    parser.add_argument("--fixtures", required=True)
    parser.add_argument("--expected", required=True)
    parser.add_argument("--cards", help="the folder holding the user's cards.dll")
    parser.add_argument("--solitaire", help="the folder holding the user's sol.exe and cards.dll")
    args = parser.parse_args()
    expected_dir = pathlib.Path(args.expected)
    if args.cards is not None:
        return run_cards(args, expected_dir)
    if args.solitaire is not None:
        return run_solitaire(args, expected_dir)

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
            for stem in DUMPS.get(name, []) if script else []:
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
