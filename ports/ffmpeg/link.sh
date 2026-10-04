#!/bin/bash
# Compile and statically link the Media Player against the FFmpeg libraries.
set -euo pipefail
. "$(dirname "${BASH_SOURCE[0]}")/env.sh"

APP="$PORT/overlay/mediaplayer"
mkdir -p "$OUT" "$OUTPUT_ROOT/external"
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

# ---- libffmpeg.so.0.4 -----------------------------------------------------
#
# Una sola libreria con los cinco componentes, y no cinco librerias. La division
# en varios .so es una comodidad de distribucion --cambiar un codec sin relincar
# todo-- y este SO no tiene nada que ganar con eso. Ademas, una sola da la prueba
# mas fuerte del cargador: una .dynsym grande y una tabla de reubicaciones grande,
# que es exactamente lo que no se ha ejercitado hasta ahora.
#
# Dos cosas que hay que saber y que costaron una hora cada una:
#
# Los .a NO se linkean solos. Nadie los referencia y la semantica de archivo solo
# extrae objetos que resuelven un simbolo indefinido, asi que el primer intento
# produjo una libreria VACIA de 864 bytes con un aviso. Necesita --whole-archive.
#
# Y los flags de SX_TARGET_LDFLAGS no sirven aqui: -static, -no-pie y el guion
# Vinculo -T, que es para el ejecutable. Una libreria compartida se enla sin
# guion de enlace y con -Wl,-shared en vez de -shared (ver mas abajo).
#
# Los 112 simbolos que quedan sin definir NO son un error: los resuelve el cargador
# contra el ejecutable, que los exporta con --export-dynamic, exactamente como
# hace con libsxgfx. Por eso --unresolved-symbols=ignore-all.
#
# Y --whole-archive no cuesta lo que parece. Podria tentarse acoplarlo con -u a las
# 48 entradas de FFmpeg que usa el reproductor y dejar que la semantica de archivo
# traiga el cierre transitivo, para no meter codigo muerto: medido, da EXACTAMENTE
# lo mismo, 6.8 MB y los mismos 17.99 MB de BSS. El registro de codecs de FFmpeg es
# una tabla que referencia todos los decodificadores, asi que el cierre llega igual
# a las tablas de transformada. Asi que --whole-archive, que es mas robusto.
ARCHIVES=(
    "$BUILD/libavformat/libavformat.a"
    "$BUILD/libavcodec/libavcodec.a"
    "$BUILD/libswscale/libswscale.a"
    "$BUILD/libswresample/libswresample.a"
    "$BUILD/libavutil/libavutil.a"
)

echo "== linkeando libffmpeg.so.0.4"
# -Wl,-shared y NO -shared. El driver de clang con el triple none ignora -shared
# (avisa "argument unused during compilation") y linkea un ET_EXEC etiquetado como
# libreria. El guardia de read_header en ldso.c lo rechaza por tipo, que es lo
# correcto, pero es mejor no construir el archivo en esa forma.
$LINK_CC -target x86_64-unknown-none-elf -nostdlib -Wl,-shared -fuse-ld=lld \
    -Wl,--whole-archive "${ARCHIVES[@]}" -Wl,--no-whole-archive \
    -Wl,--unresolved-symbols=ignore-all \
    -Wl,-soname,libffmpeg.so.0.4 \
    -Wl,-z,max-page-size=0x1000 -Wl,--build-id=none \
    -o libffmpeg.so.0.4

echo "== libffmpeg.so.0.4: $(du -h libffmpeg.so.0.4 | cut -f1)"
$SIZE_CMD libffmpeg.so.0.4 | tail -1
cp libffmpeg.so.0.4 "$OUTPUT_ROOT/external/libffmpeg.so.0.4"
echo "listo: $OUTPUT_ROOT/external/libffmpeg.so.0.4"

# ---- mediaplayer: PIE, con libffmpeg.so.0.4 como dependencia ---------------
#
# Aqui es donde el reproductor deja de llevar los cinco .a dentro. Es la primera
# aplicacion del sistema que depende de una libreria compartida, y la razon de que
# sea PIE es concreta: el ejecutable tiene que estar en el ambito de simbolos de
# libffmpeg, porque las 112 referencias externas de la libreria las resuelve el
# cargador contra el ejecutable y no hay mas sitio donde buscarlas.
#
# El orden NO es negociable: la libreria se linkea antes, porque el enlazador necesita el
# archivo para poder escribir el DT_NEEDED.
#
# Y se pasa POR SU NOMBRE, no con -lffmpeg. El archivo se llama libffmpeg.so.0.4 y -l
# busca libffmpeg.so o libffmpeg.a; ademas, pasando la ruta, es el SONAME del archivo
# --no su nombre de archivo-- lo que queda en el DT_NEEDED, y asi el reproductor
# pedira libffmpeg.so.0.4 aunque el archivo se llame de otra forma en el volumen.
#
# Lo que NO entra: libsxgui.a y libsavanxp.a si, pero las de FFmpeg no. Sus 112
# simbolos se resuelven en tiempo de carga, no de enlace.
echo "== linkeando mediaplayer (PIE, depende de libffmpeg.so.0.4)"
$SX_LD $SX_TARGET_LDFLAGS_PIE -o mediaplayer.elf \
    "$RUNTIME/crt0.o" "${objects[@]}" \
    "$OUT/libffmpeg.so.0.4" \
    "$RUNTIME/ldso.o" \
    "$RUNTIME/libsxgui.a" \
    "$RUNTIME/libsavanxp.a"

echo "== mediaplayer.elf: $(du -h mediaplayer.elf | cut -f1)"
$SIZE_CMD mediaplayer.elf | tail -1
readelf -dW mediaplayer.elf | grep -E 'NEEDED' | sed 's/^/   /'
cp mediaplayer.elf "$OUTPUT_ROOT/external/mediaplayer.elf"
echo "listo: $OUTPUT_ROOT/external/mediaplayer.elf"
