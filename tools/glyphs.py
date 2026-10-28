#!/usr/bin/env python3
"""Reads the font tables out of the built libsxgfx and renders text.

The visual scenarios could assert colour and geometry, but not the identity of the
characters on screen: a toolkit that painted every label with the wrong glyph, in
the wrong font, or at the wrong offset still passed every check there was. That is
the gap this closes.

The tables are read from libsxgfx.so.0.4 rather than from the generated .inc files
on purpose. The library is what the process actually maps, so this asserts about
the shipped artifact. A .inc that disagreed with the binary would make a
source-derived expectation quietly meaningless.

Nothing here reimplements text layout. What it does is apply the same arithmetic
the blitter applies -- baseline at ascent, glyph bitmap placed at left/top, pen
advanced by the glyph's advance, coverage blended with inv = 255 - alpha -- to
produce the pixels a correct blit would leave. A disagreement between this and
the screen is either a bug in the blitter or a wrong expectation, and both are
worth failing on.

Array sizes are derived rather than assumed. The glyph count falls out of the
ranges (base + last - first + 1, maximised), and the coverage extent out of the
glyphs themselves. So if the generator ever emits a different number of glyphs,
this follows it instead of reading past the end of what is there.
"""

from __future__ import annotations

import re
import struct
import subprocess
from dataclasses import dataclass
from pathlib import Path

# struct sx_noto_glyph { short width, rows, left, top, advance; unsigned offset; }
#
# The five shorts are 10 bytes and the unsigned lands at 12, so the entry occupies
# 16 with four bytes of tail padding. Reading it as a packed 12-byte struct puts
# every offset field in the middle of the previous glyph's padding, which looks
# plausible until a glyph is 200 pixels into its coverage table.
GLYPH_FIELDS = struct.Struct("<hhhhh")
GLYPH_SIZE = 16
GLYPH_OFFSET_FIELD = 12
# struct sx_noto_glyph_range { unsigned first, last, base; }
RANGE_STRUCT = struct.Struct("<III")

PT_LOAD = 1


class FontError(Exception):
    pass


@dataclass(frozen=True)
class Glyph:
    width: int
    rows: int
    left: int
    top: int
    advance: int
    offset: int


@dataclass(frozen=True)
class Font:
    """One of the two raster fonts, with the metrics needed to place a line."""

    name: str
    ascent: int
    line_height: int
    ranges: list[tuple[int, int, int]]
    glyphs: list[Glyph]
    coverage: bytes

    def glyph(self, codepoint: int) -> Glyph:
        for first, last, base in self.ranges:
            if first <= codepoint <= last:
                return self.glyphs[base + (codepoint - first)]
        return self.glyphs[0]

    def width(self, text: str) -> int:
        total = 0
        for char in text:
            total += self.glyph(ord(char)).advance
        return total


class SharedObject:
    """Just enough ELF64 to turn a symbol's address into bytes in the file."""

    def __init__(self, path: Path):
        self.path = path
        self.data = path.read_bytes()
        if self.data[:4] != b"\x7fELF" or self.data[4] != 2:
            raise FontError(f"{path}: not a 64-bit ELF object")
        (self.phoff,) = struct.unpack_from("<Q", self.data, 0x20)
        (self.phentsize, self.phnum) = struct.unpack_from("<HH", self.data, 0x36)
        self.loads = []
        for index in range(self.phnum):
            base = self.phoff + index * self.phentsize
            p_type, _flags, p_offset, p_vaddr, _paddr, p_filesz, p_memsz, _align = (
                struct.unpack_from("<IIQQQQQQ", self.data, base)
            )
            if p_type == PT_LOAD and p_filesz:
                self.loads.append((p_vaddr, p_memsz, p_offset, p_filesz))

    def read(self, vaddr: int, size: int) -> bytes:
        for p_vaddr, p_memsz, p_offset, p_filesz in self.loads:
            if p_vaddr <= vaddr and vaddr + size <= p_vaddr + p_memsz:
                start = p_offset + (vaddr - p_vaddr)
                if start + size > p_offset + p_filesz:
                    break
                return self.data[start : start + size]
        raise FontError(
            f"{self.path.name}: no loadable segment holds {size} bytes at 0x{vaddr:x}"
        )

    def symbols(self) -> dict[str, int]:
        out = subprocess.run(
            ["nm", str(self.path)], capture_output=True, text=True, check=False
        ).stdout
        found = {}
        for line in out.splitlines():
            parts = line.split()
            # "000000000000a0b0 r kNotoCoverage" -- value, type, name.
            if len(parts) == 3 and re.fullmatch(r"[0-9a-fA-F]+", parts[0]):
                found[parts[2]] = int(parts[0], 16)
        return found


