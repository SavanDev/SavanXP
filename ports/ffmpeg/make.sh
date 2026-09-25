#!/bin/bash
# Build FFmpeg's generated Makefile with GNU Make.
set -uo pipefail
. "$(dirname "${BASH_SOURCE[0]}")/env.sh"

LOG="$WORK/make.log"
export TMPDIR="$WORK/tmp"
mkdir -p "$TMPDIR"

cd "$BUILD"
echo "== $MAKE_CMD -k -j$(sx_jobs)"
"$MAKE_CMD" -k -j"$(sx_jobs)" >"$LOG" 2>&1
status=$?

echo "== make salio con $status"
if [ "$status" -ne 0 ]; then
    echo
    echo "===== errores distintos ====="
    grep -oE "error: .*" "$LOG" | sed 's/ \[-W.*//' | sort | uniq -c | sort -rn | head -40 || true
    echo
    echo "===== archivos que fallaron ====="
    grep -oE "[^ ]+\.c:[0-9]+:[0-9]+: error" "$LOG" | sed -E 's/:[0-9]+:[0-9]+: error$//' | sort -u | head -30 || true
    echo
    echo "log completo en $LOG"
fi
exit "$status"
