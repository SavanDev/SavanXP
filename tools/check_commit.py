#!/usr/bin/env python3
"""Check a commit against the mechanical rules of AGENTS.md.

Run by the git hooks in tools/git-hooks/ (enable them once per clone with
`git config core.hooksPath tools/git-hooks`):

  check_commit.py pre-commit        the staged changes
  check_commit.py commit-msg FILE   the commit message

Errors stop the commit; warnings only print. Only what a script can decide is
checked here: whether a change needs a changelog entry, or a comment is still
in Spanish, stays a person's judgment. See docs/build/BUILD_CMAKE.md,
"Commit checks".
"""

from __future__ import annotations

import fnmatch
import re
import subprocess
import sys

CHANGELOG = "CHANGELOG.md"
SUBSECTIONS = ["Added", "Changed", "Removed", "Fixed"]
MAX_ENTRY_LINES = 6

# Never committed (AGENTS.md, "Files that never enter the repository").
FORBIDDEN = ["*.exe", "*.dll", "*.wad", "*.log"]

# Host tooling that must not name one machine's paths (AGENTS.md, "Toolchain").
# Guest paths such as C:\windows or /mnt are the system's own and stay allowed.
TOOLING = ["build.sh", "CMakeLists.txt", "CMakePresets.json", "cmake/*", "*.cmake",
           "tools/*.sh", "ports/*.sh", "ports/*/*.sh"]
MACHINE_PATH = re.compile(r"(?:^|[^\w.])(/home/[\w.-]+/|/Users/[\w.-]+/)")

# Paths whose change never needs a changelog entry on its own.
NO_ENTRY_NEEDED = ["*.md", "docs/*", ".claude/*", "tests/*", ".gitignore", ".ignore",
                   "LICENSE"]

LINK_TARGET = re.compile(r"\]\(([^)\s]+)\)")


def git(*args: str, check: bool = True) -> str:
    result = subprocess.run(["git", *args], capture_output=True, text=True)
    if check and result.returncode != 0:
        raise SystemExit(f"check_commit: git {' '.join(args)} failed: {result.stderr.strip()}")
    return result.stdout


def matches(path: str, patterns: list[str]) -> bool:
    name = path.rsplit("/", 1)[-1]
    return any(fnmatch.fnmatch(path, p) or fnmatch.fnmatch(name.lower(), p) for p in patterns)


def staged_changes() -> list[tuple[str, str]]:
    """(status letter, path) for each staged file; a rename gives its new path."""
    out = []
    for line in git("diff", "--cached", "--name-status", "--no-renames").splitlines():
        status, path = line.split("\t", 1)
        out.append((status[0], path))
    return out


def added_lines(path: str) -> list[tuple[int, str]]:
    """(line number in the staged file, text) of every line the commit adds."""
    out, number = [], 0
    for line in git("diff", "--cached", "-U0", "--", path).splitlines():
        hunk = re.match(r"@@ -\S+ \+(\d+)", line)
        if hunk:
            number = int(hunk.group(1))
        elif line.startswith("+") and not line.startswith("+++"):
            out.append((number, line[1:]))
            number += 1
    return out


def changelog_errors(text: str, added: set[int]) -> list[str]:
    errors = []
    if "## [Unreleased]" not in text:
        return [f"{CHANGELOG} has no ## [Unreleased] section"]
    lines = text.splitlines()
    start = lines.index("## [Unreleased]")
    end = next((i for i in range(start + 1, len(lines)) if lines[i].startswith("## ")), len(lines))
    order = [l[4:].strip() for l in lines[start:end] if l.startswith("### ")]
    if order != [s for s in SUBSECTIONS if s in order] or len(order) != len(set(order)):
        errors.append(f"[Unreleased] subsections must appear once each, in the order "
                      f"{'/'.join(SUBSECTIONS)}; found {'/'.join(order)}")
    index = start + 1
    while index < end:
        if not lines[index].startswith("- "):
            index += 1
            continue
        first = index
        index += 1
        while index < end and lines[index].startswith("  "):
            index += 1
        if not any(first + 1 <= n <= index for n in added):
            continue  # an entry this commit does not touch
        size = index - first
        if size > MAX_ENTRY_LINES:
            errors.append(f"{CHANGELOG}:{first + 1}: entry of {size} lines; the limit is 3 "
                          f"for a normal change and {MAX_ENTRY_LINES} for a large one "
                          f"(the rest belongs in the commit message or docs/)")
        if not lines[first].startswith("- **"):
            errors.append(f"{CHANGELOG}:{first + 1}: an entry opens with a bold sentence "
                          f"stating the change")
    return errors


def pre_commit() -> tuple[list[str], list[str]]:
    errors, warnings = [], []
    changes = staged_changes()
    for status, path in changes:
        if status != "D" and matches(path, FORBIDDEN):
            errors.append(f"{path}: this kind of file never enters the repository "
                          f"(AGENTS.md, \"Files that never enter the repository\")")
        if status != "D" and matches(path, TOOLING) and "/source/" not in path:
            for number, text in added_lines(path):
                if MACHINE_PATH.search(text):
                    errors.append(f"{path}:{number}: a path from one machine; resolve the "
                                  f"tool through CMake or PATH (AGENTS.md, \"Toolchain\")")
        if path.startswith("docs/changelog/") and status in "MD":
            if status == "D":
                errors.append(f"{path}: a released version's notes are never removed")
                continue
            before = LINK_TARGET.sub("]()", git("show", f"HEAD:{path}"))
            after = LINK_TARGET.sub("]()", git("show", f":{path}"))
            if before != after:
                errors.append(f"{path}: a released version's text is never edited; only a "
                              f"broken link may be repointed")
    paths = [p for _, p in changes]
    if CHANGELOG in paths:
        added = {n for n, _ in added_lines(CHANGELOG)}
        errors += changelog_errors(git("show", f":{CHANGELOG}"), added)
    elif any(not matches(p, NO_ENTRY_NEEDED) for p in paths):
        warnings.append(f"no {CHANGELOG} entry: right only if nothing changes as seen from "
                        f"the outside (a refactor, a comment translation)")
    return errors, warnings


def commit_msg(path: str) -> list[str]:
    with open(path, encoding="utf-8", errors="replace") as message:
        body = [l for l in message.read().splitlines() if not l.startswith("#")]
    if any(l.startswith("Claude-Session:") or "claude.ai/code/session" in l for l in body):
        return ["the message links an agent's session, which nobody reading the history can "
                "open (AGENTS.md, \"Commit messages\"); a Co-Authored-By trailer is fine"]
    return []


def main() -> int:
    if len(sys.argv) >= 2 and sys.argv[1] == "pre-commit":
        errors, warnings = pre_commit()
    elif len(sys.argv) == 3 and sys.argv[1] == "commit-msg":
        errors, warnings = commit_msg(sys.argv[2]), []
    else:
        print(__doc__, file=sys.stderr)
        return 2
    for warning in warnings:
        print(f"check_commit: warning: {warning}", file=sys.stderr)
    for error in errors:
        print(f"check_commit: error: {error}", file=sys.stderr)
    if errors:
        print("check_commit: commit stopped; fix the above, or ask the user before "
              "--no-verify", file=sys.stderr)
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
