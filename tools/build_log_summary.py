#!/usr/bin/env python3
"""Summarize one build.sh step whose full output went to a log file.

build.sh calls this in quiet mode (SAVANXP_QUIET=1 or --quiet) after running
a step with its output redirected. It prints one line saying whether the step
passed, and only what is needed to act on the result: the compiler warnings,
Ninja's FAILED blocks, the tests that failed or did not run. The full output
stays in the log. See docs/build/BUILD_CMAKE.md, "Quiet output".
"""

import argparse
import os
import re
import sys
from pathlib import Path

MAX_WARNINGS = 10
MAX_FAILURE_LINES = 80
TAIL_LINES = 40
# A commit alone costs about 150 steps (the build number and the SXE stamps);
# a build well past that is worth one line on why.
REBUILD_REPORT_STEPS = 300
MAX_REBUILD_CAUSES = 3

NINJA_STEP = re.compile(r"^\[(\d+)/(\d+)\] ")
WARNING = re.compile(r"^\S+:\d+(:\d+)?: warning: ")
EXPLAIN = "ninja explain: "


def shorten(text, roots):
    for root in roots:
        text = text.replace(root + "/", "")
    return text


def cap(lines, limit, what="lines"):
    if len(lines) <= limit:
        return lines
    return lines[:limit] + [f"... {len(lines) - limit} more {what} in the log"]


def warnings_of(lines):
    seen = []
    for line in lines:
        if WARNING.match(line) and line not in seen:
            seen.append(line)
    return seen


def rebuild_causes(lines):
    """Group Ninja's -d explain lines by cause, most frequent first.

    "X is dirty" only repeats a cause further down the graph, so it is left
    out; what remains says which input was newer, or that a command line or a
    dependency record changed.
    """
    counts, examples = {}, {}
    for line in lines:
        if not line.startswith(EXPLAIN) or line.endswith(" is dirty"):
            continue
        reason = line[len(EXPLAIN):]
        newer = re.search(r"older than most recent input (\S+)", reason)
        if newer:
            key = f"older than {newer.group(1)}"
        elif reason.startswith("command line changed for "):
            key = "command line changed"
        elif reason.startswith("deps for "):
            key = "dependency records missing"
        elif "doesn't exist" in reason:
            key = "output missing"
        else:
            key = " ".join(reason.split()[:6])
        counts[key] = counts.get(key, 0) + 1
        examples.setdefault(key, reason)
    ranked = sorted(counts, key=lambda k: -counts[k])[:MAX_REBUILD_CAUSES]
    return [f"{counts[k]:>6}  {k}  (e.g. {examples[k][:120]})" for k in ranked]


def ninja_failures(lines):
    """Ninja's FAILED blocks: the command and the compiler output under it."""
    out, inside = [], False
    for line in lines:
        if line.startswith("FAILED: "):
            inside = True
        elif NINJA_STEP.match(line) or line.startswith("ninja: "):
            inside = False
        if inside:
            out.append(line)
        if line.startswith("ninja: build stopped"):
            out.append(line)
    return out


def cmake_errors(lines):
    for index, line in enumerate(lines):
        if line.startswith("CMake Error"):
            return lines[index:]
    return []


def ctest_report(lines):
    """The pass/fail total, the failed tests' output, and what did not run."""
    total = [l for l in lines if re.match(r"^\d+% tests passed", l)]
    outputs, inside = [], False
    for line in lines:
        if line.startswith("Output:"):
            inside = True
            continue
        if inside:
            if line.startswith("<end of output>"):
                inside = False
                continue
            if not set(line) <= {"-"}:
                outputs.append("  " + line)
    sections = []
    for heading in ("The following tests did not run:", "The following tests FAILED:"):
        if heading in lines:
            start = lines.index(heading)
            block = [heading]
            for line in lines[start + 1:]:
                if not line.startswith(("\t", " ")):
                    break
                block.append(line)
            sections.append(block)
    return total, outputs, sections


def summarize(kind, label, log, status, seconds, roots):
    text = log.read_text(encoding="utf-8", errors="replace")
    lines = [shorten(l.rstrip(), roots) for l in text.splitlines()]
    shown_log = shorten(str(log), roots)
    out = []

    if kind == "ctest":
        total, outputs, sections = ctest_report(lines)
        verdict = "OK" if status == 0 else f"FAILED (exit {status})"
        detail = total[0] if total else "no test total in the log"
        out.append(f"==> {label}: {verdict}, {detail}, {seconds}s (log: {shown_log})")
        for block in sections:
            out.extend(block)
        if status != 0:
            out.extend(cap(outputs, MAX_FAILURE_LINES))
            if not total and not outputs:
                out.extend(lines[-TAIL_LINES:])
        return out

    steps = [m for m in map(NINJA_STEP.match, lines) if m]
    warnings = warnings_of(lines)
    facts = []
    if kind == "ninja":
        facts.append(f"{len(steps)} step{'s' if len(steps) != 1 else ''}" if steps else "no work to do")
    facts.append(f"{len(warnings)} warning{'s' if len(warnings) != 1 else ''}")
    facts.append(f"{seconds}s")
    if status == 0:
        out.append(f"==> {label}: OK, {', '.join(facts)} (log: {shown_log})")
        out.extend(cap(warnings, MAX_WARNINGS, "warnings"))
        if kind == "configure":
            out.extend(l for l in lines if l.startswith("CMake Warning"))
        causes = rebuild_causes(lines) if len(steps) >= REBUILD_REPORT_STEPS else []
        if causes:
            out.append(f"--- why {len(steps)} steps (ninja -d explain, by count) ---")
            out.extend(causes)
        return out

    out.append(f"==> {label}: FAILED (exit {status}), {', '.join(facts)} (log: {shown_log})")
    detail = ninja_failures(lines) if kind == "ninja" else []
    detail = detail or cmake_errors(lines)
    if detail:
        out.extend(cap(detail, MAX_FAILURE_LINES))
    else:
        out.append(f"--- last {TAIL_LINES} lines ---")
        out.extend(lines[-TAIL_LINES:])
    return out


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--kind", choices=("configure", "ninja", "ctest", "plain"), required=True)
    parser.add_argument("--label", required=True)
    parser.add_argument("--status", type=int, required=True)
    parser.add_argument("--seconds", type=int, default=0)
    parser.add_argument("--root", action="append", default=[],
                        help="path prefix to drop from the lines shown (repeatable)")
    parser.add_argument("--build-dir", type=Path,
                        help="also drop the source path this build was configured with")
    parser.add_argument("log", type=Path)
    args = parser.parse_args()
    # The compiler prints the source path CMake was configured with, which may
    # be another spelling (a symlink) of the root build.sh runs from.
    cache = args.build_dir / "CMakeCache.txt" if args.build_dir else None
    if cache and cache.is_file():
        for line in cache.read_text(encoding="utf-8", errors="replace").splitlines():
            if line.startswith("CMAKE_HOME_DIRECTORY:INTERNAL="):
                args.root.append(line.split("=", 1)[1])
    roots = sorted({r for root in args.root for r in (root, os.path.realpath(root))},
                   key=len, reverse=True)
    for line in summarize(args.kind, args.label, args.log, args.status, args.seconds, roots):
        print(line)
    return 0


if __name__ == "__main__":
    sys.exit(main())
