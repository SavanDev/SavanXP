#!/bin/bash
# Run FFmpeg's upstream configure with the SavanXP target profile.
set -euo pipefail
. "$(dirname "${BASH_SOURCE[0]}")/env.sh"

DEMUXERS="mov,matroska,avi,ogg,mp3,flac,wav,aac,ac3,mpegps,mpegts,mpegvideo,ivf,h264,hevc,mjpeg"
# FFmpeg 7.1.1's H.264 decoder has a final-link dependency on the HEVC decoder.
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
    --host-cc="$SX_CC -ffreestanding -isystem $SYSROOT/include"
    --ar="$AR_CMD"
    --ranlib="$RANLIB_CMD"
    --nm="$NM_CMD -g"
    --strip="$STRIP_CMD"
    --extra-cflags="$SX_TARGET_CFLAGS"
    --extra-ldflags="$SX_TARGET_LDFLAGS $RUNTIME/crt0.o"
    --extra-libs="$RUNTIME/libsavanxp.a"
    --disable-everything
    --disable-autodetect
    --disable-programs
    --disable-doc
    --disable-network
    --disable-pthreads
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

runtime_fingerprint=$(cat "$RUNTIME/.savanxp-runtime-fingerprint" 2>/dev/null || echo missing)
compiler_fingerprint=$($CLANG --version | head -n 1)
WANTED="$(printf '%s\n' "$FFMPEG_COMMIT" "$FFMPEG_SHA256" "$compiler_fingerprint" "$runtime_fingerprint" "${CONFIGURE_ARGS[@]}")"
STAMP="$BUILD/.savanxp-configure"
if [ "${FORCE_CONFIGURE:-0}" != 1 ] && [ -f "$STAMP" ] && [ -f ffbuild/config.mak ] &&
    [ "$(cat "$STAMP")" = "$WANTED" ]; then
    echo "== configure ya corrido con estos argumentos (FORCE_CONFIGURE=1 para repetirlo)"
    exit 0
fi
rm -f "$STAMP"

export TMPDIR="$WORK/tmp"
mkdir -p "$TMPDIR"

set +e
"$SRC/configure" "${CONFIGURE_ARGS[@]}"
status=$?
set -e

echo
echo "===== configure salio con $status ====="
if [ "$status" -ne 0 ]; then
    echo "----- ultimas lineas de config.log -----"
    tail -40 "$BUILD/ffbuild/config.log" 2>/dev/null || echo "(sin config.log)"
    exit "$status"
fi

for macro in GPL VERSION3 NONFREE; do
    value=$(sed -n "s/^#define CONFIG_${macro} //p" config.h | head -n 1)
    if [ "$value" != "0" ]; then
        echo "error: CONFIG_${macro}=${value:-missing}; el build requiere LGPL 2.1+" >&2
        exit 1
    fi
done

printf '%s' "$WANTED" > "$STAMP"
echo "== configure complete; LGPL/GPL/nonfree guard passed"
