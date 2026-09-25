#!/usr/bin/env bash
# Restore the optional Doom ELF into a disposable smoke image when it exists.
set -euo pipefail

ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
OUTPUT_ROOT="${SAVANXP_OUTPUT_ROOT:-$ROOT/build}"
PYTHON="${SAVANXP_PYTHON:-python3}"
ELF="${SAVANXP_DOOM_ELF:-$OUTPUT_ROOT/external/doomgeneric.elf}"
IMAGE="${SAVANXP_DISK_IMAGE:-$OUTPUT_ROOT/disk.img}"
CLI="${SAVANXP_SXFS_CLI:-$OUTPUT_ROOT/tools/sxfs-cli}"

if [[ ! -f "$ELF" ]]; then
    echo "doom: no external ELF; leaving the image unchanged"
    exit 0
fi
[[ -f "$IMAGE" && -x "$CLI" ]] || {
    echo "doom: valid disk image and sxfs-cli are required" >&2
    exit 1
}

stage="$OUTPUT_ROOT/restore-doom-stage"
rm -rf "$stage"
mkdir -p "$stage/bin"
cp "$ELF" "$stage/bin/doomgeneric"
"$PYTHON" "$ROOT/tools/sxfs_sync.py" \
    --image "$IMAGE" \
    --source "$stage" \
    --cli "$CLI" \
    --sectors "${SAVANXP_SXFS_SECTORS:-131072}"
rm -rf "$stage"
echo "doom: restored /disk/bin/doomgeneric"
