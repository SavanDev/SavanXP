#!/bin/bash
# Compila FFmpeg contra SavanXP y resume que fallo.
#
# Corre con -k a proposito: en la primera pasada interesa el INVENTARIO de lo
# que falta, no el primer error. Arreglar de a uno pagando un build entero por
# vez es mucho mas lento que juntar la lista y hacer una tanda.
set -uo pipefail

FFMPEG_VERSION="${FFMPEG_VERSION:-7.1.1}"
WORK="${WORK:-$HOME/savanxp-ffmpeg}"
BUILD="$WORK/build"
LOG="$WORK/make.log"

cd "$BUILD"
echo "== make -k -j$(nproc)"
make -k -j"$(nproc)" >"$LOG" 2>&1
status=$?

echo "== make salio con $status"
echo
echo "===== errores distintos ====="
grep -oE "error: .*" "$LOG" | sed 's/ \[-W.*//' | sort | uniq -c | sort -rn | head -40
echo
echo "===== archivos que fallaron ====="
grep -oE "^[a-z0-9_/]+\.c:[0-9]+:[0-9]+: error" "$LOG" | cut -d: -f1 | sort -u | head -30
echo
echo "===== total de lineas de error ====="
grep -c "error:" "$LOG"
echo "log completo en $LOG"
exit $status
