#!/usr/bin/env python3
"""Convierte assets/brand/logo.png en build/generated/brand_logo.h.

El logo del sistema, del tamano que usa una ventana del escritorio (no el del
splash de arranque, que es otro tamano y otra tecnica: ver gen_boot_logo.py).
Viaja horneado porque el userland tampoco tiene decodificador de PNG -- el
unico formato que se lee en vivo es el BMP del fondo de escritorio --, y
porque un logo que forma parte del sistema no tiene por que depender de un
archivo que pueda faltar.

Sale como BGRA de 32 bits con el alfa REAL del PNG, no premultiplicado ni
compuesto contra un fondo: la ventana que lo dibuja elige sobre que lo compone.

Uso:  python tools/gen_brand_logo.py --project-root DIR --output brand_logo.h
"""

import argparse
import os

from PIL import Image

# Lado del logo horneado. 96 es el que pide la ventana de propiedades del
# sistema, que lo apoya a la izquierda de tres bloques de texto: mas chico no
# sostiene la columna y mas grande empuja el texto fuera de la grilla.
LOGO_SIZE = 96


def render_logo(path):
    source = Image.open(path).convert("RGBA")
    scaled = source.resize((LOGO_SIZE, LOGO_SIZE), Image.LANCZOS)
    pixels = []
    for alpha_pixel in scaled.getdata():
        red, green, blue, alpha = alpha_pixel
        pixels.append((alpha << 24) | (red << 16) | (green << 8) | blue)
    return {"width": scaled.width, "height": scaled.height, "pixels": pixels}


def write_header(path, logo):
    lines = [
        "// Generado por tools/gen_brand_logo.py. No editar a mano.",
        "// Fuente: assets/brand/logo.png.",
        "#ifndef SAVANXP_BRAND_LOGO_H",
        "#define SAVANXP_BRAND_LOGO_H",
        "",
        f"#define SX_BRAND_LOGO_W {logo['width']}",
        f"#define SX_BRAND_LOGO_H {logo['height']}",
        "",
        "/* ARGB de 32 bits con alfa real, en el orden que espera sx_bitmap_wrap",
        " * con SX_PIXEL_FORMAT_BGRA8888. */",
        "static const unsigned int k_brand_logo_pixels[SX_BRAND_LOGO_W * SX_BRAND_LOGO_H] = {",
    ]

    values = logo["pixels"]
    per_line = 6
    for start in range(0, len(values), per_line):
        chunk = values[start : start + per_line]
        suffix = "" if start + per_line >= len(values) else ","
        lines.append("    " + ", ".join("0x{:08X}u".format(value) for value in chunk) + suffix)

    lines.append("};")
    lines.append("")
    lines.append("#endif")

    os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
    with open(path, "w", encoding="ascii", newline="\n") as handle:
        handle.write("\n".join(lines) + "\n")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--project-root")
    parser.add_argument("--output", required=True)
    args = parser.parse_args()

    project_root = args.project_root or os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    logo_path = os.path.join(project_root, "assets", "brand", "logo.png")
    if not os.path.isfile(logo_path):
        raise SystemExit(f"No se encontro el logo: {logo_path}")

    write_header(args.output, render_logo(logo_path))


if __name__ == "__main__":
    main()
