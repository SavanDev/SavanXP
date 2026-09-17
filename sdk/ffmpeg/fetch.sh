#!/bin/bash
# Baja, verifica y desempaqueta el fuente de FFmpeg en el directorio de trabajo.
#
# El fuente no entra al repo ni cruza a el: son decenas de miles de archivos.
# Conviene que WORK caiga en un filesystem nativo -- en WSL, ext4 y no /mnt/c --,
# porque el build hace decenas de miles de operaciones de archivo.
set -euo pipefail
. "$(dirname "${BASH_SOURCE[0]}")/env.sh"

TARBALL="ffmpeg-${FFMPEG_VERSION}.tar.xz"
URL="https://ffmpeg.org/releases/${TARBALL}"

mkdir -p "$WORK"
cd "$WORK"

if [ ! -f "$TARBALL" ]; then
    echo "== bajando $URL"
    curl -fsSL -o "$TARBALL.part" "$URL"
    mv "$TARBALL.part" "$TARBALL"
fi

actual="$(sha256sum "$TARBALL" | cut -d' ' -f1)"
if [ "$FFMPEG_VERSION" = "7.1.1" ] && [ "$actual" != "$FFMPEG_SHA256" ]; then
    echo "error: sha256 de $TARBALL no coincide" >&2
    echo "       esperado $FFMPEG_SHA256" >&2
    echo "       obtenido $actual" >&2
    exit 1
fi
echo "== tarball: $(du -h "$TARBALL" | cut -f1)  sha256: $actual"

if [ ! -f "$SRC/configure" ]; then
    echo "== desempaquetando"
    tar xf "$TARBALL"
fi
echo "== fuente en $SRC ($(cat "$SRC/RELEASE" 2>/dev/null || echo version desconocida))"
