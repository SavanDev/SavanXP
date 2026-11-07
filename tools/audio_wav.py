"""Independent assertions for the known audiotest 440 Hz square-wave stream.

Use actual captured data bytes: QEMU can leave RIFF/data lengths at zero when
terminated, although the PCM payload is complete. No success token or driver
counter is used to decide whether sound was recorded.
"""
from __future__ import annotations
import array
import math
import struct
import sys
from pathlib import Path


def verify_tone(path: Path, *, quiet: bool = False) -> dict:
    blob = path.read_bytes()
    if blob[:4] != b"RIFF" or blob[8:12] != b"WAVE":
        raise ValueError("missing RIFF/WAVE header")
    offset = 12
    fmt = None
    pcm = None
    while offset + 8 <= len(blob):
        kind, size = struct.unpack_from("<4sI", blob, offset)
        offset += 8
        if kind == b"fmt ":
            if size < 16 or offset + size > len(blob):
                raise ValueError("truncated format")
            fmt = struct.unpack_from("<HHIIHH", blob, offset)
        elif kind == b"data":
            if size and offset + size > len(blob):
                raise ValueError("truncated PCM data")
            pcm = blob[offset:offset + size] if size else blob[offset:]
            break
        offset += size + (size & 1)
    if fmt is None or pcm is None:
        raise ValueError("missing format or PCM data")
    encoding, channels, rate, _, alignment, bits = fmt
    if encoding != 1 or channels != 2 or bits != 16 or alignment != 4 or rate != 48000:
        raise ValueError(f"expected stereo S16 PCM at 48000 Hz, got {fmt}")
    if len(pcm) % 4:
        raise ValueError("incomplete stereo frame")
    samples = array.array("h", pcm)
    if sys.byteorder != "little":
        samples.byteswap()
    left, right = samples[0::2], samples[1::2]
    # Startup and shutdown silence are allowed; internal silent holes are not.
    active = [i for i, x in enumerate(left) if abs(x) > 500]
    if not active:
        raise ValueError("capture is silent")
    first, last = active[0], active[-1]
    duration = (last - first + 1) / rate
    if not 2.0 <= duration <= 3.5:
        raise ValueError(f"expected about 2.5 seconds of tone, got {duration:.3f}")
    signal = left[first:last + 1]
    rms = math.sqrt(sum(x * x for x in signal) / len(signal))
    expected = 6000 if quiet else 12000
    if not expected * 0.80 <= rms <= expected * 1.05:
        raise ValueError(f"RMS {rms:.1f} outside expected {expected} +/- tolerance")
    longest = run = 0
    for x in signal:
        run = run + 1 if abs(x) <= 500 else 0
        longest = max(longest, run)
    gap_ms = longest * 1000 / rate
    if gap_ms > 20:
        raise ValueError(f"internal silence gap {gap_ms:.1f} ms exceeds 20 ms")
    crossings = sum(a <= 0 < b for a, b in zip(signal, signal[1:]))
    frequency = crossings / duration
    if not 420 <= frequency <= 460:
        raise ValueError(f"expected 440 Hz, got {frequency:.1f} Hz")
    mismatch = sum(abs(left[i] - right[i]) > 100 for i in range(first, last + 1))
    if mismatch > len(signal) // 100:
        raise ValueError("stereo channels disagree")
    return {"seconds": round(duration, 3), "rms": round(rms),
            "hz": round(frequency, 1), "max_gap_ms": round(gap_ms, 2)}
