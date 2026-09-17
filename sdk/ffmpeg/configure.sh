#!/bin/bash
# Corre el configure de FFmpeg apuntando al target de SavanXP.
#
# El set de formatos es el del reproductor (docs/MEDIA_PLAYER.md). Se agranda
# aca, en las listas de abajo; el resto lo resuelve configure con las
# dependencias de cada componente (los parsers y bitstream filters que un
# decoder necesita entran solos).
#
# Sin asm (--disable-asm: el toolchain no trae nasm) y sin hilos (el kernel no
# tiene primitiva de hilos), asi que todo decodifica en C escalar sobre un core.
set -euo pipefail
. "$(dirname "${BASH_SOURCE[0]}")/env.sh"

DEMUXERS="mov,matroska,avi,ogg,mp3,flac,wav,aac,ac3,mpegps,mpegts,mpegvideo,ivf,h264,hevc,mjpeg"

# hevc no es solo por HEVC: en FFmpeg 7.1.1 el decoder h264 NO linkea sin el.
# h2645_sei.o referencia ff_aom_uninit_film_grain_params, que vive en
# aom_film_grain.o, y ese objeto solo se compila si esta el decoder hevc. El
# build pasa y el error aparece recien al linkear el programa.
VIDEO_DECODERS="h264,hevc,vp8,vp9,mpeg4,msmpeg4v3,theora,mpeg1video,mpeg2video,mjpeg"
AUDIO_DECODERS="aac,mp3float,mp2float,vorbis,opus,flac,ac3,pcm_s16le,pcm_s16be,pcm_s24le,pcm_f32le,pcm_u8"

PARSERS="h264,hevc,vp8,vp9,mpeg4video,mpegvideo,mjpeg,aac,ac3,mpegaudio,vorbis,opus,flac"

CONFIGURE_ARGS=(
    --prefix="$WORK/install"
    --enable-cross-compile
    --arch=x86_64
    --target-os=none
    --cc="$SX_CC"
    --ld="$SX_LD"
    # configure exige un compilador host C11 aunque en esta configuracion no
    # construye ninguna herramienta para el host. Sirve el mismo clang.
    --host-cc="$SX_CC -ffreestanding -isystem $SYSROOT/include"
    --ar=llvm-ar
    --ranlib=llvm-ranlib
    --nm="llvm-nm -g"
    --strip=llvm-strip
    --extra-cflags="$SX_TARGET_CFLAGS"
    --extra-ldflags="$SX_TARGET_LDFLAGS $RUNTIME/crt0.o"
    --extra-libs="$RUNTIME/libsavanxp.a"
    --disable-everything
    --disable-autodetect
    --disable-programs
    --disable-doc
    --disable-network
    --disable-pthreads
    --disable-w32threads
    --disable-os2threads
    --disable-asm
    --disable-debug
    --disable-shared
    --enable-static
    --disable-avdevice
    --disable-avfilter
    --enable-protocol=file
    --enable-demuxer="$DEMUXERS"
    --enable-decoder="$VIDEO_DECODERS,$AUDIO_DECODERS"
    --enable-parser="$PARSERS"
    --enable-swscale
    --enable-swresample
)

mkdir -p "$BUILD"
cd "$BUILD"

# configure tarda minutos (bajo Git Bash, varios): no se repite si los
# argumentos no cambiaron. FORCE_CONFIGURE=1 lo fuerza igual.
STAMP="$BUILD/.savanxp-configure"
WANTED="$(printf '%s\n' "$FFMPEG_VERSION" "${CONFIGURE_ARGS[@]}")"
if [ "${FORCE_CONFIGURE:-0}" != 1 ] && [ -f "$STAMP" ] && [ -f ffbuild/config.mak ] &&
    [ "$(cat "$STAMP")" = "$WANTED" ]; then
    echo "== configure ya corrido con estos argumentos (FORCE_CONFIGURE=1 para repetirlo)"
    exit 0
fi
rm -f "$STAMP"

# En Git Bash TMPDIR llega como ruta de Windows (C:\...), que configure no puede
# usar para crear y ejecutar sus scripts de prueba.
export TMPDIR="$WORK/tmp"
mkdir -p "$TMPDIR"

set +e
"$SRC/configure" "${CONFIGURE_ARGS[@]}"
status=$?
set -e

echo
echo "===== configure salio con $status ====="
if [ $status -ne 0 ]; then
    echo "----- ultimas lineas de config.log -----"
    tail -40 "$BUILD/ffbuild/config.log" 2>/dev/null || echo "(sin config.log)"
    exit $status
fi

# configure escribe rutas absolutas con la forma de su shell. Bajo Git Bash eso
# es /c/..., que el GNU make nativo de Windows no puede abrir: la primera linea
# del Makefile generado es un include de una ruta asi. Se reescriben a C:/...,
# que entienden tanto make como clang y el sh que corre las recetas.
if [ "$SX_WINDOWS" = 1 ]; then
    native_work="$(sx_native_path "$WORK")"
    sed -i "s|$WORK|$native_work|g" Makefile ffbuild/config.mak
fi

printf '%s' "$WANTED" > "$STAMP"
