#!/usr/bin/env python3
"""Check that the shell's `help` lists every installed command.

The text in shell_print_help() is written by hand, so it drifts whenever a
program is added. This compares it with the programs CMake registers:

- every program that is not a test app is listed, unless NOT_COMMANDS says
  why it is not one;
- every test app it lists sits inside the DESKTOP_INCLUDE_TEST_APPS block,
  because a build without test apps does not install it;
- every name it lists is a builtin or a registered program.
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

# Registered programs that are not typed at a prompt, and why.
NOT_COMMANDS = {
    "init": "the first process",
    "sh": "the shell itself",
    "audiod": "a daemon started by init",
    "compositord": "a desktop service",
    "sessiond": "a desktop service",
    "elevationd": "a desktop service",
    "windowd": "a desktop service",
    "x11d": "the X server the linux launcher starts",
    "mediaplayer": "only built when FFmpeg is installed",
}

CALL = re.compile(r"\b(savanxp_program|savanxp_system_service)\((.*?)\)", re.S)
# foreach(tool cat echo ls) ... savanxp_program(NAME ${tool} ...) ... endforeach()
LOOP = re.compile(r"\bforeach\((\w+)\s+([^)]*)\)(.*?)\bendforeach\(\)", re.S)
HELP_LINE = re.compile(r'shell_emit_text\(sink, 1, "([^"]*)\\n"\);')


def registered_programs(root: Path) -> dict[str, bool]:
    """Map each registered program to whether it is a test app."""
    programs: dict[str, bool] = {}
    for name in ("cmake/UserPrograms.cmake", "cmake/SystemServices.cmake"):
        text = (root / name).read_text(encoding="utf-8")
        for match in CALL.finditer(text):
            words = match.group(2).split()
            if len(words) < 2 or words[0] != "NAME" or words[1].startswith("$"):
                continue
            programs[words[1]] = "TEST" in words
        for loop in LOOP.finditer(text):
            variable, values, body = loop.group(1), loop.group(2).split(), loop.group(3)
            for match in CALL.finditer(body):
                words = match.group(2).split()
                if words[:2] == ["NAME", "${" + variable + "}"]:
                    for value in values:
                        programs[value] = "TEST" in words
    return programs


def builtins(source: str) -> set[str]:
    body = source.split("static int shell_is_builtin", 1)[1].split("}", 1)[0]
    # `start` is handled before the builtins, but it is typed like one.
    return set(re.findall(r'strcmp\(command, "([a-z]+)"\)', body)) | {"start"}


def help_names(source: str) -> tuple[set[str], set[str]]:
    """The names help lists always, and the ones only in a test-app build."""
    body = source.split("void shell_print_help", 1)[1].split("\n}\n", 1)[0]
    body = body.split("Examples:", 1)[0]
    always: set[str] = set()
    test_only: set[str] = set()
    in_test_block = False
    for line in body.splitlines():
        stripped = line.strip()
        if stripped.startswith("#if DESKTOP_INCLUDE_TEST_APPS"):
            in_test_block = True
            continue
        if stripped.startswith("#endif"):
            in_test_block = False
            continue
        match = HELP_LINE.search(line)
        if not match:
            continue
        words = match.group(1).split(":", 1)[-1].split()
        (test_only if in_test_block else always).update(words)
    return always, test_only


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", required=True, type=Path)
    args = parser.parse_args()
    root = args.root.resolve()

    source = (root / "subsystems/posix/userland/shell_core.c").read_text(encoding="utf-8")
    programs = registered_programs(root)
    known = builtins(source)
    always, test_only = help_names(source)
    listed = always | test_only
    errors: list[str] = []

    if not programs or not always:
        errors.append("parsed nothing: the CMake files or shell_print_help changed shape")

    for name in sorted(listed):
        if name not in programs and name not in known:
            errors.append(f"help lists '{name}', which is neither a builtin nor a registered program")
    for name in sorted(always):
        if programs.get(name):
            errors.append(f"help lists test app '{name}' outside the DESKTOP_INCLUDE_TEST_APPS block")
    for name in sorted(test_only):
        if name in programs and not programs[name]:
            errors.append(f"help lists '{name}' as a test app, but it is always installed")
    for name, is_test in sorted(programs.items()):
        if not is_test and name not in listed and name not in NOT_COMMANDS:
            errors.append(f"'{name}' is installed but help does not list it "
                          "(add it to shell_print_help or to NOT_COMMANDS with a reason)")
    for name in sorted(NOT_COMMANDS):
        if name not in programs:
            errors.append(f"NOT_COMMANDS names '{name}', which is no longer registered")

    for error in errors:
        print(f"shell-help-test: {error}", file=sys.stderr)
    if errors:
        return 1
    print(f"shell-help-test: ok, {len(listed)} names checked against {len(programs)} programs")
    return 0


if __name__ == "__main__":
    sys.exit(main())
