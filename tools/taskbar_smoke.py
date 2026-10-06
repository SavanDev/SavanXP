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
    # Espejo de taskbar.c: boton Start de 64 + gap 2, despues botones de 160
    # con gap 2 desde el margen 2.
    x = 2 + 64 + 2 + index * (160 + 2)
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
    # A la derecha de los botones de ventana y a la izquierda del altavoz
    # (x=1212): con dos ventanas de 160 los botones terminan en x=390.
    actual = _pixel(image, 1000, 786)
    if actual != FACE:
        raise TaskbarSmokeError(f"{state}: taskbar strip expected {FACE}, got {actual}")


def _volume_button() -> tuple[int, int]:
    # Espejo de taskbar.c: boton de 32 a la izquierda del layout de 32,
    # con gap 2 y margen 2 en 1280 de ancho. Centro del boton.
    return (1212 + 16, 774 + 11)


def _expect_volume_popup(image: Image.Image, state: str) -> None:
    # Popup de 144x56 anclado como el de layout: x=1280-4-144, y=772-56.
    # Sin el cursor: depende del volumen heredado de la imagen (el persistido
    # en /disk/audio.cfg), asi que su posicion la afirma el arrastre, no la
    # apertura.
    if _pixel(image, 1132, 716) != LIGHT:
        raise TaskbarSmokeError(f"{state}: volume popup frame expected {LIGHT}")
    # Fila de mute entre la casilla (termina en x=1150) y el texto (x=1154).
    if _pixel(image, 1152, 730) != FACE:
        raise TaskbarSmokeError(f"{state}: volume popup face expected {FACE}")
    # Carril hundido (borde sombra).
    if _pixel(image, 1150, 748) != SHADOW:
        raise TaskbarSmokeError(f"{state}: volume trough expected {SHADOW}")


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
        # Al centro del boton 0 (x=68+80=148, y=786): Start de 64 + gap 2
        # desde el margen 2, despues medio boton de 160.
        for dx, dy in [(64, 64), (84, 64)] + [(0, 64)] * 10 + [(0, 18)]:
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

        # Al boton del altavoz: a la esquina y despues al centro del boton.
        for _ in range(22):
            client.relative(-64, -64)
            time.sleep(0.03)
        volume_x, volume_y = _volume_button()
        steps_x = volume_x // 64
        steps_y = volume_y // 64
        for _ in range(steps_x):
            client.relative(64, 0)
            time.sleep(0.03)
        for _ in range(steps_y):
            client.relative(0, 64)
            time.sleep(0.03)
        client.relative(volume_x - steps_x * 64, volume_y - steps_y * 64)
        time.sleep(0.5)
        client.button("left", True)
        time.sleep(0.25)
        client.button("left", False)
        time.sleep(3.0)
        sixth = _capture(client, output_dir, "06-volumen")
        screenshots.append(output_dir / "06-volumen.png")
        _expect_volume_popup(sixth, "volume open")
        # Toggle: cerrar y reabrir con el mismo boton. Si el cierre no
        # funcionara, el segundo click cerraria y el popup no estaria.
        client.button("left", True)
        time.sleep(0.25)
        client.button("left", False)
        time.sleep(2.0)
        client.button("left", True)
        time.sleep(0.25)
        client.button("left", False)
        time.sleep(3.0)
        seventh = _capture(client, output_dir, "07-volumen-toggle")
        screenshots.append(output_dir / "07-volumen-toggle.png")
        _expect_volume_popup(seventh, "volume reopened")

        # Arrastre del cursor a 50: del centro del cursor (pantalla 1263,754,
        # popup 131,38) a pantalla 1204 (popup 72). Con offset de agarre 5,
        # v=(72-5-8)*100/118=50 exacto; el cursor queda en popup 67..76.
        client.relative(35, -31)
        time.sleep(0.5)
        client.button("left", True)
        time.sleep(0.25)
        client.relative(-30, 0)
        time.sleep(0.2)
        client.relative(-29, 0)
        time.sleep(0.2)
        client.button("left", False)
        time.sleep(2.0)
        # Estacionar lejos: el cursor pisaria el cursor del slider.
        client.relative(-104, 6)
        time.sleep(0.5)
        eighth = _capture(client, output_dir, "08-volumen-arrastrado")
        screenshots.append(output_dir / "08-volumen-arrastrado.png")
        if _pixel(eighth, 1203, 746) != LIGHT:
            raise TaskbarSmokeError("drag: thumb did not reach 50")
        if _pixel(eighth, 1262, 754) != FACE:
            raise TaskbarSmokeError("drag: old thumb spot is not trough")

        # Mute: click al centro de la fila (pantalla 1204,728). La marca de
        # la casilla y el glifo del boton (por polling) lo confirman.
        client.relative(104, -32)
        time.sleep(0.5)
        client.button("left", True)
        time.sleep(0.25)
        client.button("left", False)
        time.sleep(3.0)
        client.relative(-104, 32)
        time.sleep(0.5)
        ninth = _capture(client, output_dir, "09-volumen-mute")
        screenshots.append(output_dir / "09-volumen-mute.png")
        if _pixel(ninth, 1144, 728) != (0, 0, 0):
            raise TaskbarSmokeError("mute: checkbox mark missing")
        # La cruz roja del icono muteado: glifo 16x16 en (1220,778).
        if _pixel(ninth, 1230, 783) != (190, 40, 32):
            raise TaskbarSmokeError("mute: taskbar glyph did not switch")

    return screenshots
