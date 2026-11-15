"""Draw the shared toolbar glyphs and generate icons.inc.

The art is our own and lives in this file, drawn with the house icon kit
(tools/icon_art.py) so the glyphs share the set's outline and top-left
light. They are 20x20, the size of the Explorer's toolbar images with text
labels (SXGUI_TOOLBAR_ICON_SIZE); a disabled button draws the glyph's
silhouette embossed, so each one must read by its shape alone.

Writes icons/<name>.png (to look at) and icons.inc: one 0xAARRGGBB array per
glyph, the SX_PIXEL_FORMAT_BGRA8888 layout, converted as
gen_sxe_resources.py does. They are embedded rather than shipped as .sxicon:
nine glyphs are ~14 KiB, used by Files' toolbar and the file dialog
(system/runtime/sxnfiledlg.c), as the media player embeds its own
(subsystems/posix/userland/mediaplayer/gen_icons.py). A program includes
icons.inc and keeps the glyphs it names; the compiler drops the rest. Both
outputs are versioned; regenerate with:

    python system/gfx/toolbar_glyphs/gen_icons.py

The names follow freedesktop's Icon Naming Specification.
"""

import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "..", "..", "tools"))

from gen_sxe_resources import read_icon_png  # noqa: E402
from icon_art import (  # noqa: E402
    BLACK, FOLDER, GREEN, Icon, NAVY, PAPER, RED, SILVER, TEAL, WOOD, YELLOW, rgb, side_face, top_face,
)

SIZE = 20


def arrow_left(ic, ramp):
    ic.part(ic.poly([(1, 10), (9, 2), (9, 6), (18, 6), (18, 14), (9, 14), (9, 18)]), ramp)


def go_previous():
    """Back: a green arrow pointing left."""
    ic = Icon(SIZE)
    arrow_left(ic, GREEN)
    return ic.image


def go_next():
    """Forward: the same arrow pointing right (drawn, not mirrored, so the
    light still comes from the top left)."""
    ic = Icon(SIZE)
    ic.part(ic.poly([(18, 10), (10, 2), (10, 6), (1, 6), (1, 14), (10, 14), (10, 18)]), GREEN)
    return ic.image


def go_up():
    """Up One Level: a folder with a green arrow rising out of it."""
    ic = Icon(SIZE)
    ic.part(ic.poly([(0, 8), (5, 8), (7, 10), (16, 10), (16, 18), (0, 18)]), side_face(FOLDER))
    ic.part(ic.rect(0, 11, 16, 18), top_face(FOLDER))
    ic.part(ic.poly([(14, 0), (19, 5), (16, 5), (16, 13), (12, 13), (12, 5), (9, 5)]), GREEN)
    return ic.image


def edit_delete():
    """Delete: a red cross, two thick strokes."""
    ic = Icon(SIZE)
    stroke = ic.line([(4, 4), (15, 15)], width=4) | ic.line([(15, 4), (4, 15)], width=4)
    ic.part(stroke, RED)
    return ic.image


def sheet(ic, x0, y0, x1, y1, lines=True):
    """A sheet of paper with a folded corner and, optionally, lines of text."""
    ic.part(ic.poly([(x0, y0), (x1 - 3, y0), (x1, y0 + 3), (x1, y1), (x0, y1)]), PAPER)
    ic.part(ic.poly([(x1 - 3, y0), (x1, y0 + 3), (x1 - 3, y0 + 3)]), SILVER)
    if lines:
        for y in range(y0 + 5, y1 - 1, 3):
            for x in range(x0 + 2, x1 - 1):
                ic.put(x, y, rgb(96, 96, 96))