def _load_font(obj: SharedObject, symbols: dict, name: str, ascent: int, line_height: int) -> Font:
    prefix = "kNoto" if name == "body" else "kTitle"
    try:
        ranges_at = symbols[prefix + "GlyphRanges"]
        glyphs_at = symbols[prefix + "Glyphs"]
        coverage_at = symbols[prefix + "Coverage"]
    except KeyError as exc:
        raise FontError(f"libsxgfx has no symbol {exc.args[0]}") from None

    # The range table is terminated by an all-zero entry and sits immediately
    # below the glyph table, so the count comes from the gap between the two
    # addresses rather than from guessing where the zeroes start. Reading past
    # the terminator would interpret glyph records as ranges, and some of them
    # happen to look like ranges.
    span = glyphs_at - ranges_at
    if span <= 0 or span % RANGE_STRUCT.size:
        raise FontError(
            f"{prefix}GlyphRanges and {prefix}Glyphs are not adjacent: {span} bytes apart"
        )
    ranges = []
    blob = obj.read(ranges_at, span)
    for offset in range(0, span, RANGE_STRUCT.size):
        first, last, base = RANGE_STRUCT.unpack_from(blob, offset)
        if first == 0 and last == 0 and base == 0:
            break
        if last < first or base == 0 or last > 0x10FFFF:
            raise FontError(f"{prefix}GlyphRanges entry at +{offset} is not a range")
        ranges.append((first, last, base))
    if not ranges:
        raise FontError(f"{prefix}GlyphRanges looks empty")

    count = max(base + (last - first) + 1 for first, last, base in ranges)
    glyphs = []
    extent = 0
    blob = obj.read(glyphs_at, GLYPH_SIZE * count)
    for index in range(count):
        base = index * GLYPH_SIZE
        width, rows, left, top, advance = GLYPH_FIELDS.unpack_from(blob, base)
        (offset,) = struct.unpack_from("<I", blob, base + GLYPH_OFFSET_FIELD)
        glyphs.append(Glyph(width, rows, left, top, advance, offset))
        extent = max(extent, offset + rows * width)

    # One extra byte: a table of exactly `extent` bytes would be indistinguishable
    # from one byte short, and the last glyph of a range is the one that shows it.
    coverage = obj.read(coverage_at, extent + 1)
    return Font(name, ascent, line_height, ranges, glyphs, coverage)


def load(library: str | Path) -> dict[str, Font]:
    """Returns {"body": Font, "title": Font} as shipped in libsxgfx."""
    obj = SharedObject(Path(library))
    symbols = obj.symbols()
    # The ascent and line height are #defines, not data, so they come from the
    # generated table's own header. They are placement, not pixels: getting them
    # wrong shifts the whole line and the assertion says so rather than guessing.
    root = Path(__file__).resolve().parent.parent
    def macro(path: str, name: str) -> int:
        text = (root / path).read_text()
        match = re.search(r"^#define %s (\d+)" % re.escape(name), text, re.M)
        if not match:
            raise FontError(f"no {name} in {path}")
        return int(match.group(1))

    body_inc = "system/gfx/gfx_font_noto.inc"
    title_inc = "system/gfx/gfx_font_title.inc"
    return {
        "body": _load_font(
            obj, symbols, "body",
            macro(body_inc, "SX_NOTO_ASCENT"),
            macro(body_inc, "SX_NOTO_LINE_HEIGHT"),
        ),
        "title": _load_font(
            obj, symbols, "title",
            macro(title_inc, "SX_TITLE_ASCENT"),
            macro(title_inc, "SX_TITLE_LINE_HEIGHT"),
        ),
    }


def blend(background: tuple, colour: tuple, alpha: int) -> tuple:
    """The same arithmetic as gfx_pixel_blend: inv = 255 - alpha, per channel /255."""
    if alpha >= 255:
        return colour
    inv = 255 - alpha
    return tuple(
        (colour[channel] * alpha + background[channel] * inv) // 255 for channel in range(3)
    )


def render(font: Font, x: int, y: int, text: str, colour: tuple, background: tuple,
           box: tuple) -> dict:
    """The pixels a correct blit leaves inside box = (x0, y0, x1, y1)."""
    expected = {}
    baseline = y + font.ascent
    pen_x = x
    for char in text:
        glyph = font.glyph(ord(char))
        for row in range(glyph.rows):
            py = baseline - glyph.top + row
            for column in range(glyph.width):
                px = pen_x + glyph.left + column
                if not (box[0] <= px < box[2] and box[1] <= py < box[3]):
                    continue
                alpha = font.coverage[glyph.offset + row * glyph.width + column]
                if alpha != 0:
                    expected[(px, py)] = blend(background, colour, alpha)
        pen_x += glyph.advance
    return expected


def inked(font: Font, x: int, y: int, text: str, box: tuple) -> set:
    """Just the positions the glyphs cover, for reporting and for geometry checks."""
    return set(render(font, x, y, text, (0, 0, 0), (0, 0, 0), box).keys())