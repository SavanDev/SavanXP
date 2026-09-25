#!/usr/bin/env python3
"""Host-side taskbar smoke actions driven through QMP."""

from __future__ import annotations

import time
from pathlib import Path

from PIL import Image

from qmp_client import QmpClient


class TaskbarSmokeError(RuntimeError):
    pass


FACE = (192, 192, 192)
LIGHT = (255, 255, 255)
SHADOW = (128, 128, 128)


def _wait_screenshot(path: Path, timeout: float = 8.0) -> Image.Image:
    deadline = time.monotonic() + timeout
    last_size = -1
    stable_since = 0.0
    while time.monotonic() < deadline:
        if path.is_file():
            size = path.stat().st_size
            now = time.monotonic()
            if size > 0 and size == last_size and now - stable_since >= 0.2:
                try:
                    with Image.open(path) as image:
                        return image.convert("RGB").copy()
                except OSError:
                    pass
            if size != last_size:
                last_size = size
                stable_since = now
        time.sleep(0.05)
    raise TaskbarSmokeError(f"screenshot did not become stable: {path}")


def _capture(client: QmpClient, output_dir: Path, stem: str) -> Image.Image:
    ppm = output_dir / f"{stem}.ppm"
    png = output_dir / f"{stem}.png"
    ppm.unlink(missing_ok=True)
    png.unlink(missing_ok=True)
    client.screenshot(ppm)
    image = _wait_screenshot(ppm)
    image.save(png)
    ppm.unlink(missing_ok=True)
    return image


def _pixel(image: Image.Image, x: int, y: int) -> tuple[int, int, int]:
    return image.convert("RGB").getpixel((x, y))


def _button(image: Image.Image, index: int) -> tuple[tuple[int, int, int], tuple[int, int, int]]:
    x = 2 if index == 0 else 164
    y = 774
    return _pixel(image, x, y), _pixel(image, x + 159, y + 23)


def _expect_button(image: Image.Image, index: int, active: bool, state: str) -> None:
    top_left, bottom_right = _button(image, index)
    expected = (SHADOW, LIGHT) if active else (LIGHT, SHADOW)
    actual = (top_left, bottom_right)
    if actual != expected:
        raise TaskbarSmokeError(
            f"{state}: button {index} expected {'active' if active else 'raised'}, "
            f"got {actual}"
        )


def _expect_strip(image: Image.Image, state: str) -> None:
    actual = _pixel(image, 1240, 786)
    if actual != FACE:
        raise TaskbarSmokeError(f"{state}: taskbar strip expected {FACE}, got {actual}")


def run_taskbar_actions(qmp_path: Path, output_dir: Path, ready_wait: float) -> list[Path]:
    output_dir.mkdir(parents=True, exist_ok=True)
    if ready_wait:
        time.sleep(ready_wait)

    screenshots: list[Path] = []
    with QmpClient(qmp_path, timeout=5.0) as client:
        first = _capture(client, output_dir, "01-solo-progman")
        screenshots.append(output_dir / "01-solo-progman.png")
        if first.size != (1280, 800):
            raise TaskbarSmokeError(f"unexpected initial screenshot size: {first.size}")
        _expect_strip(first, "initial")
        _expect_button(first, 0, True, "initial")

        # The normal desktop starts on Main, whose first three entries are
        # Shell, Files, and Notepad. Navigate directly instead of relying on
        # tab-group order, which changes when optional ports are installed.
        client.tap("right", pause=0.4)
        client.tap("right", pause=0.4)
        client.tap("ret")
        time.sleep(25.0)
        second = _capture(client, output_dir, "02-dos-ventanas")
        screenshots.append(output_dir / "02-dos-ventanas.png")
        _expect_strip(second, "two windows")
        _expect_button(second, 0, False, "two windows")
        _expect_button(second, 1, True, "two windows")

        for _ in range(22):
            client.relative(-64, -64)
            time.sleep(0.03)
        for dx, dy in [(64, 64), (18, 64)] + [(0, 64)] * 10 + [(0, 18)]:
            client.relative(dx, dy)
        time.sleep(0.5)
        third = _capture(client, output_dir, "03-cursor-sobre-boton")
        screenshots.append(output_dir / "03-cursor-sobre-boton.png")

        client.button("left", True)
        time.sleep(0.25)
        client.button("left", False)
        time.sleep(0.6)
        time.sleep(2.0)
        fourth = _capture(client, output_dir, "04-activado")
        screenshots.append(output_dir / "04-activado.png")
        _expect_strip(fourth, "after first click")
        _expect_button(fourth, 0, True, "after first click")
        _expect_button(fourth, 1, False, "after first click")

        client.button("left", True)
        time.sleep(0.25)
        client.button("left", False)
        time.sleep(2.0)
        fifth = _capture(client, output_dir, "05-minimizado")
        screenshots.append(output_dir / "05-minimizado.png")
        _expect_strip(fifth, "after second click")
        _expect_button(fifth, 0, False, "after second click")

    return screenshots
