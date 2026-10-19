#!/usr/bin/env python3
"""Host QMP keys for the native hardware-input smoke.

The guest (`windowd --hw-native-test`) prints NATIVE HW READY once the
native form is up with its field focused, then drains /dev/input0 through
the production path. These taps travel QMP -> PS/2 -> driver -> input
ring -> native client: h and i must turn the typed square green, Esc
must close the form. Generous pauses: the guest samples the composed
framebuffer, not the key queue.
"""

from __future__ import annotations

import argparse
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from qmp_client import QmpClient  # noqa: E402


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--socket", type=Path, required=True, help="QMP Unix socket")
    parser.add_argument("--out-dir", type=Path, required=True)
    args = parser.parse_args()

    with QmpClient(args.socket, timeout=5.0) as client:
        time.sleep(0.5)
        client.tap("h", pause=0.6)
        client.tap("i", pause=0.8)
        client.tap("esc", pause=0.0)
    print("qmp_native_hw: sent h, i, esc", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