def edit_cut():
    """Cut: open scissors, two steel blades crossing above two red handles."""
    ic = Icon(SIZE)
    # Each blade runs from its handle, through the pivot, to the far tip.
    ic.part(ic.poly([(6, 12), (8, 13), (15, 1), (14, 0), (12, 2)]), SILVER)
    ic.part(ic.poly([(13, 12), (11, 13), (4, 1), (5, 0), (7, 2)]), SILVER)
    ic.put(9, 8, BLACK)
    ic.put(10, 8, BLACK)
    for cx in (5, 14):
        ring = ic.disc(cx, 15.5, 4.5) - ic.disc(cx, 15.5, 2.0)
        ic.part(ring, RED, bevel=False)
    return ic.image


def edit_copy():
    """Copy: two sheets, the copy lying over the original."""
    ic = Icon(SIZE)
    sheet(ic, 1, 0, 12, 13)
    sheet(ic, 7, 6, 18, 19)
    return ic.image


def edit_paste():
    """Paste: a clipboard with its steel clip and a sheet on it."""
    ic = Icon(SIZE)
    ic.part(ic.rect(2, 2, 15, 19), WOOD)
    sheet(ic, 7, 6, 18, 19)
    ic.part(ic.rect(5, 0, 12, 4), SILVER)
    return ic.image


def document_properties():
    """Properties: a sheet with lines of text and a tick in its corner."""
    ic = Icon(SIZE)
    ic.part(ic.poly([(3, 0), (12, 0), (16, 4), (16, 19), (3, 19)]), PAPER)
    ic.part(ic.poly([(12, 0), (16, 4), (12, 4)]), SILVER)
    for y in (6, 9, 12):
        for x in range(5, 14):
            ic.put(x, y, rgb(96, 96, 96))
    tick = [(10, 15), (11, 16), (12, 17), (13, 16), (14, 15), (15, 14), (16, 13), (17, 12), (18, 11)]
    for (x, y) in tick:
        ic.put(x, y, GREEN.dark)
        ic.put(x, y + 1, GREEN.outline)
    return ic.image


def view_choose():
    """Views: a window showing a grid of icons."""
    ic = Icon(SIZE)
    ic.part(ic.rect(0, 2, 19, 17), PAPER)
    ic.fill(ic.rect(1, 3, 18, 5), NAVY.dark)
    tiles = [(3, 8, YELLOW), (11, 8, TEAL), (3, 12, RED), (11, 12, FOLDER)]
    for (x, y, ramp) in tiles:
        ic.part(ic.rect(x, y, x + 5, y + 3), ramp)
    return ic.image


ICONS = [
    ("go-previous", go_previous),
    ("go-next", go_next),
    ("go-up", go_up),
    ("edit-cut", edit_cut),
    ("edit-copy", edit_copy),
    ("edit-paste", edit_paste),
    ("edit-delete", edit_delete),
    ("document-properties", document_properties),
    ("view-choose", view_choose),
]


def symbol(name: str) -> str:
    return "toolbar_glyph_" + name.replace("-", "_")


def main() -> None:
    icon_dir = os.path.join(HERE, "icons")
    os.makedirs(icon_dir, exist_ok=True)
    lines = [
        "/* Generated by gen_icons.py. Do not edit by hand. */",
        "",
        f"#define TOOLBAR_GLYPH_SIZE {SIZE}",
        "",
    ]
    for name, draw in ICONS:
        path = os.path.join(icon_dir, name + ".png")
        draw().save(path)
        _, _, pixels = read_icon_png(path)
        values = struct.unpack(f"<{SIZE * SIZE}I", pixels)
        lines.append(f"static const uint32_t {symbol(name)}[{SIZE * SIZE}] = {{")
        for row in range(SIZE):
            chunk = values[row * SIZE:(row + 1) * SIZE]
            lines.append("    " + ", ".join(f"0x{value:08x}" for value in chunk) + ",")
        lines.append("};")
        lines.append("")

    out_path = os.path.join(HERE, "icons.inc")
    with open(out_path, "w", encoding="ascii", newline="\n") as handle:
        handle.write("\n".join(lines))
    print(f"{out_path}: {len(ICONS)} glyphs of {SIZE}x{SIZE}")


main()
