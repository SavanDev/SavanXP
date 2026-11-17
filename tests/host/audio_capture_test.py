#!/usr/bin/env python3
"""The capture verifier must distinguish changing FM audio from stale PLC."""
import array
import math
from pathlib import Path
import sys
import tempfile
sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
from shoot_session import _wav_residue_s

rate = 48000
with tempfile.TemporaryDirectory() as tmp:
    for plateau in (.05, .5):
        # Forty-Hz audio with a DC offset never crosses zero. It is still
        # real changing audio, unlike audiod's repeated last-frame PLC.
        music = [int(4000 + 2000 * math.sin(2 * math.pi * 40 * i / rate)) for i in range(rate)]
        mono = music + [music[-1]] * int(plateau * rate) + [0] * rate
        stereo = array.array("h", (value for sample in mono for value in (sample, sample)))
        path = Path(tmp) / "capture.wav"
        path.write_bytes(b"RIFF" + bytes(32) + b"data" + bytes(4) + stereo.tobytes())
        residue = _wav_residue_s(path)
        assert abs(residue - plateau) <= .021, (plateau, residue)
        assert (residue <= .15) == (plateau == .05), (plateau, residue)
print("captured audio / stale PLC discrimination: PASS")
