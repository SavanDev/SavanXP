#!/bin/bash
# Link libffmpeg.so.0.4 out of the FFmpeg archives. This port builds NO program.
set -euo pipefail
. "$(dirname "${BASH_SOURCE[0]}")/env.sh"

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

# El reproductor NO se construye aqui. Vive en subsystems/posix/userland/mediaplayer/
# y lo enlaza el arbol, que es donde estan la ventana, los controles y el reloj; este
# port solo aporta el motor. Antes lo compilaba este script y lo instalaba como
# /disk/bin/mediaplayer-ffmpeg, con un lanzador de 114 lineas en el arbol que hacia
# exec de este binario. Los dos se fueron: el motor es la libreria, y el programa es
# del arbol.
# libffmpeg DECLARA que necesita libmath, y no es cosmetico.
#
# El cargador recorre el DT_NEEDED del ejecutable en orden y, en cuanto trae una
# libreria, la reubica antes de traer la siguiente. Asi que si libffmpeg se carga
# antes que libmath, sus 42 referencias a funciones de doble precision --fabs y
#IPSIBLE-- no tienen donde resolverse y la carga falla con "necesita \"fabs\" y no
# esta en ninguna imagen cargada". Con la dependencia declarada, el cargador trae la
# cadena de la PROPIA libreria antes de reubicarla, que es lo unico que garantiza el
# orden.
#
# Antes esto no pasaba porque el reproductor del port llevaba math.c estatico dentro
# y no necesitaba la libreria. El fallo aparece al mover el reproductor al arbol, y
# por eso es de este lado: una libreria que usa otra tiene que decirlo.
SAVANXP_MATH_LIBRARY="${SAVANXP_MATH_LIBRARY:-$OUTPUT_ROOT/diskfs/lib}"
if [[ ! -f "$SAVANXP_MATH_LIBRARY/libmath.so.0.4" ]]; then
    echo "ffmpeg: falta $SAVANXP_MATH_LIBRARY/libmath.so.0.4; ejecuta ./build.sh build primero" >&2
    exit 1
fi

echo "== linkeando libffmpeg.so.0.4"
# -Wl,-shared y NO -shared. El driver de clang con el triple none ignora -shared
# (avisa "argument unused during compilation") y linkea un ET_EXEC etiquetado como
# libreria. El guardia de read_header en ldso.c lo rechaza por tipo, que es lo
# correcto, pero es mejor no construir el archivo en esa forma.
$LINK_CC -target x86_64-unknown-none-elf -nostdlib -Wl,-shared -fuse-ld=lld \
    -Wl,--whole-archive "${ARCHIVES[@]}" -Wl,--no-whole-archive \
    -Wl,--unresolved-symbols=ignore-all \
    -Wl,-soname,libffmpeg.so.0.4 \
    -L"$SAVANXP_MATH_LIBRARY" -Wl,-Bdynamic -Wl,--no-as-needed -l:libmath.so.0.4 -Wl,-Bstatic \
    -Wl,-z,max-page-size=0x1000 -Wl,--build-id=none \
    -o libffmpeg.so.0.4

echo "== libffmpeg.so.0.4: $(du -h libffmpeg.so.0.4 | cut -f1)"
$SIZE_CMD libffmpeg.so.0.4 | tail -1
cp libffmpeg.so.0.4 "$OUTPUT_ROOT/external/libffmpeg.so.0.4"
echo "listo: $OUTPUT_ROOT/external/libffmpeg.so.0.4"
