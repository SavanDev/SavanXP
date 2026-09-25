#!/bin/bash
# Download, verify, and unpack the pinned ccleste archive.
set -euo pipefail
. "$(dirname "${BASH_SOURCE[0]}")/env.sh"

mkdir -p "$WORK"
cd "$WORK"

if [ ! -f "$TARBALL" ]; then
    echo "== bajando $CCLESTE_ARCHIVE_URL"
    curl -fsSL -o "$TARBALL.part" "$CCLESTE_ARCHIVE_URL"
    mv "$TARBALL.part" "$TARBALL"
fi

actual_size="$(wc -c < "$TARBALL" | tr -d ' ')"
if [ "$actual_size" != "$CCLESTE_ARCHIVE_SIZE" ]; then
    echo "error: tamaño de $TARBALL no coincide" >&2
    echo "       esperado $CCLESTE_ARCHIVE_SIZE; obtenido $actual_size" >&2
    exit 1
fi
actual_sha="$(sha256sum "$TARBALL" | cut -d' ' -f1)"
if [ "$actual_sha" != "$CCLESTE_SHA256" ]; then
    echo "error: sha256 de $TARBALL no coincide" >&2
    echo "       esperado $CCLESTE_SHA256" >&2
    echo "       obtenido $actual_sha" >&2
    exit 1
fi
echo "== tarball: $(du -h "$TARBALL" | cut -f1)  sha256: $actual_sha"

if [ ! -d "$SRC" ]; then
    echo "== desempaquetando"
    # The archive's top directory is already named after the commit, which is
    # also $SRC, so it is unpacked into a scratch directory and moved into
    # place instead of being renamed in situ.
    UNPACK="$WORK/unpack"
    rm -rf "$UNPACK"
    mkdir -p "$UNPACK"
    tar xf "$TARBALL" -C "$UNPACK"
    extracted="ccleste-${CCLESTE_COMMIT}"
    [ -d "$UNPACK/$extracted" ] || {
        echo "error: el tarball no produjo el directorio esperado: $extracted" >&2
        exit 1
    }
    mv "$UNPACK/$extracted" "$SRC"
    rm -rf "$UNPACK"
fi

# Guard against a work directory left behind by a different pin: the engine
# units are copied out of it by name, so a stale tree would silently change the
# build instead of failing.
if [ ! -f "$SRC/celeste.c" ] || [ ! -f "$SRC/celeste.h" ] || [ ! -f "$SRC/tilemap.h" ]; then
    echo "error: el source tree no corresponde a $CCLESTE_COMMIT" >&2
    echo "       borra $WORK y volve a correr" >&2
    exit 1
fi

echo "== source: $SRC (commit $CCLESTE_COMMIT, tree $CCLESTE_TREE)"
