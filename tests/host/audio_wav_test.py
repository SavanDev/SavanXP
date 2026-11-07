#!/usr/bin/env python3
"""Prove the audio oracle rejects silence, holes, wrong rates and wrong gain."""
import array
import math
from pathlib import Path
import struct
import sys
import tempfile
sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
from audio_wav import verify_tone


def capture(path, amplitude=12000, *, gap=False, frequency=440, rate=48000, channel=True):
    signal = array.array("h")
    for frame in range(rate * 3):
        # A square wave based on a 440 Hz clock, independently of guest period math.
        x = amplitude if math.sin(2 * math.pi * frequency * frame / rate) >= 0 else -amplitude
        if frame < rate // 4 or frame >= rate * 11 // 4 or (gap and rate < frame < rate * 6 // 5):
            x = 0
        signal.extend((x, x if channel else -x))
    if sys.byteorder != "little": signal.byteswap()
    data = signal.tobytes()
    # Deliberately emulate QEMU's unfinalized size fields.
    path.write_bytes(b"RIFF\0\0\0\0WAVEfmt " + struct.pack("<IHHIIHH",16,1,2,rate,rate*4,4,16) +
                     b"data\0\0\0\0" + data)


def reject(path, **options):
    try:
        verify_tone(path, **options)
    except ValueError:
        return
    raise AssertionError("defective capture passed")


with tempfile.TemporaryDirectory() as temp:
    path = Path(temp) / "test.wav"
    capture(path); verify_tone(path)
    capture(path, 6000); verify_tone(path, quiet=True); reject(path)
    capture(path, 0); reject(path)
    capture(path, gap=True); reject(path)
    capture(path, frequency=880); reject(path)
    capture(path, rate=44100); reject(path)
    capture(path, channel=False); reject(path)
    path.write_bytes(b"RIFF"); reject(path)
print("audio-wav-test: PASS (valid tones accepted; defective captures rejected)")
