#!/usr/bin/env python3
"""Compares text.exe's output from the harness with what Windows XP printed
for it (docs/userland/WIN32.md, "Text").

Every line must be equal, except where FreeType and XP's rasterizer are
allowed to part:

- a line ending in "pixels N" (the ink in an area): the box must be equal,
  the count within 3% or 4 pixels;
- "lines levels": every grey XP used must be within one level of one of
  ours (ours may have the 1/16 level XP leaves out);
- the "family:" lines of one enumeration: the same lines in any order, since
  XP lists faces in the order they were installed;
- with widths=1, the "widths" lines may differ by a pixel per glyph and the
  extents by 3, for Microsoft's own faces, whose hinting FreeType does not
  reproduce glyph for glyph;
- with substitutes=True, what follows a "missing:" line, the face Windows
  substitutes for a family nobody has: on SavanXP it is Liberation.

A line an older capture lacks ("font smoothing", "lines levels") is
skipped. Usable as a module (compare) or a script: compare XP.OUT OURS.OUT.
"""

import re
import sys

PIXELS = re.compile(r"^(.*) pixels (\d+)$")

# Lines text.exe gained after the first captures were taken.
OPTIONAL = ("font smoothing", "lines levels")


def _numbers(line):
    head, _, tail = line.partition(":")
    return head, [int(v, 0) for v in tail.split()]


def _blocks(lines):
    """The lines, each run of "  family:" lines folded into one sorted tuple."""
    out = []
    run = []
    for line in lines:
        if line.startswith("  family:"):
            run.append(line)
            continue
        if run:
            out.append(tuple(sorted(run)))
            run = []
        out.append(line)
    if run:
        out.append(tuple(sorted(run)))
    return out


def _drop_substitutes(lines):
    out = []
    skipping = False
    for line in lines:
        if line.startswith("missing:"):
            skipping = True
            out.append(line)
            continue
        if skipping and line.startswith("  "):
            continue
        skipping = False
        out.append(line)
    return out


def _close(xp, ours, widths):
    if isinstance(xp, tuple) or isinstance(ours, tuple):
        return xp == ours
    if xp == ours:
        return True
    a = PIXELS.match(xp)
    b = PIXELS.match(ours)
    if a and b:
        if a.group(1) != b.group(1):
            return False
        n, m = int(a.group(2)), int(b.group(2))
        return abs(n - m) <= max(4, n * 3 // 100)
    if xp.startswith("lines levels:") and ours.startswith("lines levels:"):
        mine = _numbers(ours)[1]
        return all(any(abs(v - w) <= 1 for w in mine) for v in _numbers(xp)[1])
    if widths and ("widths" in xp or "extent" in xp):
        try:
            (h1, v1), (h2, v2) = _numbers(xp), _numbers(ours)
        except ValueError:
            return False
        slack = 1 if "widths" in xp else 3
        return h1 == h2 and len(v1) == len(v2) and all(abs(p - q) <= slack for p, q in zip(v1, v2))
    return False


def compare(xp_text, ours_text, widths=0, substitutes=False):
    """The lines that differ beyond the tolerances, as messages."""
    xp = xp_text.splitlines()
    ours = ours_text.splitlines()
    for label in OPTIONAL:
        if not any(line.startswith(label) for line in xp):
            ours = [line for line in ours if not line.startswith(label)]
    if substitutes:
        xp = _drop_substitutes(xp)
        ours = _drop_substitutes(ours)
    xp = _blocks(xp)
    ours = _blocks(ours)
    problems = []
    if len(xp) != len(ours):
        problems.append(f"{len(ours)} lines, Windows XP printed {len(xp)}")
    for index, (a, b) in enumerate(zip(xp, ours)):
        if not _close(a, b, widths):
            show_a = a if isinstance(a, str) else f"{len(a)} family lines starting {a[0]!r}"
            show_b = b if isinstance(b, str) else f"{len(b)} family lines starting {b[0]!r}"
            problems.append(f"line {index + 1}: {show_b!r}, Windows XP {show_a!r}")
    return problems


def main():
    if len(sys.argv) < 3:
        print("usage: win32_text_compare.py XP.OUT OURS.OUT [--widths] [--substitutes]")
        return 2
    xp = open(sys.argv[1], encoding="latin-1").read()
    ours = open(sys.argv[2], encoding="latin-1").read()
    problems = compare(xp, ours, widths=1 if "--widths" in sys.argv else 0, substitutes="--substitutes" in sys.argv)
    for line in problems:
        print(line)
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
