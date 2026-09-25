#!/bin/bash
# Download, verify, and unpack the pinned FFmpeg release.
set -euo pipefail
. "$(dirname "${BASH_SOURCE[0]}")/env.sh"

TARBALL="ffmpeg-${FFMPEG_VERSION}.tar.xz"
URL="$FFMPEG_ARCHIVE_URL"

mkdir -p "$WORK"
cd "$WORK"

if [ ! -f "$TARBALL" ]; then
    echo "== bajando $URL"
    curl -fsSL -o "$TARBALL.part" "$URL"
    mv "$TARBALL.part" "$TARBALL"
fi

actual_size="$(wc -c < "$TARBALL" | tr -d ' ')"
if [ "$actual_size" != "$FFMPEG_ARCHIVE_SIZE" ]; then
    echo "error: tamaño de $TARBALL no coincide" >&2
    echo "       esperado $FFMPEG_ARCHIVE_SIZE; obtenido $actual_size" >&2
    exit 1
fi
actual_sha="$(sha256sum "$TARBALL" | cut -d' ' -f1)"
if [ "$actual_sha" != "$FFMPEG_SHA256" ]; then
    echo "error: sha256 de $TARBALL no coincide" >&2
    echo "       esperado $FFMPEG_SHA256" >&2
    echo "       obtenido $actual_sha" >&2
    exit 1
fi

if [ "${SAVANXP_VERIFY_SIGNATURE:-0}" = 1 ]; then
    command -v gpg >/dev/null 2>&1 || {
        echo "error: SAVANXP_VERIFY_SIGNATURE=1 requiere gpg" >&2
        exit 1
    }
    SIGNATURE="$TARBALL.asc"
    KEY="$WORK/ffmpeg-devel.asc"
    [ -f "$SIGNATURE" ] || curl -fsSL -o "$SIGNATURE" "$FFMPEG_ARCHIVE_URL.asc"
    [ -f "$KEY" ] || curl -fsSL -o "$KEY" https://ffmpeg.org/ffmpeg-devel.asc
    gpg --batch --import "$KEY" >/dev/null 2>&1
    gpg --batch --verify "$SIGNATURE" "$TARBALL"
fi
echo "== tarball: $(du -h "$TARBALL" | cut -f1)  sha256: $actual_sha"

if [ ! -f "$SRC/configure" ]; then
    echo "== desempaquetando"
    tar xf "$TARBALL"
fi

if [ -f "$SRC/RELEASE" ] && [ "$(cat "$SRC/RELEASE")" != "$FFMPEG_VERSION" ]; then
    echo "error: el source tree no corresponde a $FFMPEG_VERSION" >&2
    exit 1
fi
echo "== source: $SRC (release $FFMPEG_VERSION, commit $FFMPEG_COMMIT)"
