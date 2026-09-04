"""Genera el clip de prueba del player (build/media/clip.mjpeg).

Un stream MJPEG crudo son JPEGs concatenados y nada mas: no hay contenedor que
muxear, asi que alcanza con Pillow. El demuxer 'mjpeg' de FFmpeg lo lee tal cual.

El contenido esta pensado para que un error se VEA: una barra que se mueve un
paso por cuadro (un frame repetido o salteado se nota), franjas de color puro en
los bordes (un YUV->RGB con los planos cruzados las tine mal) y un numero de
cuadro grande (un frame corrupto deja de ser legible).
"""

import os

from PIL import Image, ImageDraw

WIDTH = 320
HEIGHT = 240
FRAMES = 24
QUALITY = 85


def draw_frame(index: int) -> Image.Image:
    image = Image.new("RGB", (WIDTH, HEIGHT), (16, 16, 24))
    draw = ImageDraw.Draw(image)

    # Degradado vertical: cualquier corrimiento de filas se ve como un salto.
    for y in range(HEIGHT):
        level = int(255 * y / (HEIGHT - 1))
        draw.line([(0, y), (WIDTH - 1, y)], fill=(level // 3, level // 5, level))

    # Franjas de color puro: si los planos U/V se cruzan, cambian de color.
    bar_width = WIDTH // 8
    for slot, color in enumerate([(255, 0, 0), (0, 255, 0), (0, 0, 255), (255, 255, 0)]):
        x0 = slot * bar_width
        draw.rectangle([x0, 0, x0 + bar_width - 1, 23], fill=color)

    # Barra que avanza un paso por cuadro.
    position = int((WIDTH - 40) * index / (FRAMES - 1))
    draw.rectangle([position, HEIGHT - 48, position + 39, HEIGHT - 9], fill=(255, 255, 255))

    # Numero de cuadro, grande y en el centro.
    text = str(index)
    draw.text((WIDTH // 2 - 6 * len(text), HEIGHT // 2 - 12), text, fill=(255, 255, 255))
    return image


def main() -> None:
    root = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    out_dir = os.path.join(root, "build", "media")
    os.makedirs(out_dir, exist_ok=True)
    out_path = os.path.join(out_dir, "clip.mjpeg")

    with open(out_path, "wb") as handle:
        for index in range(FRAMES):
            frame_path = os.path.join(out_dir, ".frame.jpg")
            draw_frame(index).save(frame_path, "JPEG", quality=QUALITY)
            with open(frame_path, "rb") as frame_handle:
                handle.write(frame_handle.read())
            os.remove(frame_path)

    size = os.path.getsize(out_path)
    print(f"{out_path}: {size} bytes, {FRAMES} cuadros de {WIDTH}x{HEIGHT}")


main()
