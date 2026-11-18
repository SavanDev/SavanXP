"""Write the Media Player's unsupported-codec clips (build/media/).

Two files whose streams name codecs FFmpeg knows and this port does not enable,
so `mediaplayer --selftest --unsupported <codec> <file>` can check that the
player names the codec instead of failing without saying why:

- ffv1.avi: FFV1 video next to PCM audio. The video is left out and named, and
  the audio still plays -- the shape of an AV1 WebM on this build.
- mulaw.wav: G.711 mu-law audio and nothing else. Nothing can play, so the file
  is refused and the error names pcm_mulaw.

The video payloads are never decoded, so they are placeholders and no encoder
or image library is needed.
"""

import math
import os
import struct

WIDTH = 64
HEIGHT = 48
FPS = 10
SECONDS = 2
SAMPLE_RATE = 8000
TONE_HZ = 440
AMPLITUDE = 12000

FRAMES = FPS * SECONDS
SAMPLES_PER_FRAME = SAMPLE_RATE // FPS


def tone(count: int, start: int) -> list:
    return [int(AMPLITUDE * math.sin(2 * math.pi * TONE_HZ * (start + i) / SAMPLE_RATE)) for i in range(count)]


def chunk(fourcc: bytes, payload: bytes) -> bytes:
    data = fourcc + struct.pack("<I", len(payload)) + payload
    return data + (b"\0" if len(payload) % 2 else b"")


def list_chunk(kind: bytes, payload: bytes) -> bytes:
    return chunk(b"LIST", kind + payload)


def write_ffv1_avi(path: str) -> None:
    frame = bytes(16)
    block_align = 2
    movi = bytearray(b"movi")
    index_entries = bytearray()
    for index in range(FRAMES):
        samples = tone(SAMPLES_PER_FRAME, index * SAMPLES_PER_FRAME)
        audio = struct.pack(f"<{len(samples)}h", *samples)
        for fourcc, payload in ((b"00dc", frame), (b"01wb", audio)):
            index_entries += struct.pack("<4sIII", fourcc, 0x10, len(movi), len(payload))
            movi += chunk(fourcc, payload)

    avih = struct.pack(
        "<IIIIIIIIII16x",
        1000000 // FPS,
        len(frame) * FPS + SAMPLE_RATE * block_align,
        0,
        0x10 | 0x100,  # AVIF_HASINDEX | AVIF_ISINTERLEAVED
        FRAMES,
        0,
        2,
        len(frame),
        WIDTH,
        HEIGHT,
    )
    video_strh = struct.pack(
        "<4s4sIHHIIIIIIIIhhhh",
        b"vids", b"FFV1", 0, 0, 0, 0, 1, FPS, 0, FRAMES, len(frame), 0xFFFFFFFF, 0, 0, 0, WIDTH, HEIGHT,
    )
    video_strf = struct.pack("<IiiHH4sIiiII", 40, WIDTH, HEIGHT, 1, 24, b"FFV1", 0, 0, 0, 0, 0)
    audio_strh = struct.pack(
        "<4s4sIHHIIIIIIIIhhhh",
        b"auds", b"\0\0\0\0", 0, 0, 0, 0, block_align, SAMPLE_RATE * block_align, 0, FRAMES * SAMPLES_PER_FRAME,
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
    with open(path, "wb") as handle:
        handle.write(b"RIFF" + struct.pack("<I", len(body)) + body)
    print(f"{path}: {os.path.getsize(path)} bytes, FFV1 {WIDTH}x{HEIGHT} + PCM {SAMPLE_RATE} Hz, {SECONDS} s")


def mulaw(sample: int) -> int:
    bias = 0x84
    sign = 0x80 if sample < 0 else 0
    magnitude = min(abs(sample), 32635) + bias
    exponent = 7
    while exponent > 0 and not magnitude & (0x4000 >> (7 - exponent)):
        exponent -= 1
    mantissa = (magnitude >> (exponent + 3)) & 0x0F
    return ~(sign | (exponent << 4) | mantissa) & 0xFF


def write_mulaw_wav(path: str) -> None:
    samples = bytes(mulaw(value) for value in tone(SAMPLE_RATE * SECONDS, 0))
    header = b"RIFF" + struct.pack("<I", 36 + len(samples)) + b"WAVEfmt "
    header += struct.pack("<IHHIIHH", 16, 7, 1, SAMPLE_RATE, SAMPLE_RATE, 1, 8)  # 7 = WAVE_FORMAT_MULAW
    header += b"data" + struct.pack("<I", len(samples))
    with open(path, "wb") as handle:
        handle.write(header + samples)
    print(f"{path}: {len(header) + len(samples)} bytes, mu-law {SAMPLE_RATE} Hz")


def main() -> None:
    root = os.path.dirname(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))
    output_root = os.environ.get("SAVANXP_OUTPUT_ROOT", os.path.join(root, "build"))
    out_dir = os.path.join(output_root, "media")
    os.makedirs(out_dir, exist_ok=True)
    write_ffv1_avi(os.path.join(out_dir, "ffv1.avi"))
    write_mulaw_wav(os.path.join(out_dir, "mulaw.wav"))


main()
