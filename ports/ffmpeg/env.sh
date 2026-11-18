#!/bin/bash
# Shared environment for the official FFmpeg port.
# This file is sourced; it is not executed directly.

set -euo pipefail

PORT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="${REPO:-$(cd "$PORT/../.." && pwd)}"
OUTPUT_ROOT="${SAVANXP_OUTPUT_ROOT:-$REPO/build}"
case "$OUTPUT_ROOT" in
    /*) ;;
    *) OUTPUT_ROOT="$REPO/$OUTPUT_ROOT" ;;
esac
UPSTREAM_FILE="$PORT/UPSTREAM"

metadata() {
    sed -n "s/^$1=//p" "$UPSTREAM_FILE" | head -n 1
}

FFMPEG_TAG=$(metadata tag)
FFMPEG_VERSION="${FFMPEG_TAG#n}"
FFMPEG_SHA256=$(metadata archive_sha256)
FFMPEG_ARCHIVE_URL=$(metadata archive_url)
FFMPEG_ARCHIVE_SIZE=$(metadata archive_size)
FFMPEG_COMMIT=$(metadata commit)

# dav1d, the AV1 decoder FFmpeg's libdav1d wrapper calls. BSD-2-Clause, built
# by dav1d.sh and linked into libffmpeg.so.0.4.
DAV1D_VERSION=$(metadata dav1d_version)
DAV1D_SHA256=$(metadata dav1d_archive_sha256)
DAV1D_ARCHIVE_URL=$(metadata dav1d_archive_url)
DAV1D_ARCHIVE_SIZE=$(metadata dav1d_archive_size)
DAV1D_SIGNATURE_URL=$(metadata dav1d_signature_url)
DAV1D_SIGNATURE_KEY_URL=$(metadata dav1d_signature_key_url)

# FFmpeg's configure expands flags without quoting, so the work tree must
# remain free of whitespace.
#
# Dentro de OUTPUT_ROOT, como el de ports/ccleste, y no en el HOME. La ruta del
# HOME era de cuando el port vivia fuera del repo y se copio sin revisarla al
# traerlo; el resultado era que el arbol CMake consumia build/external/... y sus
# entradas estaban a medio camino del HOME, fuera de lo que ./build.sh clean
# sabe recuperar.
WORK="${WORK:-$OUTPUT_ROOT/ports/ffmpeg/work}"
case "$WORK" in
    *[[:space:]]*)
        echo "error: el directorio de trabajo '$WORK' tiene espacios." >&2
        echo "       Defini WORK=<ruta sin espacios> y volve a correr." >&2
        exit 1
        ;;
esac

SRC="$WORK/ffmpeg-${FFMPEG_VERSION}"
BUILD="$WORK/build"
# The SDK sysroot (docs/roadmaps/done/SDK_V2_ROADMAP.md, phase 5); runtime.sh
# brings it up to date. RUNTIME keeps this port's fingerprint of it and the
# link probe.
SYSROOT=$(SAVANXP_OUTPUT_ROOT="$OUTPUT_ROOT" "$REPO/tools/make-sdk.sh" --print-path)
RUNTIME="$WORK/runtime"
OUT="$WORK/out"
DAV1D_SRC="$WORK/dav1d-${DAV1D_VERSION}"
DAV1D_BUILD="$WORK/dav1d-build"
DAV1D_PREFIX="$WORK/dav1d-install"

# FFmpeg compiles through the SDK's compiler driver, sxcc, with its static SSE
# profile: the target's ABI flags and the sysroot's headers come from it, and a
# link gets crt0, the runtime and the linker script, which is what the test
# programs of configure and of dav1d's Meson need. FFmpeg uses floating point
# in the selected decoders, hence SSE.
#
# -fPIC on top, because this port's goal is FFmpeg ending up in
# /lib/libffmpeg.so.0.4: the objects of a .a have to be relocatable to go into a
# shared library. It overrides the profile's -fno-pic and -fno-pie, and PIC
# code still links into the static test programs.
SXCC="$SYSROOT/share/sdk/sxcc"
SX_TARGET_CFLAGS="-fPIC"

CLANG="${SAVANXP_CLANG:-clang}"
LINK_CC="${SAVANXP_LINK_CC:-$CLANG}"
MAKE_CMD="${MAKE:-${SAVANXP_MAKE:-make}}"
AR_CMD="${SAVANXP_AR:-llvm-ar}"
RANLIB_CMD="${SAVANXP_RANLIB:-llvm-ranlib}"
NM_CMD="${SAVANXP_NM:-llvm-nm}"
STRIP_CMD="${SAVANXP_STRIP:-llvm-strip}"
SIZE_CMD="${SAVANXP_SIZE:-llvm-size}"

SX_CC="$SXCC --sx-profile=sse"
SX_LD="$SX_CC"

sx_jobs() {
    if command -v nproc >/dev/null 2>&1; then
        nproc
    else
        echo 4
    fi
}
