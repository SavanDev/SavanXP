#!/usr/bin/env python3
"""Genera el wallpaper default del repo (diskfs/wallpaper.bmp).

Cielo teal con nubes suaves de value-noise, 640x400 (16:10): el desktop lo
escala nearest 2x al modo tipico de 1280x800. Seed fija para que el asset sea
reproducible; el build NO ejecuta este script (el BMP va commiteado, patron
menu_strip_savanxp.png), asi que PIL solo hace falta para regenerarlo.

Uso:  python tools/GenerateDefaultWallpaper.py
"""

import os
import random

from PIL import Image

WIDTH = 640
HEIGHT = 400
SEED = 0x53585031  # "SXP1"

# Cielo (arriba -> abajo) y color de nube, en clave teal SavanXP.
SKY_TOP = (0, 96, 102)
SKY_BOTTOM = (24, 142, 142)
CLOUD = (188, 224, 222)


def smoothstep(t):
    return t * t * (3.0 - 2.0 * t)


def value_noise(width, height, cell, rng):
    grid_w = width // cell + 2
    grid_h = height // cell + 2
    grid = [[rng.random() for _ in range(grid_w)] for _ in range(grid_h)]
    result = [[0.0] * width for _ in range(height)]
    for y in range(height):
        gy = y / cell
        y0 = int(gy)
        ty = smoothstep(gy - y0)
        row0 = grid[y0]
        row1 = grid[y0 + 1]
        out = result[y]
        for x in range(width):
            gx = x / cell
            x0 = int(gx)
            tx = smoothstep(gx - x0)
            top = row0[x0] * (1.0 - tx) + row0[x0 + 1] * tx
            bottom = row1[x0] * (1.0 - tx) + row1[x0 + 1] * tx
            out[x] = top * (1.0 - ty) + bottom * ty
    return result


def lerp3(a, b, t):
    return tuple(int(round(a[i] + (b[i] - a[i]) * t)) for i in range(3))


def main():
    rng = random.Random(SEED)
    octaves = (
        (value_noise(WIDTH, HEIGHT, 220, rng), 0.60),
        (value_noise(WIDTH, HEIGHT, 96, rng), 0.28),
        (value_noise(WIDTH, HEIGHT, 44, rng), 0.12),
    )

    image = Image.new("RGB", (WIDTH, HEIGHT))
    pixels = image.load()
    for y in range(HEIGHT):
        t = y / (HEIGHT - 1)
        sky = lerp3(SKY_TOP, SKY_BOTTOM, t)
        # Mas nubes arriba, cielo despejado hacia el "horizonte" de abajo.
        fade = 1.0 - smoothstep(min(max((t - 0.25) / 0.6, 0.0), 1.0)) * 0.8
        for x in range(WIDTH):
            value = sum(layer[y][x] * weight for layer, weight in octaves)
            # Solo la parte alta del rango de ruido forma nubes; la transicion
            # ancha suaviza los bordes.
            density = smoothstep(min(max((value - 0.50) / 0.42, 0.0), 1.0)) * 0.9 * fade
            pixels[x, y] = lerp3(sky, CLOUD, density)

    repo_root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    output = os.path.join(repo_root, "diskfs", "wallpaper.bmp")
    image.save(output, "BMP")
    print(f"wallpaper generado: {output} ({os.path.getsize(output)} bytes)")


if __name__ == "__main__":
    main()
