#!/bin/bash
# Compila y linkea wavinfo contra las libav* recien construidas, produciendo un
# ELF de SavanXP listo para instalar en la imagen.
set -euo pipefail

FFMPEG_VERSION="${FFMPEG_VERSION:-7.1.1}"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="${REPO:-$(cd "$HERE/../.." && pwd)}"
SDK="$REPO/subsystems/posix/sdk/v1"
WORK="${WORK:-$HOME/savanxp-ffmpeg}"
RUNTIME="$WORK/runtime"
SRC="$WORK/ffmpeg-${FFMPEG_VERSION}"
BUILD="$WORK/build"
PORT="$REPO/sdk/ffmpeg"
OUT="$WORK/out"

CFLAGS=(
    -target x86_64-unknown-none-elf
    -ffreestanding -fno-stack-protector -fno-pic -fno-pie
    -mno-red-zone -mcmodel=small -mno-mmx -msse -msse2
    -I "$SDK/include" -I "$REPO/include"
    -I "$SRC" -I "$BUILD"
)

mkdir -p "$OUT"
cd "$OUT"

# El orden importa: las libav* se referencian entre si y el runtime va ultimo.
# -static: sin esto el driver marca el ELF como dinamico y agrega .interp, que
# ademas empuja al segundo PT_LOAD a una direccion sin alinear a pagina.
link_program() {
    local name="$1"
    shift
    echo "== compilando $name.c"
    clang -c -x c "$PORT/$name.c" -o "$name.o" "${CFLAGS[@]}"

    echo "== linkeando $name"
    clang -nostdlib -static -no-pie -fuse-ld=lld -target x86_64-unknown-none-elf \
        -Wl,-T,"$SDK/linker.ld" -Wl,-z,max-page-size=0x1000 -Wl,--build-id=none \
        -o "$name.elf" \
        "$RUNTIME/crt0.o" "$name.o" \
        "$@" \
        "$BUILD/libavformat/libavformat.a" \
        "$BUILD/libavcodec/libavcodec.a" \
        "$BUILD/libswresample/libswresample.a" \
        "$BUILD/libavutil/libavutil.a" \
        "$RUNTIME/libsavanxp.a"

    echo "== $name.elf: $(du -h "$name.elf" | cut -f1)"
    llvm-size "$name.elf" | tail -1
    cp "$name.elf" "$REPO/build/external/$name.elf"
}

mkdir -p "$REPO/build/external"
link_program wavinfo
link_program player "$BUILD/libswscale/libswscale.a"
echo "listo: los ELF quedaron en $REPO/build/external"
