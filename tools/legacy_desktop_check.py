"""Visual assertions for the i386 desktop and its PS/2 cursor."""
from __future__ import annotations

import re
import tempfile
import time
from pathlib import Path

import glyphs
from qmp_client import QmpClient

POINTER = (
    "o          ", "oo         ", "o#o        ", "o##o       ",
    "o###o      ", "o####o     ", "o#####o    ", "o######o   ",
    "o#######o  ", "o########o ", "o#####oooo ", "o##o#o     ",
    "o#o o#o    ", "oo  o#o    ", "o    o#o   ", "     o#o   ",
    "      o#o  ", "       oo  ",
)


def check_desktop(qmp_socket: Path, binary: Path, serial: Path,
                  artifacts: Path | None) -> None:
    from PIL import Image

    fonts = glyphs.load(binary)
    with tempfile.TemporaryDirectory(prefix="savanxp-desktop-check.") as temporary:
        output = artifacts or Path(temporary)
        with QmpClient(qmp_socket, timeout=10) as client:
            def capture(name: str):
                path = output / f"{name}.ppm"
                client.screenshot(path)
                with Image.open(path) as image:
                    result = image.convert("RGB")
                result.save(output / f"{name}.png")
                return result

            def assert_desktop(image):
                if image.size != (1024, 768):
                    raise RuntimeError(f"desktop geometry: {image.size}, expected 1024x768")
                for point, colour in [((900, 500), (18, 26, 44)),
                                      ((900, 750), (28, 42, 62)),
                                      ((100, 300), (232, 236, 242))]:
                    if image.getpixel(point) != colour:
                        raise RuntimeError(f"desktop geometry/colour mismatch at {point}")
                # Full glyph coverage, including text crossing a 64-row strip.
                for font, x, y, text, colour, background in [
                    ("title", 12, 4, "SavanXP Legacy", (236, 246, 255), (28, 42, 62)),
                    ("title", 92, 76, "SavanXP Legacy", (236, 246, 255), (44, 79, 124)),
                    ("body", 96, 116, "i386 legacy target", (40, 52, 68), (232, 236, 242)),
                    ("body", 12, 742, "SavanXP Legacy", (214, 226, 240), (28, 42, 62)),
                ]:
                    expected = glyphs.render(fonts[font], x, y, text, colour, background,
                                             (0, 0, 1024, 768))
                    if not expected or any(image.getpixel(p) != c for p, c in expected.items()):
                        raise RuntimeError(f"desktop glyph mismatch: {text!r} at {(x, y)}")

            # The ready token precedes painting. Retry until a complete frame exists.
            deadline = time.monotonic() + 15
            while True:
                before = capture("desktop-before")
                try:
                    assert_desktop(before)
                    break
                except RuntimeError:
                    if time.monotonic() >= deadline:
                        raise
                    time.sleep(0.25)

            def assert_pointer(image, x, y):
                for row, line in enumerate(POINTER):
                    for column, pixel in enumerate(line):
                        if pixel == " ":
                            continue
                        colour = (244, 248, 255) if pixel == "#" else (16, 20, 24)
                        if image.getpixel((x + column, y + row)) != colour:
                            raise RuntimeError(f"cursor mismatch at {(x + column, y + row)}")

            assert_pointer(before, 512, 384)
            client.relative(160, 100)
            time.sleep(0.3)
            client.button("left", True)
            time.sleep(0.2)
            client.tap("a")  # Exercise the shared keyboard/mouse data port.
            client.button("left", False)
            deadline = time.monotonic() + 10
            while True:
                log = serial.read_text(errors="replace")
                events = re.findall(r"ps2: mouse dx (-?\d+) dy (-?\d+) buttons (\d+) at (\d+),(\d+)", log)
                if (events and any(e[2] == "1" for e in events) and
                    events[-1][2:] == ("0", "672", "484") and
                    "ps2: tecla down 0x00000061" in log and
                    "ps2: tecla up 0x00000061" in log):
                    break
                if time.monotonic() >= deadline:
                    raise RuntimeError(f"PS/2 motion/buttons/keyboard delivery failed: {events}")
                time.sleep(0.1)
            after = capture("desktop-after")
            assert_desktop(after)
            assert_pointer(after, 672, 484)
            if after.crop((512, 384, 523, 402)).getextrema() != ((232, 232), (236, 236), (242, 242)):
                raise RuntimeError("old cursor background was not restored")
            # Force a clamp at the bottom-right corner, then return to the centre.
            def move_and_wait(dx, dy, target):
                event_pattern = r"ps2: mouse dx (-?\d+) dy (-?\d+) buttons (\d+) at (\d+),(\d+)"
                start = len(re.findall(event_pattern, serial.read_text(errors="replace")))
                client.relative(dx, dy)
                deadline = time.monotonic() + 15
                while time.monotonic() < deadline:
                    delivered = re.findall(event_pattern, serial.read_text(errors="replace"))[start:]
                    if (delivered and sum(int(e[0]) for e in delivered) == dx and
                        sum(int(e[1]) for e in delivered) == -dy and
                        tuple(map(int, delivered[-1][3:])) == target):
                        return
                    time.sleep(0.1)
                raise RuntimeError(f"mouse motion did not finish at {target}: {delivered}")

            move_and_wait(400, 300, (1023, 767))
            edge = capture("desktop-edge")
            if edge.getpixel((1023, 767)) != (16, 20, 24):
                raise RuntimeError("cursor did not clamp to the screen edge")
            move_and_wait(-511, -383, (512, 384))
            restored = capture("desktop-restored")
            assert_desktop(restored)
            assert_pointer(restored, 512, 384)
            if restored.tobytes() != before.tobytes():
                raise RuntimeError("cursor movement left trails or damaged the desktop")


def check_console_clear(qmp_socket: Path, artifacts: Path | None) -> None:
    """After sh32 clear, old boot text must be gone from all 48 console rows."""
    from PIL import Image, ImageDraw

    with tempfile.TemporaryDirectory(prefix="savanxp-console-check.") as temporary:
        path = (artifacts or Path(temporary)) / "console-cleared.ppm"
        with QmpClient(qmp_socket, timeout=10) as client:
            deadline = time.monotonic() + 15
            while True:
                client.screenshot(path)
                with Image.open(path) as screenshot:
                    image = screenshot.convert("RGB")
                # Ignore the cursor and the bottom rows used by the prompt/IRQ diagnostics.
                area = image.crop((0, 0, 1024, 704))
                ImageDraw.Draw(area).rectangle((512, 384, 522, 401), fill=(16, 24, 36))
                if area.getextrema() == ((16, 16), (24, 24), (36, 36)):
                    image.save(path.with_suffix(".png"))
                    return
                if time.monotonic() >= deadline:
                    image.save(path.with_suffix(".png"))
                    raise RuntimeError("sh32 clear left old text in the graphical console")
                time.sleep(0.2)
