#!/bin/bash
# Compile and statically link the Media Player against the FFmpeg libraries.
set -euo pipefail
. "$(dirname "${BASH_SOURCE[0]}")/env.sh"

APP="$PORT/overlay/mediaplayer"
BACKEND="$PORT/overlay/sxmedia"
mkdir -p "$OUT" "$OUTPUT_ROOT/external"
cd "$OUT"

objects=()
for source in "$APP"/*.c; do
    name="$(basename "$source" .c)"
    echo "== compilando $name.c"
    $SX_CC -c -x c "$source" -o "$name.o" $SX_TARGET_CFLAGS \
        -Wall -Wextra -Wno-unused-parameter \
        -I "$BACKEND" -I "$SRC" -I "$BUILD"
    objects+=("$name.o")
done

# El backend de SxMedia: el unico archivo que le dice a una libreria como ser un
# backend. El engine (`sxmedia.c`) entra por libsavanxp.a, que runtime.sh ya
# compila; lo que se compila aca es el lado FFmpeg del vtable.
echo "== compilando sxmedia_ffmpeg.c"
$SX_CC -c -x c "$BACKEND/sxmedia_ffmpeg.c" -o sxmedia_ffmpeg.o $SX_TARGET_CFLAGS \
    -Wall -Wextra -Wno-unused-parameter \
    -I "$BACKEND" -I "$SRC" -I "$BUILD"
objects+=("sxmedia_ffmpeg.o")

# The order is part of the static-link contract.
echo "== linkeando mediaplayer"
$SX_LD $SX_TARGET_LDFLAGS -o mediaplayer.elf \
    "$RUNTIME/crt0.o" "${objects[@]}" \
    "$BUILD/libavformat/libavformat.a" \
    "$BUILD/libavcodec/libavcodec.a" \
    "$BUILD/libswscale/libswscale.a" \
    "$BUILD/libswresample/libswresample.a" \
    "$BUILD/libavutil/libavutil.a" \
    "$RUNTIME/libsxgui.a" \
    "$RUNTIME/libsxcodecs.a" \
    "$RUNTIME/libsavanxp.a"

echo "== mediaplayer.elf: $(du -h mediaplayer.elf | cut -f1)"
$SIZE_CMD mediaplayer.elf | tail -1
cp mediaplayer.elf "$OUTPUT_ROOT/external/mediaplayer.elf"
echo "listo: $OUTPUT_ROOT/external/mediaplayer.elf"
