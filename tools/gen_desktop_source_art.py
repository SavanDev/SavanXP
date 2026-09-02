#!/usr/bin/env python3
"""Genera el arte fuente del desktop (iconos 16x16/32x32 + banda del menu
Start) como PNGs bajo assets/desktop/.

Reemplaza a tools/GenerateDesktopSourceArt.ps1 (System.Drawing/GDI+, solo
Windows) por Pillow, que corre igual en Windows/Linux/macOS. build.ps1 lo
invoca en cada build antes de convertir esos PNG a headers C.

Uso:  python tools/gen_desktop_source_art.py [--project-root DIR]
"""

import argparse
import os

from PIL import Image, ImageDraw


def new_canvas(width, height):
    return Image.new("RGBA", (width, height), (0, 0, 0, 0))


def set_pixel_safe(image, x, y, color):
    if 0 <= x < image.width and 0 <= y < image.height:
        image.putpixel((x, y), color)


def fill_rect(draw, x, y, width, height, color):
    draw.rectangle([x, y, x + width - 1, y + height - 1], fill=color)


def stroke_rect(draw, x, y, width, height, color):
    draw.rectangle([x, y, x + width - 1, y + height - 1], outline=color)


def scale_nearest(image, factor):
    return image.resize((image.width * factor, image.height * factor), Image.NEAREST)


def save_png(image, path):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    image.save(path, "PNG")


def new_desktop_icon_16():
    ink = (41, 53, 60, 255)
    screen = (19, 139, 145, 255)
    screen_glow = (79, 216, 220, 255)
    gold = (232, 189, 78, 255)
    base = (198, 212, 220, 255)
    shadow = (113, 128, 139, 255)
    bmp = new_canvas(16, 16)
    draw = ImageDraw.Draw(bmp)

    stroke_rect(draw, 1, 2, 14, 10, ink)
    fill_rect(draw, 2, 3, 12, 8, screen)
    fill_rect(draw, 3, 4, 10, 1, screen_glow)
    fill_rect(draw, 5, 12, 6, 1, base)
    fill_rect(draw, 6, 13, 4, 1, shadow)
    fill_rect(draw, 4, 14, 8, 1, base)
    fill_rect(draw, 4, 5, 2, 2, gold)
    fill_rect(draw, 7, 5, 3, 2, gold)
    fill_rect(draw, 11, 5, 1, 4, gold)
    fill_rect(draw, 4, 8, 6, 1, gold)
    return bmp


def new_terminal_icon_16():
    frame = (195, 201, 206, 255)
    header = (27, 114, 106, 255)
    screen = (17, 22, 29, 255)
    green = (104, 229, 148, 255)
    white = (229, 240, 236, 255)
    bmp = new_canvas(16, 16)
    draw = ImageDraw.Draw(bmp)

    fill_rect(draw, 1, 2, 14, 12, frame)
    fill_rect(draw, 2, 3, 12, 2, header)
    fill_rect(draw, 2, 5, 12, 8, screen)
    set_pixel_safe(bmp, 3, 4, white)
    set_pixel_safe(bmp, 4, 4, white)
    fill_rect(draw, 4, 7, 1, 3, green)
    set_pixel_safe(bmp, 5, 8, green)
    set_pixel_safe(bmp, 6, 9, green)
    fill_rect(draw, 7, 10, 4, 1, white)
    return bmp


def new_gfx_demo_icon_16():
    frame = (48, 58, 74, 255)
    panel = (27, 32, 40, 255)
    cyan = (48, 214, 217, 255)
    magenta = (220, 72, 201, 255)
    gold = (236, 181, 63, 255)
    green = (90, 220, 114, 255)
    bmp = new_canvas(16, 16)
    draw = ImageDraw.Draw(bmp)

    fill_rect(draw, 1, 2, 14, 12, panel)
    stroke_rect(draw, 1, 2, 14, 12, frame)
    for offset in range(6):
        set_pixel_safe(bmp, 3 + offset, 10 - offset, cyan)
        set_pixel_safe(bmp, 4 + offset, 10 - offset, cyan)
        set_pixel_safe(bmp, 5 + offset, 4 + offset, magenta)
        set_pixel_safe(bmp, 6 + offset, 4 + offset, magenta)
    fill_rect(draw, 10, 8, 3, 3, gold)
    fill_rect(draw, 3, 11, 10, 1, green)
    return bmp


