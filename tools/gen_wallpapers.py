#!/usr/bin/env python3
"""Generates the shipped wallpapers (diskfs/wallpapers/*.bmp).

Display Properties lists every BMP in /wallpapers, so adding one here and
running the script is all a new wallpaper takes. Each image is procedural with
a fixed seed, so the assets are reproducible. The build does NOT run this
script (the BMPs are committed), so PIL is only needed to regenerate them.

The large ones are 640x400 (16:10) and meant to be stretched; Grid is a small
tile, meant to be tiled.

Usage:  python tools/gen_wallpapers.py
"""

import math
import os
import random

from PIL import Image

WIDTH = 640
HEIGHT = 400


def smoothstep(t):
    return t * t * (3.0 - 2.0 * t)


def clamp01(t):
    return min(max(t, 0.0), 1.0)


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


def clouds():
    """Teal sky with soft value-noise clouds: the factory wallpaper."""
    sky_top = (0, 96, 102)
    sky_bottom = (24, 142, 142)
    cloud = (188, 224, 222)
    rng = random.Random(0x53585031)  # "SXP1"
    octaves = (
        (value_noise(WIDTH, HEIGHT, 220, rng), 0.60),
        (value_noise(WIDTH, HEIGHT, 96, rng), 0.28),
        (value_noise(WIDTH, HEIGHT, 44, rng), 0.12),
    )

    image = Image.new("RGB", (WIDTH, HEIGHT))
    pixels = image.load()
    for y in range(HEIGHT):
        t = y / (HEIGHT - 1)
        sky = lerp3(sky_top, sky_bottom, t)
        # More clouds at the top, clear sky towards the "horizon" below.
        fade = 1.0 - smoothstep(clamp01((t - 0.25) / 0.6)) * 0.8
        for x in range(WIDTH):
            value = sum(layer[y][x] * weight for layer, weight in octaves)
            # Only the top of the noise range forms clouds; the wide
            # transition softens their edges.
            density = smoothstep(clamp01((value - 0.50) / 0.42)) * 0.9 * fade
            pixels[x, y] = lerp3(sky, cloud, density)
    return image


