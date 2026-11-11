#!/usr/bin/env python3
"""Packs the Start menu art into build/generated/start_logo.h.

Pillow turns the versioned PNGs into a C header with the same format as
tools/gen_desktop_icon_assets.py (struct savanxp_embedded_bitmap_asset, ARGB
pixels): the Start button logo from assets/brand/start_logo.png, the Shut Down
and Restart icons from assets/desktop/icons/16x16, the 32x32 computer the Shut
Down dialog shows, and the taskbar speaker at its four levels from the media
player's icons. The normal build only packages those PNGs: it
never creates or replaces the source art.

Usage: python tools/gen_start_logo.py --project-root DIR --output start_logo.h
"""

import argparse
import os

from PIL import Image

ASSETS = [
    ("k_start_logo", ("assets", "brand", "start_logo.png")),
    ("k_shutdown_icon", ("assets", "desktop", "icons", "16x16", "shutdown.png")),
    ("k_restart_icon", ("assets", "desktop", "icons", "16x16", "restart.png")),
    ("k_shutdown_icon_large", ("assets", "desktop", "icons", "32x32", "shutdown.png")),
    # Altavoz de la taskbar en sus cuatro niveles, del set del reproductor
    # (nombres de la Icon Naming Specification de freedesktop): el dibujo es
    # el mismo en los cuatro lados y asi no hay dos artes que diverjan.
    ("k_volume_high", ("subsystems", "posix", "userland", "mediaplayer", "icons", "audio-volume-high.png")),
    ("k_volume_medium", ("subsystems", "posix", "userland", "mediaplayer", "icons", "audio-volume-medium.png")),
    ("k_volume_low", ("subsystems", "posix", "userland", "mediaplayer", "icons", "audio-volume-low.png")),
    ("k_volume_muted", ("subsystems", "posix", "userland", "mediaplayer", "icons", "audio-volume-muted.png")),
]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--project-root")
    parser.add_argument("--output", required=True)
    args = parser.parse_args()

    project_root = args.project_root or os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    lines = [
        "#ifndef SAVANXP_START_LOGO_H",
        "#define SAVANXP_START_LOGO_H",
        "",
        "#include <stdint.h>",
        "",
        "struct savanxp_embedded_bitmap_asset {",
        "    uint32_t width;",
        "    uint32_t height;",
        "    const uint32_t* pixels;",
        "};",
        "",
    ]
    for symbol, parts in ASSETS:
        path = os.path.normpath(os.path.join(project_root, *parts))
        if not os.path.isfile(path):
            raise SystemExit(f"No se encontro el asset bitmap requerido: {path}")
        image = Image.open(path).convert("RGBA")
        width, height = image.size
        lines.append(f"static const uint32_t {symbol}_pixels[{width * height}] = {{")
        for row in range(height):
            values = []
            for column in range(width):
                r, g, b, a = image.getpixel((column, row))
                values.append(f"0x{(a << 24) | (r << 16) | (g << 8) | b:08X}u")
            suffix = "" if row == height - 1 else ","
            lines.append(f"    {', '.join(values)}{suffix}")
        lines.append("};")
        lines.append(
            f"static const struct savanxp_embedded_bitmap_asset {symbol} = "
            f"{{ {width}u, {height}u, {symbol}_pixels }};"
        )
        lines.append("")
    lines.append("#endif")

    os.makedirs(os.path.dirname(os.path.abspath(args.output)), exist_ok=True)
    with open(args.output, "w", encoding="ascii", newline="\n") as handle:
        handle.write("\n".join(lines) + "\n")


if __name__ == "__main__":
    main()
