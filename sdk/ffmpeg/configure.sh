#!/bin/bash
# Corre el configure de FFmpeg apuntando al target de SavanXP.
#
# Primer objetivo, a proposito minimo: WAV con PCM s16le. Alcanza para probar
# que libavutil/libavcodec/libavformat compilan, linkean y corren; los codecs
# de verdad se agregan despues, cuando eso este en pie.
set -euo pipefail

FFMPEG_VERSION="${FFMPEG_VERSION:-7.1.1}"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="${REPO:-$(cd "$HERE/../.." && pwd)}"
SDK="$REPO/subsystems/posix/sdk/v1"
WORK="${WORK:-$HOME/savanxp-ffmpeg}"
RUNTIME="$WORK/runtime"
SRC="$WORK/ffmpeg-${FFMPEG_VERSION}"
BUILD="$WORK/build"

CFLAGS="-ffreestanding -fno-stack-protector -fno-pic -fno-pie -mno-red-zone"
CFLAGS="$CFLAGS -mcmodel=small -mno-mmx -msse -msse2"
CFLAGS="$CFLAGS -I$SDK/include -I$REPO/include"

# -no-pie es imprescindible: el clang de Arch le pasa -pie al linker por
# default, y con -fno-pic las reubicaciones absolutas hacen que lld rechace
# CUALQUIER programa. Sin esto todos los tests de link de configure fallan y
# FFmpeg concluye que la libc no tiene nada -- no da error, da una config
# equivocada, que es peor.
LDFLAGS="-nostdlib -static -no-pie -fuse-ld=lld -Wl,-T,$SDK/linker.ld"
LDFLAGS="$LDFLAGS -Wl,-z,max-page-size=0x1000 -Wl,--build-id=none $RUNTIME/crt0.o"

mkdir -p "$BUILD"
cd "$BUILD"

set +e
"$SRC/configure" \
    --prefix="$WORK/install" \
    --enable-cross-compile \
    --arch=x86_64 \
    --target-os=none \
    --cc=clang \
    --ar=llvm-ar \
    --ranlib=llvm-ranlib \
    --nm="llvm-nm -g" \
    --strip=llvm-strip \
    --extra-cflags="$CFLAGS" \
    --extra-ldflags="$LDFLAGS" \
    --extra-libs="$RUNTIME/libsavanxp.a" \
    --disable-everything \
    --disable-autodetect \
    --disable-programs \
    --disable-doc \
    --disable-network \
    --disable-pthreads \
    --disable-w32threads \
    --disable-os2threads \
    --disable-asm \
    --disable-debug \
    --disable-shared \
    --enable-static \
    --enable-protocol=file \
    --enable-demuxer=wav \
    --enable-muxer=wav \
    --enable-decoder=pcm_s16le \
    --enable-encoder=pcm_s16le
status=$?
set -e

echo
echo "===== configure salio con $status ====="
if [ $status -ne 0 ]; then
    echo "----- ultimas lineas de config.log -----"
    tail -40 "$BUILD/ffbuild/config.log" 2>/dev/null || tail -40 "$BUILD/config.log" 2>/dev/null || echo "(sin config.log)"
fi
exit $status
