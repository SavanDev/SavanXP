"""A deterministic QEMU microphone, isolated from host audio devices.

ALSA's file PCM reads a raw input file over a null slave. The two channels have
independent square waves (400/600 Hz, amplitudes 12000/6000) at 48 kHz S16LE.
The guest's capture assertions know this signal independently of this module.
Only QEMU receives the resulting ALSA_CONFIG_PATH, never the user's session.
"""
from __future__ import annotations

import json
import struct
from pathlib import Path


def prepare_capture_fixture(directory: Path, tag: str, output: Path) -> Path:
    directory.mkdir(parents=True, exist_ok=True)
    source = directory / f"{tag}.microphone.raw"
    config = directory / f"{tag}.alsa.conf"
    # 240 frames contain whole cycles of both channels. Five minutes also
    # covers a visual scenario which starts recording late in its session.
    period = b"".join(struct.pack("<hh", 12000 if i % 120 < 60 else -12000,
                                6000 if i % 80 < 40 else -6000) for i in range(240))
    with source.open("wb") as handle:
        for _ in range(48000 * 300 // 240):
            handle.write(period)
    quote = lambda path: json.dumps(str(path.resolve()))
    config.write_text(
        "pcm.savanxp_fixture_input {\n"
        "  type file\n  slave.pcm { type null }\n  file \"/dev/null\"\n"
        f"  infile {quote(source)}\n  format \"raw\"\n}}\n"
        "pcm.savanxp_fixture_output {\n"
        "  type file\n  slave.pcm { type null }\n"
        f"  file {quote(output)}\n  format \"wav\"\n}}\n",
        encoding="utf-8",
    )
    return config