def gradient():
    """The old built-in gradient, navy to teal in 4-pixel bands. Narrow,
    because it only varies from top to bottom: stretched, it fills any
    screen."""
    width = 16
    image = Image.new("RGB", (width, HEIGHT))
    pixels = image.load()
    for y in range(HEIGHT):
        band = (y // 4) * 4
        t = band / (HEIGHT - 1)
        colour = (0, int(24 + (132 - 24) * t), int(96 + (160 - 96) * t))
        for x in range(width):
            pixels[x, y] = colour
    return image


def grid():
    """The old built-in pattern: a faint grid over dark teal, one cell."""
    cell = 24
    base = (0, 104, 104)
    line = (0, 122, 122)
    image = Image.new("RGB", (cell, cell), base)
    pixels = image.load()
    for i in range(cell):
        pixels[i, 0] = line
        pixels[0, i] = line
    return image


def dunes():
    """Warm dunes under a pale sky, in overlapping sine ridges."""
    rng = random.Random(0x44554E45)  # "DUNE"
    sky_top = (122, 164, 206)
    sky_bottom = (236, 214, 184)
    ridges = []
    for index in range(5):
        base = 0.42 + index * 0.13
        ridges.append((
            base,
            rng.uniform(0.025, 0.05),
            rng.uniform(1.2, 2.6),
            rng.uniform(0.0, math.tau),
            lerp3((226, 170, 110), (160, 92, 48), index / 4),
        ))

    image = Image.new("RGB", (WIDTH, HEIGHT))
    pixels = image.load()
    for x in range(WIDTH):
        u = x / (WIDTH - 1)
        heights = [
            (base + amp * math.sin(u * freq * math.tau + phase)) * HEIGHT
            for base, amp, freq, phase, _ in ridges
        ]
        for y in range(HEIGHT):
            colour = lerp3(sky_top, sky_bottom, smoothstep(y / (HEIGHT - 1)))
            for (_, _, _, _, sand), top in zip(ridges, heights):
                if y >= top:
                    # Lighter at each crest, shading down its face.
                    shade = clamp01((y - top) / (HEIGHT * 0.18))
                    colour = lerp3(lerp3(sand, (255, 230, 190), 0.35), sand, shade)
            pixels[x, y] = colour
    return image


def night():
    """A night sky: deep blue fading up from the horizon, stars and a moon."""
    rng = random.Random(0x4E494748)  # "NIGH"
    top = (6, 10, 34)
    horizon = (34, 58, 112)
    image = Image.new("RGB", (WIDTH, HEIGHT))
    pixels = image.load()
    for y in range(HEIGHT):
        colour = lerp3(top, horizon, smoothstep(y / (HEIGHT - 1)))
        for x in range(WIDTH):
            pixels[x, y] = colour

    for _ in range(260):
        x = rng.randrange(WIDTH)
        y = rng.randrange(int(HEIGHT * 0.85))
        brightness = rng.uniform(0.35, 1.0) * (1.0 - y / HEIGHT * 0.6)
        pixels[x, y] = lerp3(pixels[x, y], (255, 255, 240), brightness)

    moon_x, moon_y, radius = 486, 92, 26
    for y in range(moon_y - radius * 3, moon_y + radius * 3):
        for x in range(moon_x - radius * 3, moon_x + radius * 3):
            distance = math.hypot(x - moon_x, y - moon_y)
            if distance <= radius:
                # A faint terminator on the lower left.
                shade = clamp01((x - moon_x + y - moon_y) / (radius * 2.5) + 0.75)
                pixels[x, y] = lerp3((168, 172, 160), (246, 244, 226), shade)
            elif distance <= radius * 3:
                glow = (1.0 - (distance - radius) / (radius * 2)) ** 2 * 0.35
                pixels[x, y] = lerp3(pixels[x, y], (200, 210, 230), glow)

    # A dark hill line along the bottom.
    for x in range(WIDTH):
        ridge = HEIGHT - 34 - 14 * math.sin(x / WIDTH * math.tau * 1.3 + 0.8)
        for y in range(int(ridge), HEIGHT):
            pixels[x, y] = (4, 8, 20)
    return image


def mountains():
    """Layered mountain ridges at dusk, lighter with distance."""
    rng = random.Random(0x4D544E53)  # "MTNS"
    sky_top = (72, 86, 150)
    sky_bottom = (232, 170, 150)
    layers = 5
    profiles = []
    for index in range(layers):
        noise = value_noise(WIDTH, 1, 90 - index * 12, rng)[0]
        detail = value_noise(WIDTH, 1, 22, rng)[0]
        base = 0.38 + index * 0.12
        profiles.append([
            (base - 0.22 * noise[x] - 0.05 * detail[x]) * HEIGHT for x in range(WIDTH)
        ])

    image = Image.new("RGB", (WIDTH, HEIGHT))
    pixels = image.load()
    for x in range(WIDTH):
        for y in range(HEIGHT):
            colour = lerp3(sky_top, sky_bottom, smoothstep(y / (HEIGHT - 1)))
            for index, profile in enumerate(profiles):
                if y >= profile[x]:
                    depth = index / (layers - 1)
                    colour = lerp3((150, 128, 170), (30, 26, 58), depth)
            pixels[x, y] = colour
    return image


WALLPAPERS = {
    "Clouds": clouds,
    "Gradient": gradient,
    "Grid": grid,
    "Dunes": dunes,
    "Night": night,
    "Mountains": mountains,
}


def main():
    repo_root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    directory = os.path.join(repo_root, "diskfs", "wallpapers")
    os.makedirs(directory, exist_ok=True)
    for name, build in WALLPAPERS.items():
        output = os.path.join(directory, name + ".bmp")
        build().save(output, "BMP")
        print(f"wallpaper: {output} ({os.path.getsize(output)} bytes)")


if __name__ == "__main__":
    main()
