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
SDK="$REPO/subsystems/posix/sdk/v1"
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
    "-DSX_HEAP_SIZE=$SX_HEAP_SIZE"
)

CLANG="${SAVANXP_CLANG:-clang}"
LINK_CC="${SAVANXP_LINK_CC:-$CLANG}"
OBJCOPY="${SAVANXP_OBJCOPY:-llvm-objcopy}"
READELF="${SAVANXP_READELF:-llvm-readelf}"
SIZE_CMD="${SAVANXP_SIZE:-llvm-size}"
PYTHON="${SAVANXP_PYTHON:-python3}"

# The target compiler is freestanding. The link driver uses the Linux triple so
# clang delegates to ld.lld. The PIE profile mirrors mediaplayer's: no -static,
# no fixed linker script, -pie with an explicit entry, --export-dynamic so the
# loader has symbols to resolve the libraries against, and --dynamic-linker
# with the path the interpreter would have as a file of its own (PT_INTERP is
# recorded but not executed yet: the kernel maps the image and crt0 runs the
# loader linked inside the program).
SX_CC="$CLANG -target x86_64-unknown-none-elf"
SX_LD="$LINK_CC -target x86_64-unknown-linux-gnu"
SX_TARGET_LDFLAGS=(
    -nostdlib -fuse-ld=lld -pie
    -Wl,-z,max-page-size=0x1000 -Wl,--dynamic-linker,/disk/lib/ld.so.0.4
    -Wl,--build-id=none -Wl,--export-dynamic -Wl,--gc-sections -Wl,-e,_start
)

# libffmpeg.so.0.4 comes from the FFmpeg port (same artifacts mediaplayer uses:
# the shared object plus the public headers and the generated config.h). The
# work tree is found by glob so a version bump does not edit this file; with
# --skip-port nothing compiles and none of this is needed.
FFMPEG_LIB="$OUTPUT_ROOT/external/libffmpeg.so.0.4"
FFMPEG_BUILD="$OUTPUT_ROOT/ports/ffmpeg/work/build"
FFMPEG_SRC=""

# The game data is not vendored: the port installs the upstream data/ tree under
# /disk/games/celeste and .gitignore keeps it out of the repository.
DATA_DIR="${SAVANXP_CCLESTE_DATA:-$SRC/data}"
DATA_TARGET="/disk/games/celeste"

# The upstream frontend is SDL, which SavanXP does not have. The overlay
# replaces sdl12main.c entirely, so the engine units are the whole upstream
# build: no patches, and nothing from the SDL host backend is compiled.
ENGINE_SOURCES=(celeste.c)
