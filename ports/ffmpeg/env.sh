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
SDK="$REPO/subsystems/posix/sdk/v1"
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

# FFmpeg's configure expands flags without quoting, so the work tree must
# remain free of whitespace.
WORK="${WORK:-$HOME/savanxp-ffmpeg}"
case "$WORK" in
    *[[:space:]]*)
        echo "error: el directorio de trabajo '$WORK' tiene espacios." >&2
        echo "       Defini WORK=<ruta sin espacios> y volve a correr." >&2
        exit 1
        ;;
esac

SRC="$WORK/ffmpeg-${FFMPEG_VERSION}"
BUILD="$WORK/build"
RUNTIME="$WORK/runtime"
SYSROOT="$WORK/sysroot"
OUT="$WORK/out"

# FFmpeg uses floating point in the selected decoders, so this port uses the
# SSE/SSE2 ABI and links the SDK math runtime.
SX_TARGET_CFLAGS="-ffreestanding -fstack-protector-strong -fno-pic -fno-pie -mno-red-zone"
SX_TARGET_CFLAGS="$SX_TARGET_CFLAGS -mcmodel=small -mno-mmx -msse -msse2"
SX_TARGET_CFLAGS="$SX_TARGET_CFLAGS -isystem $SYSROOT/include"

CLANG="${SAVANXP_CLANG:-clang}"
LINK_CC="${SAVANXP_LINK_CC:-$CLANG}"
MAKE_CMD="${MAKE:-${SAVANXP_MAKE:-make}}"
AR_CMD="${SAVANXP_AR:-llvm-ar}"
RANLIB_CMD="${SAVANXP_RANLIB:-llvm-ranlib}"
NM_CMD="${SAVANXP_NM:-llvm-nm}"
STRIP_CMD="${SAVANXP_STRIP:-llvm-strip}"
SIZE_CMD="${SAVANXP_SIZE:-llvm-size}"

# The target compiler is freestanding. The link driver uses the Linux triple
# so clang delegates to ld.lld.
SX_CC="$CLANG -target x86_64-unknown-none-elf"
SX_LD="$LINK_CC -target x86_64-unknown-linux-gnu"

# -no-pie is required with -fno-pic; -static keeps the SDK linker script's
# PT_LOAD layout free of an ELF interpreter.
SX_TARGET_LDFLAGS="-nostdlib -static -no-pie -fuse-ld=lld -Wl,-T,$SYSROOT/linker.ld"
SX_TARGET_LDFLAGS="$SX_TARGET_LDFLAGS -Wl,-z,max-page-size=0x1000 -Wl,--build-id=none"

sx_jobs() {
    if command -v nproc >/dev/null 2>&1; then
        nproc
    else
        echo 4
    fi
}
