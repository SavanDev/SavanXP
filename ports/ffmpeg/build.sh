#!/usr/bin/env bash
# Build and install the official SavanXP FFmpeg Media Player port.
set -euo pipefail

HERE="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
NO_INSTALL=0
SKIP_PORT=0
WITH_TEST_MEDIA=0
NO_COMPACT=0
WORK_OVERRIDE=""

usage() {
    cat <<'EOF'
Usage: ./ports/ffmpeg/build.sh [options]

Options:
  --no-install       Build and stamp without touching build/disk.img.
  --skip-port        Skip FFmpeg compilation and stamp/install the existing ELF.
  --with-test-media  Generate and install the three deterministic test clips.
  --no-compact       Disable automatic SxFS compaction during installation.
  --work PATH        Override the whitespace-free FFmpeg work directory.
  -h, --help         Show this help.
EOF
}

while (($#)); do
    case "$1" in
        --no-install) NO_INSTALL=1; shift ;;
        --skip-port) SKIP_PORT=1; shift ;;
        --with-test-media) WITH_TEST_MEDIA=1; shift ;;
        --no-compact) NO_COMPACT=1; shift ;;
        --work)
            (($# >= 2)) || { echo "ffmpeg: --work requires a path" >&2; exit 2; }
            WORK_OVERRIDE=$2
            shift 2
            ;;
        -h|--help) usage; exit 0 ;;
        *) echo "ffmpeg: unknown option '$1'" >&2; usage >&2; exit 2 ;;
    esac
done

if [[ -n "$WORK_OVERRIDE" ]]; then
    export WORK="$WORK_OVERRIDE"
fi

if ((SKIP_PORT == 0)); then
    bash "$HERE/all.sh"
fi

args=()
if ((NO_COMPACT)); then args+=(--no-compact); fi
if ((NO_INSTALL)); then args+=(--no-install); fi
if ((WITH_TEST_MEDIA)); then args+=(--with-test-media); fi
bash "$HERE/install.sh" "${args[@]}"
