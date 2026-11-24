#!/usr/bin/env python3
"""tools/check_commit.py: what the commit hooks stop, and what they let through."""

from __future__ import annotations

import argparse
import subprocess
import sys
import tempfile
from pathlib import Path

GIT = ["git", "-c", "user.name=test", "-c", "user.email=test@example.invalid",
       "-c", "commit.gpgsign=false", "-c", "core.hooksPath=/dev/null"]

CHANGELOG = """\
# Changelog

## [Unreleased]

### Added

- **An existing entry.** Two lines
  long.

### Fixed

- **An existing fix.**

## Previous versions

- [0.1.0](docs/changelog/v0.1.0.md) — 2026-01-01
"""

RELEASED = "# SavanXP 0.1.0\n\n## [0.1.0] - 2026-01-01\n\n- **Old.** See [x](../old/X.md).\n"


class Repo:
    def __init__(self, root: Path, tool: Path):
        self.root, self.tool = root, tool
        self.git("init", "-q")
        self.write("CHANGELOG.md", CHANGELOG)
        self.write("docs/changelog/v0.1.0.md", RELEASED)
        self.write("build.sh", "#!/bin/sh\n")
        self.git("add", ".")
        self.git("commit", "-q", "-m", "base")

    def git(self, *args: str) -> None:
        subprocess.run([*GIT, *args], cwd=self.root, check=True, capture_output=True)

    def write(self, path: str, text: str) -> None:
        target = self.root / path
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(text, encoding="utf-8")

    def stage(self, path: str, text: str) -> None:
        self.write(path, text)
        self.git("add", path)

    def reset(self) -> None:
        self.git("reset", "-q", "--hard", "HEAD")
        self.git("clean", "-qfd")

    def check(self, *args: str) -> tuple[int, str]:
        result = subprocess.run([sys.executable, str(self.tool), *args], cwd=self.root,
                                capture_output=True, text=True)
        return result.returncode, result.stderr


def with_entry(entry: str, where: str = "### Fixed\n") -> str:
    return CHANGELOG.replace(where, where + "\n" + entry, 1)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", required=True, type=Path)
    tool = parser.parse_args().root.resolve() / "tools" / "check_commit.py"

    with tempfile.TemporaryDirectory() as tmp:
        repo = Repo(Path(tmp), tool)

        repo.stage("fixtures/cards.DLL", "MZ")
        status, err = repo.check("pre-commit")
        assert status == 1 and "never enters the repository" in err, err
        repo.reset()

        repo.stage("build.sh", "#!/bin/sh\nCC=/home/alice/llvm/bin/clang\n")
        status, err = repo.check("pre-commit")
        assert status == 1 and "build.sh:2: a path from one machine" in err, err
        repo.reset()

        # A guest path is the system's own, not a machine's.
        repo.stage("cmake/Win32.cmake", "# copied to C:\\windows\\system32 and /mnt/usb\n")
        status, err = repo.check("pre-commit")
        assert status == 0 and "error" not in err, err
        repo.reset()

        # A code change with no entry is allowed, with a warning.
        repo.stage("kernel/x.cpp", "int x;\n")
        status, err = repo.check("pre-commit")
        assert status == 0 and "warning: no CHANGELOG.md entry" in err, err
        repo.reset()

        # Documentation alone needs no entry and gets no warning.
        repo.stage("docs/kernel/X.md", "# X\n")
        status, err = repo.check("pre-commit")
        assert status == 0 and err == "", err
        repo.reset()

        repo.stage("kernel/x.cpp", "int x;\n")
        repo.stage("CHANGELOG.md", with_entry("- **A fix.** In three\n  lines, which\n  is fine.\n"))
        status, err = repo.check("pre-commit")
        assert status == 0 and err == "", err
        repo.reset()

        long_entry = "- **Too long.**\n" + "".join(f"  line {n}\n" for n in range(6))
        repo.stage("CHANGELOG.md", with_entry(long_entry))
        status, err = repo.check("pre-commit")
        assert status == 1 and "entry of 7 lines" in err, err
        repo.reset()

        repo.stage("CHANGELOG.md", with_entry("- A plain entry.\n"))
        status, err = repo.check("pre-commit")
        assert status == 1 and "opens with a bold sentence" in err, err
        repo.reset()

        # Only the entries the commit touches are judged.
        repo.stage("CHANGELOG.md", CHANGELOG.replace("- **An existing fix.**",
                                                     "- an existing fix, plain"))
        status, err = repo.check("pre-commit")
        assert status == 1 and "CHANGELOG.md:12:" in err, err
        repo.reset()

        repo.stage("CHANGELOG.md", CHANGELOG.replace("### Fixed\n", "### Fixed\n\n- **B.**\n\n### Added\n"))
        status, err = repo.check("pre-commit")
        assert status == 1 and "must appear once each" in err, err
        repo.reset()

        repo.stage("docs/changelog/v0.1.0.md", RELEASED.replace("../old/X.md", "../new/X.md"))
        status, err = repo.check("pre-commit")
        assert status == 0 and err == "", err
        repo.reset()

        repo.stage("docs/changelog/v0.1.0.md", RELEASED.replace("**Old.**", "**Rewritten.**"))
        status, err = repo.check("pre-commit")
        assert status == 1 and "text is never edited" in err, err
        repo.reset()

        message = Path(tmp) / "MSG"
        message.write_text("fix: x\n\nCo-Authored-By: A <a@example.invalid>\n"
                           "Claude-Session: https://claude.ai/code/session_x\n")
        status, err = repo.check("commit-msg", str(message))
        assert status == 1 and "links an agent's session" in err, err
        message.write_text("fix: x\n\nCo-Authored-By: A <a@example.invalid>\n"
                           "# Claude-Session: in a comment git strips\n")
        status, err = repo.check("commit-msg", str(message))
        assert status == 0, err

    print("check-commit-test: OK")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
