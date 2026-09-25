#!/bin/bash
# Compile and statically link the Media Player against the FFmpeg libraries.
set -euo pipefail
. "$(dirname "${BASH_SOURCE[0]}")/env.sh"

APP="$PORT/overlay/mediaplayer"
BACKEND="$PORT/overlay/sxmedia"
mkdir -p "$OUT" "$OUTPUT_ROOT/external"
cd "$OUT"

PYTHON="${SAVANXP_PYTHON:-python3}"
command -v "$PYTHON" >/dev/null 2>&1 || {
    echo "ffmpeg link: hace falta python3 (SAVANXP_PYTHON)" >&2
    exit 1
}

# La tabla de capacidades: lo que este build puede decodificar, preguntado a la
# configuracion que se uso y no escrito a mano. Va al directorio de build y no al
# overlay porque es una funcion de como se configuro FFmpeg; una copia en el repo
# seria una promesa sobre un build que nadie hizo.
echo "== generando la tabla de capacidades de SxMedia"
"$PYTHON" "$PORT/tools/gen_sxmedia_caps.py" \
    --config-components "$BUILD/config_components.h" \
    --codec-desc "$SRC/libavcodec/codec_desc.c" \
    --output "$BUILD/sxmedia_ffmpeg_caps.inc"

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
