"""Dibuja los glifos de los controles del reproductor y genera icons.inc.

El arte es propio y esta en este archivo: se probo el set Tango 0.8.90 (el del
catalogo de tipos de archivo) y sus iconos de transporte son grises claros con
contorno, pensados para fondo blanco; sobre la cara gris de un boton un Play
habilitado se lee igual que uno deshabilitado. Estos van en la paleta de los
iconos de app del sistema (assets/desktop/icons/32x32): negro plano para los
glifos, como el texto y las flechas de un scrollbar, y ocre y rojo para los dos
que necesitan color.

Escribe icons/<nombre>.png (para mirarlos) y icons.inc: un arreglo 0xAARRGGBB
por icono, el formato de SX_PIXEL_FORMAT_BGRA8888 con alfa sin premultiplicar,
con la misma conversion que gen_sxe_resources.py. Van embebidos y no como
.sxicon en el disco: son diez de 16x16, ~4 KiB contra 6 MiB de texto, y no
sirven fuera de este programa. Ambos se versionan; se regeneran con:

    python ports/ffmpeg/overlay/mediaplayer/gen_icons.py

Los nombres siguen la Icon Naming Specification de freedesktop, igual que
diskfs/mimeicon.ini, para poder cambiar de origen sin tocar el programa.
"""

import os
import struct
import sys

from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "..", "..", "..", "tools"))

from gen_sxe_resources import read_icon_png  # noqa: E402

SIZE = 16

INK = (0, 0, 0, 255)
OUTLINE = (58, 48, 34, 255)
OCHRE = (232, 189, 78, 255)
OCHRE_DARK = (168, 120, 40, 255)
PAPER = (247, 244, 232, 255)
RED = (190, 40, 32, 255)


def canvas():
    return Image.new("RGBA", (SIZE, SIZE), (0, 0, 0, 0))


def rect(image, x0, y0, x1, y1, colour):
    for y in range(y0, y1 + 1):
        for x in range(x0, x1 + 1):
            image.putpixel((x, y), colour)


def triangle(image, x_base, y_top, rows, direction):
    """Triangulo con la base vertical en x_base y la punta hacia `direction`
    (+1 derecha, -1 izquierda). El renglon del medio es el mas largo."""
    for row in range(rows):
        length = min(row, rows - 1 - row) + 1
        for step in range(length):
            image.putpixel((x_base + direction * step, y_top + row), INK)


def play():
    image = canvas()
    triangle(image, 5, 2, 11, +1)
    return image


def pause():
    image = canvas()
    rect(image, 4, 3, 6, 12, INK)
    rect(image, 9, 3, 11, 12, INK)
    return image


def stop():
    image = canvas()
    rect(image, 4, 4, 11, 11, INK)
    return image


def seek_backward():
    image = canvas()
    triangle(image, 7, 4, 9, -1)
    triangle(image, 12, 4, 9, -1)
    return image


def seek_forward():
    image = canvas()
    triangle(image, 3, 4, 9, +1)
    triangle(image, 8, 4, 9, +1)
    return image


def document_open():
    image = canvas()
    # Carpeta: solapa arriba a la izquierda, cuerpo, y la tapa abierta mas
    # clara adelante.
    rect(image, 1, 3, 6, 5, OUTLINE)
    rect(image, 2, 4, 5, 5, OCHRE_DARK)
    rect(image, 1, 5, 14, 13, OUTLINE)
    rect(image, 2, 6, 13, 12, OCHRE_DARK)
    rect(image, 3, 7, 12, 7, PAPER)
    for row, (x0, x1) in enumerate([(4, 15), (4, 15), (3, 14), (3, 14), (2, 13), (2, 13)]):
        y = 8 + row
        rect(image, x0, y, x1, y, OUTLINE)
        if row not in (0, 5):
            rect(image, x0 + 1, y, x1 - 1, y, OCHRE)
    return image


def speaker(waves, muted=False):
    image = canvas()
    rect(image, 1, 6, 3, 9, INK)
    for column, (y0, y1) in enumerate([(5, 10), (4, 11), (3, 12), (2, 13)]):
        rect(image, 4 + column, y0, 4 + column, y1, INK)
    if muted:
        for offset in range(5):
            for x, y in ((10 + offset, 5 + offset), (14 - offset, 5 + offset)):
                image.putpixel((x, y), RED)
                image.putpixel((x, y + 1), RED)
        return image
    arcs = [
        [(10, 6), (10, 7), (10, 8), (10, 9), (9, 5), (9, 10)],
        [(12, 5), (12, 6), (12, 7), (12, 8), (12, 9), (12, 10), (11, 4), (11, 11)],
        [(14, 4), (14, 5), (14, 6), (14, 7), (14, 8), (14, 9), (14, 10), (14, 11), (13, 3), (13, 12)],
    ]
    for arc in arcs[:waves]:
        for x, y in arc:
            image.putpixel((x, y), INK)
    return image


ICONS = [
    ("document-open", document_open),
    ("media-playback-start", play),
    ("media-playback-pause", pause),
    ("media-playback-stop", stop),
    ("media-seek-backward", seek_backward),
    ("media-seek-forward", seek_forward),
    ("audio-volume-high", lambda: speaker(3)),
    ("audio-volume-medium", lambda: speaker(2)),
    ("audio-volume-low", lambda: speaker(1)),
    ("audio-volume-muted", lambda: speaker(0, muted=True)),
]


def symbol(name: str) -> str:
    return "mp_icon_" + name.replace("-", "_")


def main() -> None:
    icon_dir = os.path.join(HERE, "icons")
    os.makedirs(icon_dir, exist_ok=True)
    lines = [
        "/* Generado por gen_icons.py. No editar a mano. */",
        "",
        f"#define MP_ICON_SIZE {SIZE}",
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
    print(f"{out_path}: {len(ICONS)} iconos de {SIZE}x{SIZE}")


main()
