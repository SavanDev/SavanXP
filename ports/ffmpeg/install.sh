#!/bin/bash
# Stamp the optional Media Player backend and install it through SxFS.
set -euo pipefail
. "$(dirname "${BASH_SOURCE[0]}")/env.sh"

BACKEND_NAME="mediaplayer-ffmpeg"
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
LINKED="${SAVANXP_MEDIA_LINKED:-$OUTPUT_ROOT/external/mediaplayer.elf}"
if [[ ! -f "$LINKED" && -f "$REPO/build/external/mediaplayer.elf" ]]; then
    LINKED="$REPO/build/external/mediaplayer.elf"
fi
if [[ ! -f "$LINKED" ]]; then
    echo "ffmpeg: falta $LINKED; ejecuta ports/ffmpeg/build.sh" >&2
    exit 1
fi

PYTHON="${SAVANXP_PYTHON:-python3}"
command -v "$PYTHON" >/dev/null 2>&1 || {
    echo "ffmpeg: Python no encontrado: $PYTHON" >&2
    exit 1
}
command -v "${SAVANXP_OBJCOPY:-llvm-objcopy}" >/dev/null 2>&1 || {
    echo "ffmpeg: llvm-objcopy no encontrado" >&2
    exit 1
}
command -v "${SAVANXP_READELF:-llvm-readelf}" >/dev/null 2>&1 || {
    echo "ffmpeg: llvm-readelf no encontrado" >&2
    exit 1
}

STAGE="$OUTPUT_ROOT/ports/ffmpeg/install-stage"
RESOURCE_DIR="$OUTPUT_ROOT/ffmpeg/sxe"
STAMPED="$OUTPUT_ROOT/ffmpeg/$BACKEND_NAME"
rm -rf "$STAGE" "$RESOURCE_DIR"
mkdir -p "$STAGE/bin" "$RESOURCE_DIR" "$OUTPUT_ROOT/ffmpeg"
cp "$LINKED" "$STAMPED"

BUILD_ID=$(git -C "$REPO" rev-parse --short HEAD 2>/dev/null || true)
[[ -n "$BUILD_ID" ]] || BUILD_ID=unknown
"$PYTHON" "$REPO/tools/gen_sxe_resources.py" \
    --project-root "$REPO" \
    --manifest-dir "$PORT/overlay/mediaplayer" \
    --output-dir "$RESOURCE_DIR" \
    --build-id "$BUILD_ID" \
    --program "$BACKEND_NAME"
"$PYTHON" "$REPO/tools/stamp_sxe.py" \
    --binary "$STAMPED" \
    --name "$BACKEND_NAME" \
    --resource-dir "$RESOURCE_DIR" \
    --objcopy "${SAVANXP_OBJCOPY:-llvm-objcopy}" \
    --readelf "${SAVANXP_READELF:-llvm-readelf}"
cp "$STAMPED" "$STAGE/bin/$BACKEND_NAME"

if ((WITH_TEST_MEDIA)); then
    mkdir -p "$OUTPUT_ROOT/media" "$STAGE/media"
    for generator in make-tone.py make-clip.py make-avclip.py; do
        "$PYTHON" "$PORT/tests/$generator"
    done
    for media in tono.wav clip.mjpeg avsync.avi; do
        cp "$OUTPUT_ROOT/media/$media" "$STAGE/media/$media"
    done
fi

printf 'ffmpeg: stamped %s: %s bytes\n' "$BACKEND_NAME" "$(wc -c < "$STAMPED" | tr -d ' ')"
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
    --sectors "${SAVANXP_SXFS_SECTORS:-131072}"
)
if ((NO_COMPACT)); then
    SYNC_ARGS+=(--no-compact)
fi
"$PYTHON" "$REPO/tools/sxfs_sync.py" "${SYNC_ARGS[@]}"
echo "ffmpeg: instalado /disk/bin/$BACKEND_NAME"
if ((WITH_TEST_MEDIA)); then
    echo "ffmpeg: instalados /disk/media/{tono.wav,clip.mjpeg,avsync.avi}"
fi
