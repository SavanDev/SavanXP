#!/usr/bin/env python3
"""Stage third-party license notices into /system/licenses, and check them.

Every component that ships in the image has a `.notice` file: the base system's
under `assets/licenses/`, each port's under `ports/<name>/licenses/`. The
format is described in docs/third-party/LICENSE_NOTICES.md.

    stage_licenses.py stage --root REPO --out DIR NOTICE...
        Writes DIR/<id>/NOTICE and copies every `text=` file next to it, by
        basename. The id is the notice's file name without `.notice`.

    stage_licenses.py check --root REPO
        Validates every notice in the tree and checks that each entry of
        docs/third-party/THIRD_PARTY_PROVENANCE.md that ships something has a
        notice naming it with `provenance=`.
"""

from __future__ import annotations

import argparse
import shutil
import sys
from pathlib import Path

REQUIRED = ("name", "license", "provenance")
SINGLE = ("name", "license", "provenance", "origin", "version", "source")
REPEATABLE = ("copyright", "used-in", "note", "text")
PROVENANCE_DOC = Path("docs/third-party/THIRD_PARTY_PROVENANCE.md")
# Registry entries that ship nothing third-party: the project's own art.
NOT_THIRD_PARTY = {"Desktop icons and art", "Cursor imports and original artwork"}
# Sections of the registry whose entries do not end up in any image.
NOT_SHIPPED_SECTIONS = {"Build only: not distributed"}


class NoticeError(Exception):
    pass


def notice_id(path: Path) -> str:
    return path.stem


def parse(path: Path) -> list[tuple[str, str]]:
    fields: list[tuple[str, str]] = []
    seen: set[str] = set()
    for number, raw in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        key, sep, value = line.partition("=")
        key, value = key.strip(), value.strip()
        if not sep or not value:
            raise NoticeError(f"{path}:{number}: expected key=value")
        if key not in SINGLE and key not in REPEATABLE:
            raise NoticeError(f"{path}:{number}: unknown key '{key}'")
        if key in SINGLE and key in seen:
            raise NoticeError(f"{path}:{number}: '{key}' given twice")
        seen.add(key)
        fields.append((key, value))
    missing = [key for key in REQUIRED if key not in seen]
    if missing:
        raise NoticeError(f"{path}: missing {', '.join(missing)}")
    return fields


def texts(root: Path, path: Path, fields: list[tuple[str, str]]) -> list[Path]:
    files: list[Path] = []
    names: set[str] = {"NOTICE"}
    for key, value in fields:
        if key != "text":
            continue
        source = root / value
        if not source.is_file():
            raise NoticeError(f"{path}: text file not found: {value}")
        if source.name in names:
            raise NoticeError(f"{path}: two text files named {source.name}")
        names.add(source.name)
        files.append(source)
    return files


def all_notices(root: Path) -> list[Path]:
    return sorted((root / "assets/licenses").glob("*.notice")) + sorted(
        (root / "ports").glob("*/licenses/*.notice"))


def stage(root: Path, out: Path, notices: list[Path]) -> None:
    for path in notices:
        fields = parse(path)
        files = texts(root, path, fields)
        target = out / notice_id(path)
        if target.exists():
            shutil.rmtree(target)
        target.mkdir(parents=True)
        lines = [f"{key}={Path(value).name if key == 'text' else value}" for key, value in fields]
        (target / "NOTICE").write_text("\n".join(lines) + "\n", encoding="utf-8")
        for source in files:
            shutil.copyfile(source, target / source.name)


def shipped_entries(root: Path) -> list[str]:
    entries: list[str] = []
    section = ""
    for line in (root / PROVENANCE_DOC).read_text(encoding="utf-8").splitlines():
        if line.startswith("## "):
            section = line[3:].strip()
        elif line.startswith("### "):
            title = line[4:].strip()
            if section not in NOT_SHIPPED_SECTIONS and title not in NOT_THIRD_PARTY:
                entries.append(title)
    return entries


def check(root: Path) -> int:
    errors: list[str] = []
    named: dict[str, str] = {}
    ids: set[str] = set()
    for path in all_notices(root):
        try:
            fields = parse(path)
            texts(root, path, fields)
        except NoticeError as error:
            errors.append(str(error))
            continue
        ident = notice_id(path)
        if ident in ids:
            errors.append(f"{path}: id '{ident}' is used twice")
        ids.add(ident)
        named[dict(fields)["provenance"]] = str(path.relative_to(root))
    for entry in shipped_entries(root):
        if entry not in named:
            errors.append(f"{PROVENANCE_DOC}: '{entry}' ships but has no notice with provenance={entry}")
    for entry, path in named.items():
        if entry != "SavanXP" and entry not in shipped_entries(root):
            errors.append(f"{path}: provenance '{entry}' is not an entry of {PROVENANCE_DOC}")
    for error in errors:
        print(f"licenses: {error}", file=sys.stderr)
    if not errors:
        print(f"licenses: {len(named)} notices cover the provenance registry")
    return 1 if errors else 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    commands = parser.add_subparsers(dest="command", required=True)
    stage_parser = commands.add_parser("stage")
    stage_parser.add_argument("--root", type=Path, required=True)
    stage_parser.add_argument("--out", type=Path, required=True)
    stage_parser.add_argument("--all-base", action="store_true",
                              help="stage every notice under assets/licenses")
    stage_parser.add_argument("notices", nargs="*", type=Path)
    check_parser = commands.add_parser("check")
    check_parser.add_argument("--root", type=Path, required=True)
    args = parser.parse_args()

    root = args.root.resolve()
    try:
        if args.command == "check":
            return check(root)
        notices = [path if path.is_absolute() else root / path for path in args.notices]
        if args.all_base:
            notices += sorted((root / "assets/licenses").glob("*.notice"))
        if not notices:
            parser.error("no notices to stage")
        stage(root, args.out, notices)
    except NoticeError as error:
        print(f"licenses: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
