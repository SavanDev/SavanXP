#!/usr/bin/env python3
"""Convierte los PNG de assets/desktop/cursors/ a build/generated/cursor_asset.h.

Reemplaza a tools/GenerateCursorAsset.ps1 (System.Drawing/GDI+, solo Windows)
por Pillow, que corre igual en Windows/Linux/macOS. build.ps1 lo invoca en
cada build.

Uso:  python tools/gen_cursor_asset.py --project-root DIR --output cursor_asset.h
"""

import argparse
import os

from PIL import Image

# El orden coincide con enum savanxp_cursor_shape
# (subsystems/posix/sdk/v1/include/savanxp/syscall.h). Los hotspots se
# eligieron a mano por forma -- los PNG no traen metadata de hotspot.
MANIFEST = [
    {"shape": "SAVANXP_CURSOR_ARROW", "symbol": "k_desktop_cursor_arrow", "file": "arrow.png", "hotspot_x": 3, "hotspot_y": 1},
    {"shape": "SAVANXP_CURSOR_WAIT", "symbol": "k_desktop_cursor_wait", "file": "wait.png", "hotspot_x": 9, "hotspot_y": 9},
    {"shape": "SAVANXP_CURSOR_TEXT", "symbol": "k_desktop_cursor_text", "file": "text.png", "hotspot_x": 3, "hotspot_y": 8},
    {"shape": "SAVANXP_CURSOR_MOVE", "symbol": "k_desktop_cursor_move", "file": "move.png", "hotspot_x": 9, "hotspot_y": 9},
    {"shape": "SAVANXP_CURSOR_RESIZE_H", "symbol": "k_desktop_cursor_resize_h", "file": "resize_h.png", "hotspot_x": 9, "hotspot_y": 5},
    {"shape": "SAVANXP_CURSOR_RESIZE_V", "symbol": "k_desktop_cursor_resize_v", "file": "resize_v.png", "hotspot_x": 5, "hotspot_y": 9},
    {"shape": "SAVANXP_CURSOR_UNAVAILABLE", "symbol": "k_desktop_cursor_unavailable", "file": "unavailable.png", "hotspot_x": 9, "hotspot_y": 9},
    {"shape": "SAVANXP_CURSOR_LINK", "symbol": "k_desktop_cursor_link", "file": "link.png", "hotspot_x": 5, "hotspot_y": 2},
]


def get_png_cursor_pixels(path):
    image = Image.open(path).convert("RGBA")
    width, height = image.size
    pixels = []
    for y in range(height):
        for x in range(width):
            r, g, b, a = image.getpixel((x, y))
            pixels.append((a << 24) | (r << 16) | (g << 8) | b)
    return {"width": width, "height": height, "pixels": pixels}


def get_fallback_cursor_pixels():
    rows = [
        "#",
        "##",
        "#.#",
        "#..#",
        "#...#",
        "#....#",
        "#.....#",
        "#......#",
        "#.......#",
        "#........#",
        "#.........#",
        "#....#######",
        "#..##...#",
        "###.#...#",
        "   #...#",
        "   #..#",
        "    ##",
        "    #",
    ]
    width = 13
    height = len(rows)
    pixels = []
    for row in rows:
        for column in range(width):
            character = row[column] if column < len(row) else " "
            if character == "#":
                pixels.append(0xFF000000)
            elif character == ".":
                pixels.append(0xFFFFFFFF)
            else:
                pixels.append(0)
    return {"width": width, "height": height, "pixels": pixels}


def write_cursor_pixel_array(lines, symbol_name, cursor):
    width, height = cursor["width"], cursor["height"]
    pixels = cursor["pixels"]
    lines.append(f"static const unsigned int {symbol_name}[{width * height}] = {{")
    for row in range(height):
        values = [f"0x{pixels[row * width + column]:08X}u" for column in range(width)]
        suffix = "" if row == height - 1 else ","
        lines.append(f"    {', '.join(values)}{suffix}")
    lines.append("};")
    lines.append("")


def write_cursor_header(path, entries):
    lines = ["#ifndef SAVANXP_CURSOR_ASSET_H", "#define SAVANXP_CURSOR_ASSET_H", ""]

    for entry in entries:
        write_cursor_pixel_array(lines, f"{entry['symbol']}_pixels", entry["cursor"])

    lines += [
        "struct desktop_cursor_asset {",
        "    int width;",
        "    int height;",
        "    int hotspot_x;",
        "    int hotspot_y;",
        "    const unsigned int *pixels;",
        "};",
        "",
        "static const struct desktop_cursor_asset k_desktop_cursor_assets[SAVANXP_CURSOR_SHAPE_COUNT] = {",
    ]
    for entry in entries:
        cursor = entry["cursor"]
        lines.append(
            f"    [{entry['shape']}] = {{ {cursor['width']}, {cursor['height']}, "
            f"{entry['hotspot_x']}, {entry['hotspot_y']}, {entry['symbol']}_pixels }},"
        )
    lines += ["};", "", "#endif"]

    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="ascii", newline="\n") as handle:
        handle.write("\n".join(lines) + "\n")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--project-root")
    parser.add_argument("--output", required=True)
    args = parser.parse_args()

    project_root = args.project_root or os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    asset_root = os.path.join(project_root, "assets", "desktop", "cursors")

    entries = []
    for item in MANIFEST:
        source_path = os.path.join(asset_root, item["file"])
        if os.path.isfile(source_path):
            cursor = get_png_cursor_pixels(source_path)
        elif item["shape"] == "SAVANXP_CURSOR_ARROW":
            cursor = get_fallback_cursor_pixels()
        else:
            raise SystemExit(f"No se encontro el asset de cursor requerido: {source_path}")

        entries.append({
            "shape": item["shape"],
            "symbol": item["symbol"],
            "hotspot_x": item["hotspot_x"],
            "hotspot_y": item["hotspot_y"],
            "cursor": cursor,
        })

    write_cursor_header(args.output, entries)


if __name__ == "__main__":
    main()
