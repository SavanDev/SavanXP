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


def _read_stereo(path: Path):
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
    return left, right


def _verify_tone(path: Path, expected: int, minimum_rms: float, channel_tolerance: int) -> dict:
    left, right = _read_stereo(path)
    rate = 48000
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
    if not expected * minimum_rms <= rms <= expected * 1.05:
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
    mismatch = sum(abs(left[i] - right[i]) > channel_tolerance for i in range(first, last + 1))
    if mismatch > len(signal) // 100:
        raise ValueError("stereo channels disagree")
    return {"seconds": round(duration, 3), "rms": round(rms),
            "hz": round(frequency, 1), "max_gap_ms": round(gap_ms, 2)}


def verify_tone(path: Path, *, quiet: bool = False) -> dict:
    return _verify_tone(path,6000 if quiet else 12000,0.80,100)


def verify_preferences(path: Path) -> dict:
    """A 3000-amplitude source restored at application volume 43 percent."""
    return _verify_tone(path,1290,0.95,2)


def verify_duplex(path: Path) -> dict:
    """A continuous 440 Hz/3000 source while native capture assertions run.

    The guest deliberately closes/reopens playback for ownership tests. Require
    a real continuous duplex interval, independently of the guest's PASS token.
    Descriptor-starved tiny direct writes cannot form this interval.
    """
    left, right = _read_stereo(path)
    start = None
    best = (0, 0)
    for i in range(len(left)+1):
        active = i < len(left) and abs(left[i]) > 100 and abs(right[i]) > 100
        if active and start is None:
            start = i
        elif not active and start is not None:
            if i-start > best[1]-best[0]: best = (start, i)
            start = None
    first, last = best
    duration = (last-first)/48000
    if duration < 0.9: raise ValueError(f"no continuous duplex tone: longest interval {duration:.3f} s")
    signal = left[first:last]
    rms = math.sqrt(sum(x*x for x in signal)/len(signal))
    frequency = sum(a <= 0 < b for a,b in zip(signal,signal[1:]))/duration
    if not 2850 <= rms <= 3150: raise ValueError(f"wrong duplex RMS: {rms:.1f}")
    if not 420 <= frequency <= 460: raise ValueError(f"wrong duplex frequency: {frequency:.1f}")
    if any(abs(left[i]-right[i]) > 2 for i in range(first,last)):
        raise ValueError("duplex output channels differ")
    return {"continuous_seconds":round(duration,3),"rms":round(rms),"hz":round(frequency,1)}


def verify_recording(path: Path) -> dict:
    """Sound's three-second recording test, replaying the known virtual mic."""
    left, right = _read_stereo(path)
    active = [i for i in range(len(left)) if abs(left[i]) > 100 or abs(right[i]) > 100]
    if not active: raise ValueError("recording replay is silent")
    first, last = active[0], active[-1]+1
    duration = (last-first)/48000
    if not 2.7 <= duration <= 3.3: raise ValueError(f"wrong recording duration: {duration:.3f}")
    result = {"seconds":round(duration,3)}
    for name,channel,amplitude,hz in (("left",left,12000,400),("right",right,6000,600)):
        signal = channel[first:last]
        rms = math.sqrt(sum(x*x for x in signal)/len(signal))
        frequency = sum(a <= 0 < b for a,b in zip(signal,signal[1:]))/duration
        if not amplitude*.95 <= rms <= amplitude*1.05: raise ValueError(f"wrong recording {name} RMS: {rms:.1f}")
        if abs(frequency-hz) > 10: raise ValueError(f"wrong recording {name} frequency: {frequency:.1f}")
        gap = longest = 0
        for sample in signal:
            gap = gap+1 if abs(sample) < 100 else 0
            longest = max(longest,gap)
        if longest > 960: raise ValueError(f"recording {name} contains a silent hole")
        result[name+"_rms"] = round(rms); result[name+"_hz"] = round(frequency,1)
    return result
