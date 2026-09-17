#!/bin/bash
# Compila las bibliotecas de FFmpeg contra SavanXP y resume que fallo.
#
# Corre con -k a proposito: cuando se agranda el set de formatos, lo que
# interesa es el INVENTARIO de lo que falta en la libc, no el primer error.
# Arreglar de a uno pagando un build entero por vez es mucho mas lento que
# juntar la lista y hacer una tanda.
set -uo pipefail
. "$(dirname "${BASH_SOURCE[0]}")/env.sh"

MAKE="${MAKE:-make}"
LOG="$WORK/make.log"
export TMPDIR="$WORK/tmp"
mkdir -p "$TMPDIR"

cd "$BUILD"
echo "== $MAKE -k -j$(sx_jobs)"
"$MAKE" -k -j"$(sx_jobs)" >"$LOG" 2>&1
status=$?

echo "== make salio con $status"
if [ $status -ne 0 ]; then
    echo
    echo "===== errores distintos ====="
    grep -oE "error: .*" "$LOG" | sed 's/ \[-W.*//' | sort | uniq -c | sort -rn | head -40
    echo
    echo "===== archivos que fallaron ====="
    grep -oE "[^ ]+\.c:[0-9]+:[0-9]+: error" "$LOG" | sed -E 's/:[0-9]+:[0-9]+: error$//' | sort -u | head -30
    echo
    echo "log completo en $LOG"
fi
exit $status
