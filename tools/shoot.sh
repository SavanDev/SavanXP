#!/usr/bin/env bash
# Capture and verify the minimal i386 desktop through the current QMP harness.
set -euo pipefail
ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
OUTPUT_ROOT=${SAVANXP_OUTPUT_ROOT:-"$ROOT/build"}
BUILD_DIR=${SAVANXP_BUILD_DIR:-"$ROOT/build/linux-i386"}
OUT_DIR="$OUTPUT_ROOT/shots"
ACCEL=${SAVANXP_ACCEL:-tcg}
while (($#)); do
    case "$1" in
        --scenario)
            [[ "${2:?--scenario requires a value}" == desktop ]] || {
                echo "shoot.sh: only the minimal i386 desktop is ported; use master for the full desktop scenarios" >&2
                exit 2
            }
            shift 2 ;;
        --out-dir) OUT_DIR=${2:?--out-dir requires a value}; shift 2 ;;
        --accel) ACCEL=${2:?--accel requires a value}; shift 2 ;;
        -h|--help)
            echo "Usage: tools/shoot.sh [--scenario desktop] [--out-dir PATH] [--accel tcg|kvm]"
            exit 0 ;;
        *) echo "shoot.sh: unsupported i386 option '$1'" >&2; exit 2 ;;
    esac
done
"$ROOT/build.sh" iso
exec "${SAVANXP_PYTHON:-python3}" "$ROOT/tools/run_smoke.py" \
    --scenario desktop --iso "$OUTPUT_ROOT/SavanXP.iso" \
    --disk-image "$OUTPUT_ROOT/disk.img" --desktop-elf "$BUILD_DIR/desktop32" \
    --artifacts "$OUT_DIR" --accel "$ACCEL"
