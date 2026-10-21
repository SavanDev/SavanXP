#!/usr/bin/env python3
"""Host-side taskbar smoke actions driven through QMP."""

from __future__ import annotations

import time
from pathlib import Path

from PIL import Image

from qmp_client import QmpClient
from shoot_session import (
    STARTMENU_HEIGHT,
    STARTMENU_STRIP_WIDTH,
    STARTMENU_VIEW_HEIGHT,
    STARTMENU_WIDTH,
    find_text,
)


class TaskbarSmokeError(RuntimeError):
    pass


FACE = (192, 192, 192)
LIGHT = (255, 255, 255)
SHADOW = (128, 128, 128)
# A point on Calculator's caption: the WM centres the window, so its title bar
# spans x=505..830 at y=308 on the 1280x800 screen.
CALC_CAPTION = (600, 308)


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
    # A la derecha de los botones de ventana y a la izquierda del altavoz:
    # con dos ventanas de 160 los botones terminan en x=390 y la bandeja
    # (volumen 1146..1178, layout 1180..1212, reloj 1214..1278) empieza en
    # x=1146.
    actual = _pixel(image, 1000, 786)
    if actual != FACE:
        raise TaskbarSmokeError(f"{state}: taskbar strip expected {FACE}, got {actual}")


def _expect_clock(image: Image.Image, state: str) -> None:
    # Espejo de taskbar.c: pozo hundido de 64 al borde derecho (x=1214..1278,
    # pantalla 1280) con el HH:MM centrado. Los bordes son rellenos solidos;
    # el texto se afirma solo por presencia (antialiaseado, no pixel exacto).
    if _pixel(image, 1214, 774) != SHADOW:
        raise TaskbarSmokeError(f"{state}: clock top-left expected {SHADOW}")
    if _pixel(image, 1277, 797) != LIGHT:
        raise TaskbarSmokeError(f"{state}: clock bottom-right expected {LIGHT}")
    inner = [_pixel(image, x, y) for x in range(1216, 1276, 3) for y in range(777, 795, 3)]
    if all(pixel == FACE for pixel in inner):
        raise TaskbarSmokeError(f"{state}: clock text missing")


def _expect_clock_popup(image: Image.Image, state: str) -> None:
    # Popup de 178x180 anclado como el de volumen: x=1280-4-178, y=772-180.
    if _pixel(image, 1098, 592) != LIGHT:
        raise TaskbarSmokeError(f"{state}: clock popup frame expected {LIGHT}")
    if _pixel(image, 1100, 594) != FACE:
        raise TaskbarSmokeError(f"{state}: clock popup face expected {FACE}")
    # Cabecera con el mes: basta presencia (antialiaseado, no pixel exacto).
    header = [_pixel(image, x, y) for x in range(1104, 1270, 3) for y in range(598, 614, 3)]
    if all(pixel == FACE for pixel in header):
        raise TaskbarSmokeError(f"{state}: clock popup header text missing")
    # El dia de hoy va sobre azul (SELECT): el RTC del invitado es la hora del
    # host, asi que siempre hay exactamente un dia resaltado en la grilla.
    grid = [_pixel(image, x, y) for x in range(1103, 1271, 4) for y in range(640, 744, 4)]
    if (0, 0, 128) not in grid:
        raise TaskbarSmokeError(f"{state}: clock popup today highlight missing")


def _click(client: QmpClient) -> None:
    client.button("left", True)
    time.sleep(0.25)
    client.button("left", False)
    time.sleep(0.5)


def _volume_button() -> tuple[int, int]:
    # Espejo de taskbar.c: boton de 32 a la izquierda del layout de 32, que a
    # su vez esta a la izquierda del reloj de 64, con gaps de 2 y margen 2 en
    # 1280 de ancho (1146..1178). Centro del boton.
    return (1146 + 16, 774 + 11)


def _expect_volume_popup(image: Image.Image, state: str) -> None:
    # Popup de 144x140 anclado como el de layout: x=1280-4-144, y=772-140.
    # Sin el cursor: depende del volumen heredado de la imagen (el persistido
    # en /disk/audio.cfg), asi que su posicion la afirma el arrastre, no la
    # apertura.
    if _pixel(image, 1132, 632) != LIGHT:
        raise TaskbarSmokeError(f"{state}: volume popup frame expected {LIGHT}")
    # Fila de mute entre la casilla (termina en x=1150) y el texto (x=1154).
    if _pixel(image, 1152, 646) != FACE:
        raise TaskbarSmokeError(f"{state}: volume popup face expected {FACE}")
    # Carril hundido (borde sombra).
    if _pixel(image, 1150, 664) != SHADOW:
        raise TaskbarSmokeError(f"{state}: volume trough expected {SHADOW}")


