#!/bin/bash
# Link libffmpeg.so.0.4 out of the FFmpeg archives. This port builds NO program.
set -euo pipefail
. "$(dirname "${BASH_SOURCE[0]}")/env.sh"

mkdir -p "$OUT"

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

# The player is NOT built here. It lives in subsystems/posix/userland/mediaplayer/
# and the tree links it, which is where the window, the controls and the clock
# are; this port only contributes the engine. This script used to compile it and
# install it as mediaplayer-ffmpeg in what was then /disk/bin, with a 114-line
# launcher in the tree that exec'd this binary. Both went away: the engine is the
# library, and the program belongs to the tree.
# libffmpeg DECLARES that it needs libmath, and it is not cosmetic.
#
# The loader walks the executable's DT_NEEDED in order and, as soon as it brings
# in a library, relocates it before bringing the next. So if libffmpeg is loaded
# before libmath, its 42 references to double-precision functions -- fabs and
# the like -- have nowhere to resolve and the load fails with a "needs \"fabs\"
# and it is in no loaded image" message. With the dependency declared, the
# loader brings the library's OWN chain before relocating it, which is the only
# thing that guarantees the order.
#
# This did not happen before because the port's player carried math.c statically
# and did not need the library. The failure shows up when the player moves into
# the tree, and that is why it is fixed on this side: a library that uses another
# has to say so.
SAVANXP_MATH_LIBRARY="${SAVANXP_MATH_LIBRARY:-$OUTPUT_ROOT/diskfs/lib}"
if [[ ! -f "$SAVANXP_MATH_LIBRARY/libmath.so.0.4" ]]; then
    echo "ffmpeg: falta $SAVANXP_MATH_LIBRARY/libmath.so.0.4; ejecuta ./build.sh build primero" >&2
    exit 1
fi

# Todo se escribe con ruta ABSOLUTA. Antes este script hacia `cd "$OUT"` y vivia
# dentro del directorio de trabajo; se fue al quitarse el reproductor, y el -o de
# abajo se quedo resolviendo contra el directorio desde el que se llama. Lanzado
# desde la raiz del repo --que es como lo llama build.sh-- eso escribia un
# libffmpeg.so.0.4 de 7 MB en la raiz, y un `git add -a` de raiz lo versiono.
#
# Un artefacto de 7 MB en el arbol no es un descuido de forma: es 7 MB en cada
# clon y un binario que no se revisa en un diff.
cd "$OUT"
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
cp "$OUT/libffmpeg.so.0.4" "$OUTPUT_ROOT/external/libffmpeg.so.0.4"
echo "listo: $OUTPUT_ROOT/external/libffmpeg.so.0.4"
