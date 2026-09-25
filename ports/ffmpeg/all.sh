#!/bin/bash
# Run the pinned upstream FFmpeg fetch/configure/make/link pipeline.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
for step in fetch runtime configure make link; do
    echo
    echo "############ $step ############"
    bash "$HERE/$step.sh"
done
