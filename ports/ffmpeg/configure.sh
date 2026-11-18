#!/bin/bash
# Run FFmpeg's upstream configure with the SavanXP target profile.
set -euo pipefail
. "$(dirname "${BASH_SOURCE[0]}")/env.sh"

DEMUXERS="mov,matroska,avi,ogg,mp3,flac,wav,aac,ac3,mpegps,mpegts,mpegvideo,ivf,h264,hevc,mjpeg"
# FFmpeg 7.1.1's H.264 decoder has a final-link dependency on the HEVC decoder.
# libdav1d is AV1, through dav1d (dav1d.sh); FFmpeg's own "av1" decoder only
# drives hardware decoders and is left out.
VIDEO_DECODERS="h264,hevc,vp8,vp9,libdav1d,mpeg4,msmpeg4v3,theora,mpeg1video,mpeg2video,mjpeg"
AUDIO_DECODERS="aac,mp3float,mp2float,vorbis,opus,flac,ac3,pcm_s16le,pcm_s16be,pcm_s24le,pcm_f32le,pcm_u8"
PARSERS="h264,hevc,vp8,vp9,av1,mpeg4video,mpegvideo,mjpeg,aac,ac3,mpegaudio,vorbis,opus,flac"

CONFIGURE_ARGS=(
    --prefix="$WORK/install"
    --enable-cross-compile
    --arch=x86_64
    --target-os=none
    --cc="$SX_CC"
    --ld="$SX_LD"
    --host-cc="$SX_CC"
    --ar="$AR_CMD"
    --ranlib="$RANLIB_CMD"
    --nm="$NM_CMD -g"
    --strip="$STRIP_CMD"
    --extra-cflags="$SX_TARGET_CFLAGS"
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
    --enable-libdav1d
    --pkg-config="${SAVANXP_PKG_CONFIG:-pkg-config}"
    --pkg-config-flags=--static
)

# dav1d.sh installed dav1d.pc here; nothing from the build machine is looked at.
export PKG_CONFIG_LIBDIR="$DAV1D_PREFIX/lib/pkgconfig"
export PKG_CONFIG_PATH=
[ -f "$PKG_CONFIG_LIBDIR/dav1d.pc" ] || {
    echo "error: missing $PKG_CONFIG_LIBDIR/dav1d.pc; run dav1d.sh first" >&2
    exit 1
}

mkdir -p "$BUILD"
cd "$BUILD"

runtime_fingerprint=$(cat "$RUNTIME/.savanxp-runtime-fingerprint" 2>/dev/null || echo missing)
compiler_fingerprint=$($CLANG --version | head -n 1)
WANTED="$(printf '%s\n' "$FFMPEG_COMMIT" "$FFMPEG_SHA256" "$DAV1D_SHA256" "$compiler_fingerprint" "$runtime_fingerprint" "${CONFIGURE_ARGS[@]}")"
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