def new_keyboard_icon_16():
    body = (188, 195, 203, 255)
    outline = (74, 86, 96, 255)
    key = (244, 247, 250, 255)
    accent = (101, 197, 128, 255)
    shadow = (131, 145, 156, 255)
    bmp = new_canvas(16, 16)
    draw = ImageDraw.Draw(bmp)

    fill_rect(draw, 1, 4, 14, 8, body)
    stroke_rect(draw, 1, 4, 14, 8, outline)
    fill_rect(draw, 2, 5, 12, 1, accent)
    for row in range(3):
        for column in range(5):
            fill_rect(draw, 2 + (column * 2), 7 + row, 1, 1, key)
    fill_rect(draw, 4, 10, 8, 1, shadow)
    return bmp


def new_mouse_icon_16():
    outline = (99, 109, 119, 255)
    shell = (239, 243, 245, 255)
    shadow = (207, 214, 219, 255)
    wheel = (61, 181, 176, 255)
    bmp = new_canvas(16, 16)
    draw = ImageDraw.Draw(bmp)

    fill_rect(draw, 4, 2, 8, 11, shell)
    stroke_rect(draw, 4, 2, 8, 11, outline)
    fill_rect(draw, 5, 3, 6, 8, shell)
    fill_rect(draw, 7, 4, 2, 3, wheel)
    fill_rect(draw, 7, 7, 2, 1, outline)
    fill_rect(draw, 5, 11, 6, 1, shadow)
    set_pixel_safe(bmp, 5, 13, outline)
    set_pixel_safe(bmp, 10, 13, outline)
    return bmp


def new_notepad_icon_16():
    paper = (247, 244, 232, 255)
    shadow = (211, 205, 186, 255)
    outline = (94, 84, 58, 255)
    rule = (168, 186, 214, 255)
    margin = (214, 108, 96, 255)
    pencil_wood = (232, 189, 78, 255)
    pencil_tip = (120, 100, 60, 255)
    pencil_lead = (58, 48, 34, 255)
    bmp = new_canvas(16, 16)
    draw = ImageDraw.Draw(bmp)

    fill_rect(draw, 2, 1, 9, 13, shadow)
    fill_rect(draw, 2, 1, 8, 13, paper)
    stroke_rect(draw, 2, 1, 8, 13, outline)
    fill_rect(draw, 4, 1, 1, 13, margin)
    for y in (4, 6, 8, 10, 12):
        draw.line([(5, y), (9, y)], fill=rule)

    draw.line([(10, 13), (14, 3)], fill=pencil_wood, width=2)
    draw.line([(13, 4), (14, 3), (14, 5)], fill=pencil_tip)
    set_pixel_safe(bmp, 10, 13, pencil_lead)

    return bmp


def write_icon_set(base_path, name, factory):
    icon16 = factory()
    icon32 = scale_nearest(icon16, 2)
    save_png(icon16, os.path.join(base_path, "16x16", name))
    save_png(icon32, os.path.join(base_path, "32x32", name))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--project-root")
    args = parser.parse_args()

    project_root = args.project_root or os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

    asset_root = os.path.join(project_root, "assets", "desktop", "icons")
    os.makedirs(os.path.join(asset_root, "16x16"), exist_ok=True)
    os.makedirs(os.path.join(asset_root, "32x32"), exist_ok=True)

    write_icon_set(asset_root, "desktop.png", new_desktop_icon_16)
    write_icon_set(asset_root, "app-terminal.png", new_terminal_icon_16)
    # El de Doom (antes "app-spider.png") se fue del set del sistema: vive
    # ahora en sdk/doomgeneric/icon.png y se estampa via icon_file= en
    # doomgeneric.sxres. Doom se construye con un build aparte, asi que no
    # tiene sentido que su icono siga horneado en el binario del WM.
    write_icon_set(asset_root, "app-libgfx-demo.png", new_gfx_demo_icon_16)
    write_icon_set(asset_root, "app-keyboard-settings.png", new_keyboard_icon_16)
    write_icon_set(asset_root, "app-mouse.png", new_mouse_icon_16)
    write_icon_set(asset_root, "app-notepad.png", new_notepad_icon_16)


if __name__ == "__main__":
    main()
