#!/bin/bash
# Compila el reproductor y lo linkea contra las libav* recien construidas,
# dejando build/external/mediaplayer.elf listo para estampar e instalar.
#
# Es UN solo binario a proposito: el link estatico mete en cada ejecutable todos
# los decoders habilitados -- las tablas de allcodecs los referencian a todos --,
# asi que cada programa que linkee libavcodec pesa megas. Las herramientas de
# prueba viven como modos del reproductor (--probe, --selftest, --gpu-hold).
set -euo pipefail
. "$(dirname "${BASH_SOURCE[0]}")/env.sh"

APP="$PORT/mediaplayer"
mkdir -p "$OUT" "$REPO/build/external"
cd "$OUT"

objects=()
for source in "$APP"/*.c; do
    name="$(basename "$source" .c)"
    echo "== compilando $name.c"
    $SX_CC -c -x c "$source" -o "$name.o" $SX_TARGET_CFLAGS \
        -Wall -Wextra -Wno-unused-parameter \
        -I "$SRC" -I "$BUILD"
    objects+=("$name.o")
done

# El orden importa: las libav* se referencian entre si, el toolkit usa la libc y
# el runtime va ultimo.
echo "== linkeando mediaplayer"
$SX_LD $SX_TARGET_LDFLAGS -o mediaplayer.elf \
    "$RUNTIME/crt0.o" "${objects[@]}" \
    "$BUILD/libavformat/libavformat.a" \
    "$BUILD/libavcodec/libavcodec.a" \
    "$BUILD/libswscale/libswscale.a" \
    "$BUILD/libswresample/libswresample.a" \
    "$BUILD/libavutil/libavutil.a" \
    "$RUNTIME/libsxgui.a" \
    "$RUNTIME/libsavanxp.a"

echo "== mediaplayer.elf: $(du -h mediaplayer.elf | cut -f1)"
llvm-size mediaplayer.elf | tail -1
cp mediaplayer.elf "$REPO/build/external/mediaplayer.elf"
echo "listo: $REPO/build/external/mediaplayer.elf"
