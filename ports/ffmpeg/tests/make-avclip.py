"""Genera el clip de sincronia del reproductor (build/media/avsync.avi).

Un AVI con video MJPEG y audio PCM entrelazados, escrito a mano: el contenedor
es simple y asi no hace falta ningun encoder, solo Pillow. Ejercita lo que los
otros dos clips no: un demuxer con dos streams intercalados, el resampleo (el
audio va a 44.1 kHz mono y el dispositivo es de 48 kHz estereo) y el seek por
indice.

El contenido es una prueba de sincronia medible sin ojos ni oidos: al comienzo
exacto de cada segundo hay un cuadro de destello (fondo casi blanco) y arranca
un pitido de 1 kHz de 100 ms sobre silencio. `mediaplayer --selftest --sync`
detecta los dos eventos y compara sus tiempos; a mano, en la ventana, el pitido
tiene que oirse con el destello.
"""

import io
import math
import os
import struct

from PIL import Image, ImageDraw

WIDTH = 320
HEIGHT = 240
FPS = 25
SECONDS = 6
QUALITY = 80
SAMPLE_RATE = 44100
TONE_HZ = 1000
TONE_MS = 100
TONE_AMPLITUDE = 16384

FRAMES = FPS * SECONDS
SAMPLES_PER_FRAME = SAMPLE_RATE // FPS


def draw_frame(index: int) -> bytes:
    flash = index % FPS == 0
    background = (240, 240, 232) if flash else (18, 22, 34)
    ink = (20, 20, 20) if flash else (235, 235, 235)
    image = Image.new("RGB", (WIDTH, HEIGHT), background)
    draw = ImageDraw.Draw(image)

    # Barra que avanza un paso por cuadro dentro de cada segundo.
    step = (WIDTH - 40) * (index % FPS) // (FPS - 1)
    draw.rectangle([step, HEIGHT - 40, step + 39, HEIGHT - 12], fill=(232, 189, 78))
    # Franjas de color puro arriba: con los planos U/V cruzados cambian de color.
    for slot, color in enumerate([(255, 0, 0), (0, 255, 0), (0, 0, 255)]):
        draw.rectangle([slot * 40, 0, slot * 40 + 39, 15], fill=color)
    seconds, frame = divmod(index, FPS)
    draw.text((WIDTH // 2 - 30, HEIGHT // 2 - 8), f"{seconds}s + {frame:02d}", fill=ink)

    buffer = io.BytesIO()
    image.save(buffer, "JPEG", quality=QUALITY)
    return buffer.getvalue()


def audio_for_frame(index: int) -> bytes:
    first = index * SAMPLES_PER_FRAME
    tone_samples = SAMPLE_RATE * TONE_MS // 1000
    samples = []
    for n in range(first, first + SAMPLES_PER_FRAME):
        offset = n % SAMPLE_RATE
        if offset < tone_samples:
            value = int(TONE_AMPLITUDE * math.sin(2 * math.pi * TONE_HZ * offset / SAMPLE_RATE))
        else:
            value = 0
        samples.append(value)
    return struct.pack(f"<{len(samples)}h", *samples)


def chunk(fourcc: bytes, payload: bytes) -> bytes:
    data = fourcc + struct.pack("<I", len(payload)) + payload
    return data + (b"\0" if len(payload) % 2 else b"")


def list_chunk(kind: bytes, payload: bytes) -> bytes:
    return chunk(b"LIST", kind + payload)


def main() -> None:
    root = os.path.dirname(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))
    output_root = os.environ.get("SAVANXP_OUTPUT_ROOT", os.path.join(root, "build"))
    out_dir = os.path.join(output_root, "media")
    os.makedirs(out_dir, exist_ok=True)
    out_path = os.path.join(out_dir, "avsync.avi")

    video = [draw_frame(index) for index in range(FRAMES)]
    audio = [audio_for_frame(index) for index in range(FRAMES)]
    max_video = max(len(frame) for frame in video)
    block_align = 2
    total_samples = FRAMES * SAMPLES_PER_FRAME

    # movi + idx1. Los offsets del indice cuentan desde el fourcc 'movi'.
    movi = bytearray(b"movi")
    index_entries = bytearray()
    for frame, samples in zip(video, audio):
        for fourcc, payload in ((b"00dc", frame), (b"01wb", samples)):
            index_entries += struct.pack("<4sIII", fourcc, 0x10, len(movi), len(payload))
            movi += chunk(fourcc, payload)

    avih = struct.pack(
        "<IIIIIIIIII16x",
        1000000 // FPS,              # microsegundos por cuadro
        max_video * FPS + SAMPLE_RATE * block_align,
        0,
        0x10 | 0x100,                # AVIF_HASINDEX | AVIF_ISINTERLEAVED
        FRAMES,
        0,
        2,
        max_video,
        WIDTH,
        HEIGHT,
    )
    video_strh = struct.pack(
        "<4s4sIHHIIIIIIIIhhhh",
        b"vids", b"MJPG", 0, 0, 0, 0, 1, FPS, 0, FRAMES, max_video, 0xFFFFFFFF, 0, 0, 0, WIDTH, HEIGHT,
    )
    video_strf = struct.pack("<IiiHH4sIiiII", 40, WIDTH, HEIGHT, 1, 24, b"MJPG", WIDTH * HEIGHT * 3, 0, 0, 0, 0)
    audio_strh = struct.pack(
        "<4s4sIHHIIIIIIIIhhhh",
        b"auds", b"\0\0\0\0", 0, 0, 0, 0, block_align, SAMPLE_RATE * block_align, 0, total_samples,
        SAMPLES_PER_FRAME * block_align, 0xFFFFFFFF, block_align, 0, 0, 0, 0,
    )
    audio_strf = struct.pack("<HHIIHH", 1, 1, SAMPLE_RATE, SAMPLE_RATE * block_align, block_align, 16)

    hdrl = list_chunk(
        b"hdrl",
        chunk(b"avih", avih)
        + list_chunk(b"strl", chunk(b"strh", video_strh) + chunk(b"strf", video_strf))
        + list_chunk(b"strl", chunk(b"strh", audio_strh) + chunk(b"strf", audio_strf)),
    )
    body = b"AVI " + hdrl + chunk(b"LIST", bytes(movi)) + chunk(b"idx1", bytes(index_entries))

    with open(out_path, "wb") as handle:
        handle.write(b"RIFF" + struct.pack("<I", len(body)) + body)

    print(f"{out_path}: {os.path.getsize(out_path)} bytes, {FRAMES} cuadros de {WIDTH}x{HEIGHT} "
          f"a {FPS} fps + PCM {SAMPLE_RATE} Hz mono, {SECONDS} s")


main()
