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

# ---- dav1d -----------------------------------------------------------------
DAV1D_TARBALL="dav1d-${DAV1D_VERSION}.tar.xz"
if [ ! -f "$DAV1D_TARBALL" ]; then
    echo "== downloading $DAV1D_ARCHIVE_URL"
    curl -fsSL -o "$DAV1D_TARBALL.part" "$DAV1D_ARCHIVE_URL"
    mv "$DAV1D_TARBALL.part" "$DAV1D_TARBALL"
fi
actual_size="$(wc -c < "$DAV1D_TARBALL" | tr -d ' ')"
actual_sha="$(sha256sum "$DAV1D_TARBALL" | cut -d' ' -f1)"
if [ "$actual_size" != "$DAV1D_ARCHIVE_SIZE" ] || [ "$actual_sha" != "$DAV1D_SHA256" ]; then
    echo "error: $DAV1D_TARBALL does not match UPSTREAM" >&2
    echo "       expected $DAV1D_ARCHIVE_SIZE bytes, sha256 $DAV1D_SHA256" >&2
    echo "       got      $actual_size bytes, sha256 $actual_sha" >&2
    exit 1
fi
if [ "${SAVANXP_VERIFY_SIGNATURE:-0}" = 1 ]; then
    GNUPGHOME="$WORK/gnupg-dav1d"
    export GNUPGHOME
    mkdir -p "$GNUPGHOME"
    chmod 700 "$GNUPGHOME"
    [ -f "$DAV1D_TARBALL.asc" ] || curl -fsSL -o "$DAV1D_TARBALL.asc" "$DAV1D_SIGNATURE_URL"
    [ -f "$WORK/videolan-release.asc" ] || curl -fsSL -o "$WORK/videolan-release.asc" "$DAV1D_SIGNATURE_KEY_URL"
    gpg --batch --import "$WORK/videolan-release.asc" >/dev/null 2>&1
    gpg --batch --verify "$DAV1D_TARBALL.asc" "$DAV1D_TARBALL"
    unset GNUPGHOME
fi
echo "== dav1d tarball: $(du -h "$DAV1D_TARBALL" | cut -f1)  sha256: $actual_sha"
if [ ! -f "$DAV1D_SRC/meson.build" ]; then
    tar xf "$DAV1D_TARBALL"
fi
echo "== dav1d source: $DAV1D_SRC (release $DAV1D_VERSION)"
