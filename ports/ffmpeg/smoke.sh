#!/usr/bin/env bash
# Run the FFmpeg port's two-phase Media Player smoke.
#
# Phase one decodes the generated WAV/MJPEG/AVI media without a window.
# Phase two asks the port to present a real frame through /dev/gpu0; the
# generic Linux smoke runner supplies QMP and the port-owned display callback
# checks that the held frame is not a blank screen.
set -euo pipefail

HERE="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd -- "$HERE/../.." && pwd)"
OUTPUT_ROOT="${SAVANXP_OUTPUT_ROOT:-$ROOT/build}"
BUILD_DIR="${SAVANXP_BUILD_DIR:-$ROOT/build/linux}"
JOBS="${SAVANXP_JOBS:-}"
ACCEL="${SAVANXP_ACCEL:-tcg}"
SMP="${SAVANXP_SMP:-1}"
VIRTIO="${SAVANXP_VIRTIO:-OFF}"
TIMEOUT="${SAVANXP_SMOKE_TIMEOUT:-}"
WORK="${SAVANXP_FFMPEG_WORK:-}"
SKIP_PORT=0
NO_COMPACT=0

usage() {
    cat <<'EOF'
Usage: ./ports/ffmpeg/smoke.sh [options]

Options:
  --skip-port       Reuse build/external/mediaplayer.elf and only install it.
  --work PATH       Pass a whitespace-free FFmpeg work directory to the build.
  --no-compact      Disable SxFS compaction while installing the port.
  --accel tcg|kvm   Select the QEMU accelerator for both smoke boots.
  --smp N           Set the number of virtual CPUs for both smoke boots.
  --virtio          Use QEMU virtio graphics, input and storage.
  --timeout SEC     Override the catalog timeout for both smoke boots.
  --jobs N          Pass a parallelism limit to the base build.
  -h, --help        Show this help.
EOF
}

while (($#)); do
    case "$1" in
        --skip-port) SKIP_PORT=1; shift ;;
        --work)
            (($# >= 2)) || { echo "ffmpeg smoke: --work requires a value" >&2; exit 2; }
            WORK=$2
            shift 2
            ;;
        --no-compact) NO_COMPACT=1; shift ;;
        --accel)
            (($# >= 2)) || { echo "ffmpeg smoke: --accel requires a value" >&2; exit 2; }
            ACCEL=$2
            shift 2
            ;;
        --smp)
            (($# >= 2)) || { echo "ffmpeg smoke: --smp requires a value" >&2; exit 2; }
            SMP=$2
            shift 2
            ;;
        --virtio) VIRTIO=ON; shift ;;
        --timeout)
            (($# >= 2)) || { echo "ffmpeg smoke: --timeout requires a value" >&2; exit 2; }
            TIMEOUT=$2
            shift 2
            ;;
        --jobs)
            (($# >= 2)) || { echo "ffmpeg smoke: --jobs requires a value" >&2; exit 2; }
            JOBS=$2
            shift 2
            ;;
        -h|--help) usage; exit 0 ;;
        *) echo "ffmpeg smoke: unknown option '$1'" >&2; usage >&2; exit 2 ;;
    esac
done

case "$ACCEL" in
    tcg|kvm) ;;
    *) echo "ffmpeg smoke: --accel must be tcg or kvm" >&2; exit 2 ;;
esac
case "$SMP" in
    ''|*[!0-9]*) echo "ffmpeg smoke: --smp must be an integer between 1 and 32" >&2; exit 2 ;;
esac
((SMP >= 1 && SMP <= 32)) || {
    echo "ffmpeg smoke: --smp must be between 1 and 32" >&2
    exit 2
}

export SAVANXP_OUTPUT_ROOT="$OUTPUT_ROOT"
export SAVANXP_BUILD_DIR="$BUILD_DIR"
export SAVANXP_ACCEL="$ACCEL"
export SAVANXP_SMP="$SMP"
export SAVANXP_VIRTIO="$VIRTIO"
[[ -z "$JOBS" ]] || export SAVANXP_JOBS="$JOBS"
[[ -z "$TIMEOUT" ]] || export SAVANXP_SMOKE_TIMEOUT="$TIMEOUT"

# Make the command self-contained on a clean checkout: the port installer needs
# the base disk image and sxfs helper, and this also leaves no stale smoke
# specification before the port is installed.
"$ROOT/build.sh" build

port_args=(--with-test-media)
((NO_COMPACT == 0)) || port_args+=(--no-compact)
if ((SKIP_PORT)); then
    [[ -z "$WORK" ]] || { echo "ffmpeg smoke: --work cannot be combined with --skip-port" >&2; exit 2; }
    bash "$HERE/install.sh" "${port_args[@]}"
else
    [[ -z "$WORK" ]] || port_args+=(--work "$WORK")
    bash "$HERE/build.sh" "${port_args[@]}"
fi

smoke_args=()
[[ -z "$TIMEOUT" ]] || smoke_args+=(--timeout "$TIMEOUT")

# Each inner smoke call stages its own initramfs specification and uses the
# disposable-image lock. A normal build in the trap removes the final /SMOKE
# marker and leaves the persistent image in the same state as a normal build.
cleanup() {
    local status=$?
    trap - EXIT
    if ! "$ROOT/build.sh" build; then
        echo "ffmpeg smoke: cleanup build failed" >&2
        status=1
    fi
    exit "$status"
}
trap cleanup EXIT

"$ROOT/build.sh" smoke mediaplayer-selftest "${smoke_args[@]}"
"$ROOT/build.sh" smoke mediaplayer-display "${smoke_args[@]}"

# Cleanup is part of the test contract: do not leave an automation spec in the
# initramfs after a successful two-phase run.
trap - EXIT
"$ROOT/build.sh" build
echo "FFMPEG SMOKE PASS"
