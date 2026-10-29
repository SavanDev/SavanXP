#!/usr/bin/env python3
"""Host QMP keys for the elevation smoke.

The guest (`elevtest`) asks for elevation four times as the user. windowd
prints "consent prompt shown" when the first prompt is up, which is when this
runs:

  1. N declines the first request.
  2. Y is held down -- one make code after another, as a keyboard repeats --
     from before the second prompt opens until well after it arms, then
     released; N declines it. Repeats must not say yes.
  3. Y accepts the third.
  4. Y accepts the fourth, `appwiz --uninstall`, whose arguments the prompt
     shows; its screenshot is kept.

The keys travel QMP -> PS/2 (or virtio) -> driver -> /dev/input0 -> windowd,
the only path an answer may take. Each pause outlasts the round trip to the
next prompt and its arming delay (WINDOWD_CONSENT_ARM_MS).
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

    args.out_dir.mkdir(parents=True, exist_ok=True)
    with QmpClient(args.socket, timeout=5.0) as client:
        time.sleep(1.2)
        # The prompt as the user saw it, kept with the smoke's logs.
        client.screenshot(args.out_dir / "prompt.ppm")
        client.tap("n", pause=0.05)
        # Held: the second prompt opens and arms while only repeats arrive.
        for _ in range(30):
            client.key("y", True)
            time.sleep(0.1)
        client.key("y", False)
        time.sleep(0.3)
        client.tap("n", pause=0.0)
        time.sleep(4.0)
        client.tap("y", pause=0.0)
        time.sleep(4.0)
        client.screenshot(args.out_dir / "prompt-arguments.ppm")
        client.tap("y", pause=0.0)
    print("qmp_elevation: sent n, a held y then n, y, y", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
