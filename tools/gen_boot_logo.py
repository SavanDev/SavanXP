#!/usr/bin/env python3
"""Convierte assets/brand/logo.png en build/generated/boot_logo.h.

El boot screen corre en el kernel, antes de que exista nada parecido a un
decodificador de imagenes, asi que el logo viaja horneado: el PNG se compone
sobre el fondo del splash (sin alpha en tiempo de arranque), se reescala y se
cuantiza a una paleta de 256 colores. El kernel solo hace lookup + blit.

El nombre del SO se hornea aparte como mascara de cobertura de 8 bits, rendereada
con la Noto Sans del escritorio: el splash necesita un wordmark suavizado y la
fuente de consola es un bitmap de 8x16. El nombre sale de
include/shared/version.h para que siga habiendo una sola fuente de verdad.

Uso:  python tools/gen_boot_logo.py --project-root DIR --output boot_logo.h
"""

import argparse
import os
import re

from PIL import Image, ImageDraw, ImageFont

# Lado del logo horneado, en pixeles de framebuffer. 224 ocupa ~28% del alto en
# 1280x800 (la resolucion que pide limine.conf) y sigue entrando en 640x480.
LOGO_SIZE = 224
# Colores de la paleta. El logo es un degrade suave: con dithering, 256 alcanzan
# para que no se vean bandas.
LOGO_COLORS = 256
# Alto nominal del wordmark. Noto Sans a 60 px da un "SavanXP" de ~235 px de
# ancho, proporcionado con un logo de 224.
WORDMARK_PX = 60
# El splash es negro puro, como el de XP: el logo se compone sobre ese color.
BACKGROUND = (0, 0, 0)


def read_system_name(project_root):
    version_header = os.path.join(project_root, "include", "shared", "version.h")
    with open(version_header, "r", encoding="utf-8") as handle:
        match = re.search(r'#define\s+SAVANXP_SYSTEM_NAME\s+"([^"]+)"', handle.read())
    if match is None:
        raise SystemExit(f"No se encontro SAVANXP_SYSTEM_NAME en {version_header}")
    return match.group(1)


def render_logo(path):
    source = Image.open(path).convert("RGBA")
    flattened = Image.new("RGB", source.size, BACKGROUND)
    flattened.paste(source, (0, 0), source)
    scaled = flattened.resize((LOGO_SIZE, LOGO_SIZE), Image.LANCZOS)
    quantized = scaled.quantize(colors=LOGO_COLORS, method=Image.MEDIANCUT, dither=Image.FLOYDSTEINBERG)

    raw_palette = quantized.getpalette()[: LOGO_COLORS * 3]
    palette = []
    for index in range(LOGO_COLORS):
        red, green, blue = raw_palette[index * 3 : index * 3 + 3]
        palette.append((red << 16) | (green << 8) | blue)

    return {"width": LOGO_SIZE, "height": LOGO_SIZE, "palette": palette, "indices": list(quantized.tobytes())}


def render_wordmark(font_path, text):
    font = ImageFont.truetype(font_path, WORDMARK_PX)
    left, top, right, bottom = font.getbbox(text)
    canvas = Image.new("L", (right - left + 2, bottom - top + 2), 0)
    ImageDraw.Draw(canvas).text((1 - left, 1 - top), text, font=font, fill=255)
    canvas = canvas.crop(canvas.getbbox())
    return {"width": canvas.width, "height": canvas.height, "coverage": list(canvas.tobytes())}


def write_byte_array(lines, declaration, values, per_line=16, formatter="0x{:02X}u"):
    lines.append(declaration + " = {")
    for start in range(0, len(values), per_line):
        chunk = values[start : start + per_line]
        suffix = "" if start + per_line >= len(values) else ","
        lines.append("    " + ", ".join(formatter.format(value) for value in chunk) + suffix)
    lines.append("};")
    lines.append("")


def write_header(path, logo, wordmark, name):
    lines = [
        "// Generado por tools/gen_boot_logo.py. No editar a mano.",
        f"// Fuente: assets/brand/logo.png, wordmark \"{name}\".",
        "#ifndef SAVANXP_BOOT_LOGO_H",
        "#define SAVANXP_BOOT_LOGO_H",
        "",
        f"#define SX_BOOT_LOGO_W {logo['width']}",
        f"#define SX_BOOT_LOGO_H {logo['height']}",
        f"#define SX_BOOT_LOGO_COLORS {len(logo['palette'])}",
        f"#define SX_BOOT_WORDMARK_W {wordmark['width']}",
        f"#define SX_BOOT_WORDMARK_H {wordmark['height']}",
        "",
    ]

    write_byte_array(
        lines,
        f"static const unsigned int k_boot_logo_palette[SX_BOOT_LOGO_COLORS]",
        logo["palette"],
        per_line=8,
        formatter="0x{:06X}u",
    )
    write_byte_array(
        lines,
        "static const unsigned char k_boot_logo_pixels[SX_BOOT_LOGO_W * SX_BOOT_LOGO_H]",
        logo["indices"],
    )
    write_byte_array(
        lines,
        "static const unsigned char k_boot_wordmark_alpha[SX_BOOT_WORDMARK_W * SX_BOOT_WORDMARK_H]",
        wordmark["coverage"],
    )

    lines.append("#endif")

    os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
    with open(path, "w", encoding="ascii", newline="\n") as handle:
        handle.write("\n".join(lines) + "\n")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--project-root")
    parser.add_argument("--output", required=True)
    parser.add_argument("--name", help="Texto del wordmark (por default, SAVANXP_SYSTEM_NAME)")
    args = parser.parse_args()

    project_root = args.project_root or os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    logo_path = os.path.join(project_root, "assets", "brand", "logo.png")
    font_path = os.path.join(project_root, "assets", "desktop", "fonts", "NotoSans-Regular.ttf")
    if not os.path.isfile(logo_path):
        raise SystemExit(f"No se encontro el logo: {logo_path}")

    name = args.name or read_system_name(project_root)
    write_header(args.output, render_logo(logo_path), render_wordmark(font_path, name), name)


if __name__ == "__main__":
    main()
