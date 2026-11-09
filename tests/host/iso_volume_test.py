#!/usr/bin/env python3
"""The LiveCD volume preserves file contents and recorded permissions."""

from __future__ import annotations

import argparse
import hashlib
import subprocess
import sys
import tempfile
from pathlib import Path


def run(*command: str) -> None:
    result = subprocess.run(command, text=True, capture_output=True, check=False)
    if result.returncode != 0:
        raise RuntimeError(f"{' '.join(command)}\n{result.stdout}\n{result.stderr}")


def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--cli", required=True, type=Path)
    parser.add_argument("--stage", required=True, type=Path)
    args = parser.parse_args()
    cli = str(args.cli.resolve())
    stage = str(args.stage.resolve())
    with tempfile.TemporaryDirectory(prefix="savanxp-iso-volume-test.") as temporary:
        root = Path(temporary)
        payload = root / "payload"
        payload.write_bytes(b"persistent LiveCD fixture\n")
        image = root / "source.img"
        manifest = root / "seed.manifest"
        manifest.write_text(
            "mkdir\tsystem\nmkdir\tbin\nmkdir\tgames\n"
            "mkdir\tusers\nmkdir\tusers/user\n"
            f"file\tsystem/sessiond.sxe\t{payload}\n"
            f"file\tsystem/preferences.cfg\t{payload}\n"
            f"file\tbin/external\t{payload}\n"
            f"file\tgames/save.dat\t{payload}\n"
            f"file\tusers/user/private.txt\t{payload}\n"
            "owner\tsystem/sessiond.sxe\t0\t0\t100755\n"
            "owner\tsystem/preferences.cfg\t0\t0\t100640\n"
            "owner\tbin/external\t1000\t100\t100750\n"
            "owner\tgames\t1000\t100\t40750\n"
            "owner\tgames/save.dat\t1000\t100\t100600\n"
            "owner\tusers/user/private.txt\t1000\t100\t100400\n",
            encoding="utf-8",
        )
        run(cli, "create", str(image), "16384")
        run(cli, "apply", str(image), str(manifest))
        before_hash = digest(image)
        before = root / "before"
        before_owners = root / "before.owners"
        run(cli, "extract", str(image), str(before), str(before_owners))
        expected_owners = set(before_owners.read_text().splitlines())
        for sectors in (16384, 8192):
            output = root / f"iso-{sectors}.img"
            run(sys.executable, stage, "--cli", cli, "--image", str(image),
                "--output", str(output), "--sectors", str(sectors))
            after = root / f"after-{sectors}"
            after_owners = root / f"after-{sectors}.owners"
            run(cli, "extract", str(output), str(after), str(after_owners))
            assert output.stat().st_size == sectors * 512
            assert set(after_owners.read_text().splitlines()) == expected_owners, (
                f"ISO staging changed ownership or permissions at {sectors} sectors"
            )
            for path in before.rglob("*"):
                if path.is_file():
                    assert path.read_bytes() == (after / path.relative_to(before)).read_bytes()
            assert digest(image) == before_hash, "ISO staging modified the source image"
    print("ISO volume contents and ownership: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
