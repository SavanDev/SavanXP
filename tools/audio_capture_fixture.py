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


def prepare_capture_fixture(directory: Path, tag: str, output: Path, *, devices: int = 1) -> Path:
    directory.mkdir(parents=True, exist_ok=True)
    config = directory / f"{tag}.alsa.conf"
    quote = lambda path: json.dumps(str(path.resolve()))
    text = ""
    signals = ((120,80,12000,6000,240),(96,48,8000,4000,96),(160,64,10000,2000,320))
    for index, (left,right,a,b,length) in enumerate(signals[:devices]):
        suffix = "" if index == 0 else str(index+1)
        source = directory / f"{tag}.microphone{suffix}.raw"
        # Whole cycles, with five minutes for late visual recording actions.
        period = b"".join(struct.pack("<hh", a if i%left < left//2 else -a,
                                    b if i%right < right//2 else -b) for i in range(length))
        with source.open("wb") as handle:
            for _ in range(48000*300//length): handle.write(period)
        destination = output if not suffix else output.with_name(output.stem+suffix+output.suffix)
        text += (
            f"pcm.savanxp_fixture_input{suffix} {{\n"
            "  type file\n  slave.pcm { type null }\n  file \"/dev/null\"\n"
            f"  infile {quote(source)}\n  format \"raw\"\n}}\n"
            f"pcm.savanxp_fixture_output{suffix} {{\n"
            "  type file\n  slave.pcm { type null }\n"
            f"  file {quote(destination)}\n  format \"wav\"\n}}\n")
    config.write_text(text,encoding="utf-8")
    return config
