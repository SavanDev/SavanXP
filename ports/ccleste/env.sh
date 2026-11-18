#!/bin/bash
# Shared environment for the official ccleste port.
# This file is sourced; it is not executed directly.

set -euo pipefail

PORT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="${REPO:-$(cd "$PORT/../.." && pwd)}"
OUTPUT_ROOT="${SAVANXP_OUTPUT_ROOT:-$REPO/build}"
case "$OUTPUT_ROOT" in
    /*) ;;
    *) OUTPUT_ROOT="$REPO/$OUTPUT_ROOT" ;;
esac
# shellcheck source=../../tools/lib/sdk_flags.sh
. "$REPO/tools/lib/sdk_flags.sh"
UPSTREAM_FILE="$PORT/UPSTREAM"

metadata() {
    sed -n "s/^$1=//p" "$UPSTREAM_FILE" | head -n 1
}

CCLESTE_COMMIT=$(metadata commit)
CCLESTE_TREE=$(metadata tree)
CCLESTE_SHA256=$(metadata archive_sha256)
CCLESTE_ARCHIVE_URL=$(metadata archive_url)
CCLESTE_ARCHIVE_SIZE=$(metadata archive_size)
CCLESTE_LICENSE=$(metadata license)

TARBALL="ccleste-${CCLESTE_COMMIT}.tar.gz"
WORK="${WORK:-$OUTPUT_ROOT/ports/ccleste/work}"
SRC="$WORK/ccleste-${CCLESTE_COMMIT}"
OBJ="$WORK/obj"
STAGE="$WORK/install-stage"
RESOURCE_DIR="$WORK/sxe"
FRESH_ELF="$WORK/ccleste.elf"
OUTPUT="$OUTPUT_ROOT/external/ccleste.elf"
STAMPED="$OUTPUT_ROOT/ccleste/ccleste"

# The 23 sound effects decode to 772 KiB of unsigned 8-bit mono PCM, and the
# five music loops decode to about 4 MB more, so the port asks for more heap
# up front. The heap lives in .bss: it costs address space, not image bytes.
# build.sh links it as a strong bootstrap arena over the prebuilt runtime's
# weak one (savanxp_sdk_heap_source), as build-user.sh --heap-mib does.
SX_HEAP_SIZE="$((8 * 1024 * 1024))"

# Celeste Classic uses floating point (sin/cos/fmodf/floorf) in its update step,
# so this port uses the SSE/SSE2 ABI and links the SDK math runtime, exactly
# like the FFmpeg port. The non-SSE path only exposes four double x87 helpers.
#
# The music comes from libffmpeg.so.0.4, so this port links PIE like mediaplayer
# (same profile: PIC objects, -pie, DT_NEEDED, PT_INTERP): -fno-pic objects
# cannot go into a shared-library dependency graph, and -static leaves no
# DT_NEEDED for the loader to map.
SX_TARGET_CFLAGS=(
    -ffreestanding
    -fstack-protector-strong
    -fPIC
    -mno-red-zone
    -mcmodel=medium
    -mno-mmx
    -msse
    -msse2
    -Wno-language-extension-token
    -Wno-c23-extensions
)

CLANG="${SAVANXP_CLANG:-clang}"
LINK_CC="${SAVANXP_LINK_CC:-$CLANG}"
OBJCOPY="${SAVANXP_OBJCOPY:-llvm-objcopy}"
READELF="${SAVANXP_READELF:-llvm-readelf}"
SIZE_CMD="${SAVANXP_SIZE:-llvm-size}"
PYTHON="${SAVANXP_PYTHON:-python3}"

# The target compiler is freestanding. The link driver and its flags are the
# SDK's PIE profile (tools/lib/sdk_flags.sh), the one mediaplayer uses: clang
# with the Linux triple delegates to ld.lld, -pie with an explicit entry,
# --export-dynamic so the loader has symbols to resolve the libraries against,
# and --dynamic-linker with the loader's path. The loader itself is linked
# inside the program from the SDK sysroot, and crt0 runs it.
SX_CC="$CLANG -target x86_64-unknown-none-elf"
SX_LD="$LINK_CC"
mapfile -t SX_TARGET_LDFLAGS < <(savanxp_sdk_pie_link_flags 0)

# libffmpeg.so.0.4 comes from the FFmpeg port (same artifacts mediaplayer uses:
# the shared object plus the public headers and the generated config.h). The
# work tree is found by glob so a version bump does not edit this file; with
# --skip-port nothing compiles and none of this is needed.
FFMPEG_LIB="$OUTPUT_ROOT/external/libffmpeg.so.0.4"
FFMPEG_BUILD="$OUTPUT_ROOT/ports/ffmpeg/work/build"
FFMPEG_SRC=""

# The game data is not vendored: the port installs the upstream data/ tree under
# /games/celeste and .gitignore keeps it out of the repository.
DATA_DIR="${SAVANXP_CCLESTE_DATA:-$SRC/data}"
DATA_TARGET="/games/celeste"

# The upstream frontend is SDL, which SavanXP does not have. The overlay
# replaces sdl12main.c entirely, so the engine units are the whole upstream
# build: no patches, and nothing from the SDL host backend is compiled.
ENGINE_SOURCES=(celeste.c)
