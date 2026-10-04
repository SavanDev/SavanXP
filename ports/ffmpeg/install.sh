#!/bin/bash
# Install libffmpeg.so.0.4 through SxFS. This port builds no program.
set -euo pipefail
. "$(dirname "${BASH_SOURCE[0]}")/env.sh"

export SAVANXP_OUTPUT_ROOT="$OUTPUT_ROOT"
NO_INSTALL=0
WITH_TEST_MEDIA=0
NO_COMPACT=0
while (($#)); do
    case "$1" in
        --no-install) NO_INSTALL=1; shift ;;
        --with-test-media) WITH_TEST_MEDIA=1; shift ;;
        --no-compact) NO_COMPACT=1; shift ;;
        *) echo "ffmpeg install: unknown option '$1'" >&2; exit 2 ;;
    esac
done

# The raw link artifact keeps its historical name; only the installed identity
# is distinct from the system launcher.
PYTHON="${SAVANXP_PYTHON:-python3}"
command -v "$PYTHON" >/dev/null 2>&1 || {
    echo "ffmpeg: Python no encontrado: $PYTHON" >&2
    exit 1
}

# La libreria va a /disk/lib. Esto es TODO lo que instala el port: ni un programa, ni
# un icono, ni una entrada del lanzador. El reproductor es del arbol
# (subsystems/posix/userland/mediaplayer/) y se enlaza alli contra esta libreria.
#
# Lo que antes instalaba este script --un mediaplayer-ffmpeg estampado en /disk/bin-- se
# fue con el launcher: el programa es del arbol y el motor es la libreria.
LIBRARY="$OUTPUT_ROOT/external/libffmpeg.so.0.4"
if [[ ! -f "$LIBRARY" ]]; then
    echo "ffmpeg: falta $LIBRARY; ejecuta ports/ffmpeg/link.sh" >&2
    exit 1
fi

STAGE="$OUTPUT_ROOT/ports/ffmpeg/install-stage"
rm -rf "$STAGE"
mkdir -p "$STAGE/lib"
cp "$LIBRARY" "$STAGE/lib/libffmpeg.so.0.4"
echo "ffmpeg: instalado /disk/lib/libffmpeg.so.0.4"

if ((WITH_TEST_MEDIA)); then
    mkdir -p "$OUTPUT_ROOT/media" "$STAGE/media"
    for generator in make-tone.py make-clip.py make-avclip.py; do
        "$PYTHON" "$PORT/tests/$generator"
    done
    for media in tono.wav clip.mjpeg avsync.avi; do
        cp "$OUTPUT_ROOT/media/$media" "$STAGE/media/$media"
    done
fi

printf 'ffmpeg: %s: %s bytes\n' "$LIBRARY" "$(wc -c < "$LIBRARY" | tr -d ' ')"
if ((NO_INSTALL)); then
    exit 0
fi

IMAGE="${SAVANXP_DISK_IMAGE:-$OUTPUT_ROOT/disk.img}"
CLI="${SAVANXP_SXFS_CLI:-$OUTPUT_ROOT/tools/sxfs-cli}"
case "$IMAGE" in
    /*) ;;
    *) IMAGE="$REPO/$IMAGE" ;;
esac
case "$CLI" in
    /*) ;;
    *) CLI="$REPO/$CLI" ;;
esac
[[ -f "$IMAGE" ]] || {
    echo "ffmpeg: falta $IMAGE; ejecuta ./build.sh build primero" >&2
    exit 1
}
[[ -x "$CLI" ]] || {
    echo "ffmpeg: falta $CLI; ejecuta ./build.sh build primero" >&2
    exit 1
}

SYNC_ARGS=(
    --image "$IMAGE"
    --source "$STAGE"
    --cli "$CLI"
    --sectors "${SAVANXP_SXFS_SECTORS:-2097152}"
)
if ((NO_COMPACT)); then
    SYNC_ARGS+=(--no-compact)
fi
"$PYTHON" "$REPO/tools/sxfs_sync.py" "${SYNC_ARGS[@]}"
echo "ffmpeg: sincronizado el volumen"
if ((WITH_TEST_MEDIA)); then
    echo "ffmpeg: instalados /disk/media/{tono.wav,clip.mjpeg,avsync.avi}"
fi
