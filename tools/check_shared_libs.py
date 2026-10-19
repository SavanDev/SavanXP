#!/usr/bin/env python3
"""Checks that no executable carries a private copy of a library it maps.

A program that declares DT_NEEDED for a shared library is supposed to leave that
library's symbols undefined, so the linker resolves them through the library and
the code is mapped once. If it also defines them, there are two copies of the
same function in the process, the executable's own copy wins, and the library is
mapped for nothing.

Nothing goes wrong when that happens. The program links, it runs, every test
passes, and the only symptom is that the sharing the migration was for did not
occur. So it is checked here instead of being left to be noticed.

The failure is also easy to cause by accident: add a unit to a library and forget
to add it to SAVANXP_LIBRARY_REPLACES_<lib>, or add a DT_NEEDED without the DEPENDS
that removes the unit from the runtime. Both leave the copy in the executable.

Usage:
    tools/check_shared_libs.py BUILD_DIR [--verbose]
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
from pathlib import Path

# Symbols the linker puts in every object it links. They are not the library's
# code and a program having one is not a second copy of anything.
LINKER_SYMBOLS = {"_DYNAMIC", "_GLOBAL_OFFSET_TABLE_", "_PROCEDURE_LINKAGE_TABLE_",
                  "_edata", "_end", "__bss_start", "__init_array_start",
                  "__init_array_end", "__fini_array_start", "__fini_array_end",
                  "__dso_handle", "__GNU_EH_FRAME_HDR", "__TMC_END__"}


def library_symbols(library_dir):
    """The exported symbols of each built library, keyed by its DT_NEEDED name.

    Read from the artifacts rather than from a list of prefixes here. A prefix list
    is a maintenance obligation that is wrong the moment a library is added: when
    libgfx2d appeared, a check that knew about gfx_ and sxgui_ would have said
    nothing about sx_painter_*, which is most of what a toolkit duplicates.

    Only a library that a program actually maps is consulted below. Carrying
    unused code that belongs to a library the program does not map is a size
    problem, not a second copy of anything -- --gc-sections is the answer to that
    one, and it is a different question.
    """
    found = {}
    if not library_dir.is_dir():
        return found
    for path in sorted(library_dir.glob("*.so.*")):
        if path.is_symlink() or not path.is_file():
            continue
        out = subprocess.run(
            ["nm", "--defined-only", "--format=posix", str(path)],
            capture_output=True, text=True, check=False,
        ).stdout
        symbols = set()
        for line in out.splitlines():
            parts = line.split()
            if len(parts) >= 2 and parts[1] in "TtDdBbRrWwVv":
                if parts[0] not in LINKER_SYMBOLS:
                    symbols.add(parts[0])
        found[path.name] = symbols
    return found


def readelf_dynamic(path: Path) -> list[str]:
    out = subprocess.run(
        ["readelf", "-dW", str(path)], capture_output=True, text=True, check=False
    ).stdout
    return re.findall(r"\[(lib[a-z0-9_.]+)\]", out)


def defined_symbols(path: Path) -> set[str]:
    out = subprocess.run(
        ["nm", str(path)], capture_output=True, text=True, check=False
    ).stdout
    found = set()
    for line in out.splitlines():
        parts = line.split()
        if len(parts) == 3 and parts[1] in "TtDdBbRrWw":
            found.add(parts[2])
    return found


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("build_dir", nargs="?", default="build/linux")
    parser.add_argument("--verbose", action="store_true")
    args = parser.parse_args()

    root = Path(args.build_dir)
    if not root.is_dir():
        print(f"check_shared_libs: {root} is not a directory", file=sys.stderr)
        return 2

    library_dir = root.parent / "diskfs" / "lib"
    libraries = library_symbols(library_dir)
    if not libraries:
        print(
            f"check_shared_libs: no built libraries under {library_dir}, so there is "
            f"nothing to compare against",
            file=sys.stderr,
        )
        return 2

    violations = []
    checked = 0
    for path in sorted(root.iterdir()):
        if not path.is_file():
            continue
        needed = readelf_dynamic(path)
        mapped = [lib for lib in needed if lib in libraries]
        if not mapped:
            continue
        # A library itself defines its own symbols; that is its job.
        if ".so." in path.name:
            continue
        checked += 1
        symbols = defined_symbols(path)
        for lib in mapped:
            owned = symbols & libraries[lib]
            if owned:
                sample = ", ".join(sorted(owned)[:6])
                violations.append(
                    f"{path.name}: maps {lib} and defines {len(owned)} of its "
                    f"symbols ({sample})"
                )

    if violations:
        print(
            "check_shared_libs: an executable carries a private copy of a library "
            "it maps",
            file=sys.stderr,
        )
        for line in violations:
            print(f"  {line}", file=sys.stderr)
        print(
            "\nThe DT_NEEDED is real, so the library is loaded, but the executable's "
            "own copy\nsatisfies the references and nothing is shared. Add the unit to "
            "SAVANXP_LIBRARY_REPLACES_<lib>\nin CMakeLists.txt and declare the library in "
            "the program's DEPENDS.",
            file=sys.stderr,
        )
        return 1

    if args.verbose:
        print(
            f"check_shared_libs: {checked} programs map one of "
            f"{len(libraries)} libraries ({', '.join(sorted(libraries))}), none duplicates it"
        )
    else:
        print("check_shared_libs: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())