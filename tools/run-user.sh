#!/usr/bin/env bash
# Build, install and boot one external POSIX application.
set -euo pipefail

ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
SOURCE=""
NAME=""
DESTINATION=""
GUI=0
SSE=0
AUDIO=0
NO_COMPACT=0
HEAP_MIB=""

usage() {
    cat <<'EOF'
Usage: tools/run-user.sh --source PATH [options]

Options:
  --name NAME          Program name (defaults to the source name).
  --destination PATH   Install below /disk (default: /disk/bin/NAME).
  --gui                 Link the SXGUI runtime.
  --sse                 Enable the SSE/SSE2 ABI and math runtime.
  --audio               Link the PCM audio runtime.
  --heap-mib N          Set the fixed heap size in MiB.
  --no-compact          Disable SxFS compaction for installation.
  -h, --help            Show this help.
EOF
}

while (($#)); do
    case "$1" in
        --source)
            (($# >= 2)) || { echo "run-user.sh: --source requires a value" >&2; exit 2; }
            SOURCE=$2
            shift 2
            ;;
        --name)
            (($# >= 2)) || { echo "run-user.sh: --name requires a value" >&2; exit 2; }
            NAME=$2
            shift 2
            ;;
        --destination)
            (($# >= 2)) || { echo "run-user.sh: --destination requires a value" >&2; exit 2; }
            DESTINATION=$2
            shift 2
            ;;
        --gui) GUI=1; shift ;;
        --sse) SSE=1; shift ;;
        --audio) AUDIO=1; shift ;;
        --heap-mib)
            (($# >= 2)) || { echo "run-user.sh: --heap-mib requires a value" >&2; exit 2; }
            HEAP_MIB=$2
            shift 2
            ;;
        --no-compact) NO_COMPACT=1; shift ;;
        -h|--help) usage; exit 0 ;;
        *) echo "run-user.sh: unknown option '$1'" >&2; usage >&2; exit 2 ;;
    esac
done

[[ -n "$SOURCE" ]] || { echo "run-user.sh: --source is required" >&2; exit 2; }
args=(--source "$SOURCE")
[[ -z "$NAME" ]] || args+=(--name "$NAME")
[[ -z "$DESTINATION" ]] || args+=(--destination "$DESTINATION")
((GUI == 0)) || args+=(--gui)
((SSE == 0)) || args+=(--sse)
((AUDIO == 0)) || args+=(--audio)
[[ -z "$HEAP_MIB" ]] || args+=(--heap-mib "$HEAP_MIB")
((NO_COMPACT == 0)) || args+=(--no-compact)

bash "$ROOT/tools/build-user.sh" "${args[@]}"
exec "$ROOT/build.sh" run
