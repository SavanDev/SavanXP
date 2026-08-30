#!/usr/bin/env python3
"""Convierte los PNG de iconos/menu strip del desktop a
build/generated/desktop_icon_assets.h.

Reemplaza a tools/GenerateDesktopIconAssets.ps1 (System.Drawing/GDI+, solo
Windows) por Pillow, que corre igual en Windows/Linux/macOS. build.ps1 lo
invoca en cada build, despues de gen_desktop_source_art.py.

Uso:  python tools/gen_desktop_icon_assets.py --project-root DIR --output desktop_icon_assets.h
"""

import argparse
import os

from PIL import Image

MANIFEST = [
    {"symbol": "k_desktop_icon_desktop_16", "relative_path": "16x16/desktop.png"},
    {"symbol": "k_desktop_icon_shell_16", "relative_path": "16x16/app-terminal.png"},
    {"symbol": "k_desktop_icon_doom_16", "relative_path": "16x16/app-spider.png"},
    {"symbol": "k_desktop_icon_gfx_demo_16", "relative_path": "16x16/app-libgfx-demo.png"},
    {"symbol": "k_desktop_icon_key_test_16", "relative_path": "16x16/app-keyboard-settings.png"},
    {"symbol": "k_desktop_icon_mouse_test_16", "relative_path": "16x16/app-mouse.png"},
    {"symbol": "k_desktop_icon_notepad_16", "relative_path": "16x16/app-notepad.png"},
    {"symbol": "k_desktop_icon_desktop_32", "relative_path": "32x32/desktop.png"},
    {"symbol": "k_desktop_icon_shell_32", "relative_path": "32x32/app-terminal.png"},
    {"symbol": "k_desktop_icon_doom_32", "relative_path": "32x32/app-spider.png"},
    {"symbol": "k_desktop_icon_gfx_demo_32", "relative_path": "32x32/app-libgfx-demo.png"},
    {"symbol": "k_desktop_icon_key_test_32", "relative_path": "32x32/app-keyboard-settings.png"},
    {"symbol": "k_desktop_icon_mouse_test_32", "relative_path": "32x32/app-mouse.png"},
    {"symbol": "k_desktop_icon_notepad_32", "relative_path": "32x32/app-notepad.png"},
]


def get_png_bitmap_pixels(path):
    if not os.path.isfile(path):
        raise SystemExit(f"No se encontro el asset bitmap requerido: {path}")

    image = Image.open(path).convert("RGBA")
    width, height = image.size
    pixels = []
    for y in range(height):
        for x in range(width):
            r, g, b, a = image.getpixel((x, y))
            pixels.append((a << 24) | (r << 16) | (g << 8) | b)
    return {"width": width, "height": height, "pixels": pixels}


def write_embedded_bitmap(lines, symbol_name, bitmap):
    width, height = bitmap["width"], bitmap["height"]
    pixels = bitmap["pixels"]
    lines.append(f"static const uint32_t {symbol_name}_pixels[{width * height}] = {{")
    for row in range(height):
        values = [f"0x{pixels[row * width + column]:08X}u" for column in range(width)]
        suffix = "" if row == height - 1 else ","
        lines.append(f"    {', '.join(values)}{suffix}")
    lines.append("};")
    lines.append(
        f"static const struct savanxp_embedded_bitmap_asset {symbol_name} = "
        f"{{ {width}u, {height}u, {symbol_name}_pixels }};"
    )
    lines.append("")


def write_desktop_icon_header(path, entries):
    lines = [
        "#ifndef SAVANXP_DESKTOP_ICON_ASSETS_H",
        "#define SAVANXP_DESKTOP_ICON_ASSETS_H",
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

    for entry in entries:
        write_embedded_bitmap(lines, entry["symbol"], entry["bitmap"])

    lines.append("#endif")

    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="ascii", newline="\n") as handle:
        handle.write("\n".join(lines) + "\n")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--project-root")
    parser.add_argument("--output", required=True)
    args = parser.parse_args()

    project_root = args.project_root or os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    asset_root = os.path.join(project_root, "assets", "desktop", "icons")

    entries = []
    for item in MANIFEST:
        path = os.path.normpath(os.path.join(asset_root, *item["relative_path"].split("/")))
        entries.append({"symbol": item["symbol"], "bitmap": get_png_bitmap_pixels(path)})

    write_desktop_icon_header(args.output, entries)


if __name__ == "__main__":
    main()