def _move_to(client: QmpClient, x: int, y: int) -> None:
    # The guest pointer is relative: park it in the top-left corner, then walk
    # to the target in steps of 64 (no acceleration at that size).
    for _ in range(22):
        client.relative(-64, -64)
        time.sleep(0.03)
    for _ in range(x // 64):
        client.relative(64, 0)
        time.sleep(0.03)
    for _ in range(y // 64):
        client.relative(0, 64)
        time.sleep(0.03)
    client.relative(x % 64, y % 64)
    time.sleep(0.5)


def _open_progman(client: QmpClient, output_dir: Path) -> None:
    """Opens Program Manager from the Start menu, as a user would.

    The session does not start it. The menu is mouse only and its list may be
    longer than the menu, so the row is found by its text: first as the menu
    opens, then after resting the pointer on the down arrow strip, which
    scrolls the list to its end (System, Program Manager's group, is last).
    """
    menu_x, menu_y = 2, 800 - 28 - STARTMENU_HEIGHT
    view_x = menu_x + 2 + STARTMENU_STRIP_WIDTH
    box = (view_x, menu_y + 2, menu_x + STARTMENU_WIDTH - 2, menu_y + 2 + STARTMENU_VIEW_HEIGHT)
    _move_to(client, 2 + 32, 786)
    _click(client)
    image = None
    for attempt in range(15):
        time.sleep(2.0)
        image = _capture(client, output_dir, f"00-menu-{attempt:02d}")
        if _pixel(image, view_x + 2, menu_y + 4) == FACE:
            break
    else:
        raise TaskbarSmokeError("the Start menu did not open")
    origin = find_text(image, box, "Program Manager", (0, 0, 0), FACE, solid_only=True)[0]
    if origin is None:
        # Rest on the down strip's left end: on the strip, but off the arrow
        # and off every row (a hovered row is repainted white on blue).
        down_strip_y = menu_y + 2 + STARTMENU_VIEW_HEIGHT - 12
        _move_to(client, view_x + 20, down_strip_y + 6)
        time.sleep(10.0)
        image = _capture(client, output_dir, "00-menu-scrolled")
        origin = find_text(image, box, "Program Manager", (0, 0, 0), FACE, solid_only=True)[0]
    if origin is None:
        raise TaskbarSmokeError("the Start menu does not list Program Manager")
    _move_to(client, origin[0] + 20, origin[1] + 6)
    _click(client)
    time.sleep(25.0)


def run_taskbar_actions(qmp_path: Path, output_dir: Path, ready_wait: float) -> list[Path]:
    output_dir.mkdir(parents=True, exist_ok=True)
    if ready_wait:
        time.sleep(ready_wait)

    screenshots: list[Path] = []
    with QmpClient(qmp_path, timeout=5.0) as client:
        empty = _capture(client, output_dir, "00-sin-ventanas")
        screenshots.append(output_dir / "00-sin-ventanas.png")
        if empty.size != (1280, 800):
            raise TaskbarSmokeError(f"unexpected initial screenshot size: {empty.size}")
        _expect_strip(empty, "session start")
        _expect_clock(empty, "session start")
        # The session opens no window: Program Manager is started from the menu.
        if _button(empty, 0) != (FACE, FACE):
            raise TaskbarSmokeError("session start: a window button is already on the taskbar")

        _open_progman(client, output_dir)
        first = _capture(client, output_dir, "01-solo-progman")
        screenshots.append(output_dir / "01-solo-progman.png")
        _expect_strip(first, "initial")
        _expect_clock(first, "initial")
        _expect_button(first, 0, True, "initial")

        # Enter on Program Manager's first icon, Calculator: the point is a
        # second window. Not an icon further right, since that position
        # depends on what is installed (a native demo there opens no window).
        client.tap("ret")
        time.sleep(25.0)
        second = _capture(client, output_dir, "02-dos-ventanas")
        screenshots.append(output_dir / "02-dos-ventanas.png")
        _expect_strip(second, "two windows")
        _expect_clock(second, "two windows")
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
        # Minimizing from the taskbar hands activation to the window below,
        # and its caption must be repainted active: the same pixel it had when
        # Calculator was freshly opened and active. Nothing else repaints it
        # here, so a stale inactive caption is exactly the bug this catches.
        active_caption = _pixel(second, *CALC_CAPTION)
        if _pixel(fifth, *CALC_CAPTION) != active_caption:
            raise TaskbarSmokeError("after second click: the window left active kept an inactive caption")

        # Al reloj: a la esquina y despues al centro del pozo (1214..1278,
        # centro 1246,785). Abre el calendario; toggle lo cierra y lo reabre.
        for _ in range(22):
            client.relative(-64, -64)
            time.sleep(0.03)
        clock_x, clock_y = (1214 + 32, 774 + 11)
        steps_x = clock_x // 64
        steps_y = clock_y // 64
        for _ in range(steps_x):
            client.relative(64, 0)
            time.sleep(0.03)
        for _ in range(steps_y):
            client.relative(0, 64)
            time.sleep(0.03)
        client.relative(clock_x - steps_x * 64, clock_y - steps_y * 64)
        time.sleep(0.5)
        _click(client)
        time.sleep(3.0)
        sixth = _capture(client, output_dir, "06-reloj")
        screenshots.append(output_dir / "06-reloj.png")
        _expect_clock_popup(sixth, "clock open")
        # Toggle: cerrar y reabrir con el mismo reloj.
        _click(client)
        time.sleep(2.0)
        _click(client)
        time.sleep(3.0)
        seventh = _capture(client, output_dir, "07-reloj-toggle")
        screenshots.append(output_dir / "07-reloj-toggle.png")
        _expect_clock_popup(seventh, "clock reopened")
        # Cerrar para dejar la esquina libre y estacionar en la esquina para
        # la seccion de volumen.
        _click(client)
        time.sleep(2.0)

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
        sixth = _capture(client, output_dir, "08-volumen")
        screenshots.append(output_dir / "08-volumen.png")
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
        seventh = _capture(client, output_dir, "09-volumen-toggle")
        screenshots.append(output_dir / "09-volumen-toggle.png")
        _expect_volume_popup(seventh, "volume reopened")

        # Arrastre del cursor a 50: del centro del cursor (pantalla 1263,670,
        # popup 131,38) a pantalla 1204 (popup 72). Con offset de agarre 5,
        # v=(72-5-8)*100/118=50 exacto; el cursor queda en popup 67..76.
        # El punto de agarre es fijo del popup; el dx compensa que el boton
        # del altavoz esta a la izquierda del reloj (centro x=1162).
        client.relative(101, -115)
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
        client.relative(-564, -370)
        time.sleep(0.5)
        eighth = _capture(client, output_dir, "10-volumen-arrastrado")
        screenshots.append(output_dir / "10-volumen-arrastrado.png")
        if _pixel(eighth, 1203, 662) != LIGHT:
            raise TaskbarSmokeError("drag: thumb did not reach 50")
        if _pixel(eighth, 1262, 670) != FACE:
            raise TaskbarSmokeError("drag: old thumb spot is not trough")

        # Mute: click en la casilla mute (popup local 12,11 -> pantalla
        # 1144,643). La marca de la casilla y el glifo del boton (por
        # polling) lo confirman.
        client.relative(504, 343)
        time.sleep(0.5)
        client.button("left", True)
        time.sleep(0.25)
        client.button("left", False)
        time.sleep(3.0)
        client.relative(-504, -343)
        time.sleep(0.5)
        ninth = _capture(client, output_dir, "11-volumen-mute")
        screenshots.append(output_dir / "11-volumen-mute.png")
        if _pixel(ninth, 1144, 644) != (0, 0, 0):
            raise TaskbarSmokeError("mute: checkbox mark missing")
        # La cruz roja del icono muteado: glifo 16x16 en el boton (1146..1178,
        # centro 1162, pantalla y=778..794).
        if _pixel(ninth, 1164, 784) != (190, 40, 32):
            raise TaskbarSmokeError("mute: taskbar glyph did not switch")

    return screenshots
