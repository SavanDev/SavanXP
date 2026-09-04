#!/bin/bash
# Baja y desempaqueta el fuente de FFmpeg en el filesystem de WSL.
#
# En ext4 y no en /mnt/f a proposito: el build de FFmpeg hace decenas de miles
# de operaciones de archivo y sobre drvfs eso tarda un orden de magnitud mas.
# Lo unico que despues cruza a Windows son los .a.
set -euo pipefail

FFMPEG_VERSION="${FFMPEG_VERSION:-7.1.1}"
WORK="${WORK:-$HOME/savanxp-ffmpeg}"
TARBALL="ffmpeg-${FFMPEG_VERSION}.tar.xz"
URL="https://ffmpeg.org/releases/${TARBALL}"

mkdir -p "$WORK"
cd "$WORK"

if [ ! -f "$TARBALL" ]; then
    echo "== bajando $URL"
    curl -fsSL -o "$TARBALL.part" "$URL"
    mv "$TARBALL.part" "$TARBALL"
fi
echo "== tarball: $(du -h "$TARBALL" | cut -f1)  sha256: $(sha256sum "$TARBALL" | cut -d' ' -f1)"

if [ ! -d "ffmpeg-${FFMPEG_VERSION}" ]; then
    echo "== desempaquetando"
    tar xf "$TARBALL"
fi

cd "ffmpeg-${FFMPEG_VERSION}"
echo "== version: $(cat RELEASE 2>/dev/null || echo desconocida)"
echo "== target-os soportados por configure:"
grep -A 3 "^    none)" configure | head -8 || echo "  (no hay caso 'none')"
echo "== listo en $WORK/ffmpeg-${FFMPEG_VERSION}"
