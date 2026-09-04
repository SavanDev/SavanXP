"""Genera el WAV de prueba del port de FFmpeg (build/media/tono.wav).

Un segundo de 440 Hz, 8000 Hz mono, PCM s16le: 16 KB. Se genera y no se
versiona porque es contenido derivado, y ademas el arbol tiene que poder
reconstruirlo sin bajar nada.
"""

import math
import os
import struct

RATE = 8000
SECONDS = 1
FREQUENCY = 440.0
AMPLITUDE = 12000


def main() -> None:
    root = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    out_dir = os.path.join(root, "build", "media")
    os.makedirs(out_dir, exist_ok=True)
    out_path = os.path.join(out_dir, "tono.wav")

    frames = RATE * SECONDS
    samples = b"".join(
        struct.pack("<h", int(AMPLITUDE * math.sin(2 * math.pi * FREQUENCY * i / RATE)))
        for i in range(frames)
    )

    header = b"RIFF" + struct.pack("<I", 36 + len(samples)) + b"WAVEfmt "
    header += struct.pack("<IHHIIHH", 16, 1, 1, RATE, RATE * 2, 2, 16)
    header += b"data" + struct.pack("<I", len(samples))

    with open(out_path, "wb") as handle:
        handle.write(header + samples)

    print(f"{out_path}: {len(header) + len(samples)} bytes, {frames} samples")


main()
