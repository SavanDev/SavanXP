#!/usr/bin/env bash
# Build and install the official SavanXP ccleste port.
set -euo pipefail
. "$(dirname "${BASH_SOURCE[0]}")/env.sh"

NO_INSTALL=0
SKIP_PORT=0
NO_COMPACT=0
WITH_ASSETS=1
WORK_OVERRIDE=""

usage() {
    cat <<'EOF'
Usage: ./ports/ccleste/build.sh [options]

Options:
  --no-install       Build and stamp without touching build/disk.img.
  --skip-port        Reuse build/external/ccleste.elf and only install it.
  --no-assets        Install only the executable, not /disk/games/celeste.
  --no-compact       Disable automatic SxFS compaction while installing.
  --work PATH        Override the port work directory.
  -h, --help         Show this help.
EOF
}

while (($#)); do
    case "$1" in
        --no-install) NO_INSTALL=1; shift ;;
        --skip-port) SKIP_PORT=1; shift ;;
        --no-assets) WITH_ASSETS=0; shift ;;
        --no-compact) NO_COMPACT=1; shift ;;
        --work)
            (($# >= 2)) || { echo "ccleste: --work requires a path" >&2; exit 2; }
            WORK_OVERRIDE=$2
            shift 2
            ;;
        -h|--help) usage; exit 0 ;;
        *) echo "ccleste: unknown option '$1'" >&2; usage >&2; exit 2 ;;
    esac
done

if [[ -n "$WORK_OVERRIDE" ]]; then
    case "$WORK_OVERRIDE" in
        /*) WORK=$WORK_OVERRIDE ;;
        *) WORK="$REPO/$WORK_OVERRIDE" ;;
    esac
    SRC="$WORK/ccleste-${CCLESTE_COMMIT}"
    OBJ="$WORK/obj"
    STAGE="$WORK/install-stage"
    RESOURCE_DIR="$WORK/sxe"
    FRESH_ELF="$WORK/ccleste.elf"
    DATA_DIR="$SRC/data"
fi

for tool in "$CLANG" "$OBJCOPY" "$READELF" "$PYTHON"; do
    command -v "$tool" >/dev/null 2>&1 || {
        echo "ccleste: required tool not found: $tool" >&2
        exit 1
    }
done

if ((SKIP_PORT == 0)); then
    bash "$PORT/fetch.sh"
fi

if ((SKIP_PORT == 0)); then
    # The decoder lives in another port: fail here with the fix, not later
    # with a missing header.
    [[ -f "$FFMPEG_LIB" ]] || {
        echo "ccleste: falta $FFMPEG_LIB; construi ports/ffmpeg primero (./ports/ffmpeg/build.sh)" >&2
        exit 1
    }
    [[ -f "$FFMPEG_BUILD/config.h" ]] || {
        echo "ccleste: falta $FFMPEG_BUILD/config.h; construi ports/ffmpeg primero" >&2
        exit 1
    }
    shopt -s nullglob
    FFMPEG_HEADERS=("$OUTPUT_ROOT"/ports/ffmpeg/work/ffmpeg-*/libavcodec/avcodec.h)
    shopt -u nullglob
    (( ${#FFMPEG_HEADERS[@]} == 1 )) || {
        echo "ccleste: arbol FFmpeg ambiguo o ausente bajo $OUTPUT_ROOT/ports/ffmpeg/work" >&2
        exit 1
    }
    FFMPEG_SRC=$(dirname "$(dirname "${FFMPEG_HEADERS[0]}")")
fi

if ((WITH_ASSETS)); then
    for asset in gfx.bmp font.bmp; do
        [[ -f "$DATA_DIR/$asset" ]] || {
            echo "ccleste: missing game asset $DATA_DIR/$asset" >&2
            echo "ccleste: run ports/ccleste/fetch.sh, or pass --no-assets" >&2
            exit 1
        }
    done
fi

compile_c() {
    local source=$1
    local object=$2
    shift 2
    $SX_CC -c -x c "$source" -o "$object" "${SX_TARGET_CFLAGS[@]}" "$@"
}

compile_asm() {
    local source=$1
    local object=$2
    "$CLANG" -c -x assembler-with-cpp "$source" -o "$object" "${SX_TARGET_CFLAGS[@]}"
}

if ((SKIP_PORT == 0)); then
    # The engine is compiled from the pristine upstream tree; the SavanXP
    # frontend is compiled alongside it and never overwrites an upstream file.
    # Only celeste.c is needed: sdl12main.c is the SDL host frontend this port
    # replaces, and tilemap.h is included by the overlay, not compiled alone.
    BUILD_SRC="$WORK/src"
    rm -rf "$BUILD_SRC" "$OBJ"
    mkdir -p "$BUILD_SRC" "$OBJ" "$RESOURCE_DIR" "$OUTPUT_ROOT/external" "$OUTPUT_ROOT/ccleste"
    cp "$SRC/celeste.c" "$SRC/celeste.h" "$SRC/tilemap.h" "$BUILD_SRC/"
    cp "$PORT/overlay/ccleste_savanxp.c" \
       "$PORT/overlay/ccleste_savanxp_assets.c" \
       "$PORT/overlay/ccleste_savanxp_assets.h" \
       "$BUILD_SRC/"

    INCLUDE_FLAGS=(-I "$SDK/include" -I "$REPO/include" -I "$BUILD_SRC")
    # ldso.c incluye "libc.h" del userland, y el overlay los headers publicos
    # de FFmpeg mas el config.h generado (igual que mediaplayer).
    INCLUDE_FLAGS+=(-I "$REPO/subsystems/posix/userland")
    if [[ -n "$FFMPEG_SRC" ]]; then
        INCLUDE_FLAGS+=(-I "$FFMPEG_SRC" -I "$FFMPEG_BUILD")
    fi

    OBJECTS=()
    echo "== compilando el motor (upstream, sin parches)"
    for relative in "${ENGINE_SOURCES[@]}"; do
        name="engine-$(basename "$relative" .c)"
        echo "   $relative"
        # The engine is pristine upstream and this port carries no patch, so its
        # K&R-style empty parameter lists and its stray top-level semicolon are
        # left alone rather than edited: the two -Wno flags keep the build
        # readable without touching a file this port does not own.
        compile_c "$BUILD_SRC/$relative" "$OBJ/$name.o" \
            -Wall -Wextra -Wpedantic -Wno-strict-prototypes -Wno-extra-semi \
            -Wno-unused-parameter \
            "${INCLUDE_FLAGS[@]}"
        OBJECTS+=("$OBJ/$name.o")
    done

    echo "== compilando el frontend de SavanXP (overlay)"
    for name in ccleste_savanxp ccleste_savanxp_assets; do
        echo "   $name.c"
        compile_c "$BUILD_SRC/$name.c" "$OBJ/$name.o" \
            -Wall -Wextra -Wpedantic \
            "${INCLUDE_FLAGS[@]}" \
            "-DCCLESTE_DATA_DIR=\"$DATA_TARGET\"" \
            -fmacro-prefix-map="$BUILD_SRC=ports/ccleste/overlay"
        OBJECTS+=("$OBJ/$name.o")
    done

    echo "== compilando el runtime del SDK"
    compile_asm "$SDK/runtime/crt0.S" "$OBJ/crt0.o"
    compile_asm "$SDK/runtime/setjmp.S" "$OBJ/setjmp.o"
    for unit in libc posix gfx gfx2d audio math; do
        compile_c "$SDK/runtime/$unit.c" "$OBJ/$unit.o" \
            -fno-builtin "${INCLUDE_FLAGS[@]}"
    done

    echo "== compilando el cargador (ldso va dentro del programa, como en mediaplayer)"
    compile_c "$REPO/subsystems/posix/userland/ldso.c" "$OBJ/ldso.o" \
        -Wall -Wextra -Wpedantic "${INCLUDE_FLAGS[@]}"

    echo "== linkeando ccleste"
    LINK_ARGS=(
        "$OBJ/crt0.o" "$OBJ/libc.o" "$OBJ/posix.o" "$OBJ/gfx.o" "$OBJ/gfx2d.o"
        "$OBJ/math.o" "$OBJ/setjmp.o" "$OBJ/audio.o" "$OBJ/ldso.o"
        "${OBJECTS[@]}"
    )
    # DT_NEEDED por soname: el cargador mapea libffmpeg.so.0.4 de /disk/lib y
    # de ahi cuelga libmath. Sin el port no se compila (ver el chequeo arriba).
    if [[ -n "$FFMPEG_SRC" ]]; then
        LINK_ARGS+=("$FFMPEG_LIB")
    fi
    $SX_LD "${SX_TARGET_LDFLAGS[@]}" -o "$FRESH_ELF" "${LINK_ARGS[@]}"
    $SIZE_CMD "$FRESH_ELF" | tail -1

    BUILD_ID=$(git -C "$REPO" rev-parse --short HEAD 2>/dev/null || true)
    [[ -n "$BUILD_ID" ]] || BUILD_ID=unknown
    "$PYTHON" "$REPO/tools/gen_sxe_resources.py" \
        --project-root "$REPO" \
        --output-dir "$RESOURCE_DIR" \
        --manifest-dir "$PORT/overlay" \
        --build-id "$BUILD_ID" \
        --program ccleste
    "$PYTHON" "$REPO/tools/stamp_sxe.py" \
        --binary "$FRESH_ELF" \
        --name ccleste \
        --resource-dir "$RESOURCE_DIR" \
        --objcopy "$OBJCOPY" \
        --readelf "$READELF"

    publish_tmp="$OUTPUT_ROOT/external/.ccleste.elf.tmp.$$"
    cp "$FRESH_ELF" "$publish_tmp"
    chmod +x "$publish_tmp"
    mv -f "$publish_tmp" "$OUTPUT"
    echo "ccleste: ELF generated: $OUTPUT"
else
    if [[ ! -f "$OUTPUT" ]]; then
        echo "ccleste: missing $OUTPUT; run ports/ccleste/build.sh first" >&2
        exit 1
    fi
fi

if ((NO_INSTALL)); then
    exit 0
fi

if [[ ! -f "$OUTPUT" ]]; then
    echo "ccleste: missing $OUTPUT" >&2
    exit 1
fi
# Always refresh the stamped copy from the published ELF. Guarding this on the
# file being absent made every rebuild after the first one a silent no-op: the
# installer staged the original binary forever, so fixes to the overlay never
# reached the image while the build reported success. --skip-port still means
# "reuse build/external/ccleste.elf", which is exactly what $OUTPUT holds then.
cp "$OUTPUT" "$STAMPED"

IMAGE="${SAVANXP_DISK_IMAGE:-$OUTPUT_ROOT/disk.img}"
SXFS_CLI="${SAVANXP_SXFS_CLI:-$OUTPUT_ROOT/tools/sxfs-cli}"
case "$IMAGE" in
    /*) ;;
    *) IMAGE="$REPO/$IMAGE" ;;
esac
case "$SXFS_CLI" in
    /*) ;;
    *) SXFS_CLI="$REPO/$SXFS_CLI" ;;
esac
if [[ ! -f "$IMAGE" ]]; then
    echo "ccleste: persistent image is missing; run ./build.sh build first" >&2
    exit 1
fi
if [[ ! -x "$SXFS_CLI" ]]; then
    echo "ccleste: sxfs-cli is missing; run ./build.sh build first" >&2
    exit 1
fi

rm -rf "$STAGE"
mkdir -p "$STAGE/bin"
cp "$STAMPED" "$STAGE/bin/ccleste"
echo "ccleste: staged /disk/bin/ccleste"

if ((WITH_ASSETS)); then
    mkdir -p "$STAGE/games/celeste"
    cp "$DATA_DIR/gfx.bmp" "$DATA_DIR/font.bmp" "$STAGE/games/celeste/"
    installed=2
    # La musica viaja comprimida (~1 MB los cinco OGG): se decodifica en el
    # juego a pedido con libffmpeg, asi que en disco no cuesta los 4 MB del
    # PCM. Ver ports/ccleste/README.md.
    shopt -s nullglob
    for track in "$DATA_DIR"/mus*.ogg; do
        cp "$track" "$STAGE/games/celeste/$(basename -- "$track")"
        installed=$((installed + 1))
    done
    for sound in "$DATA_DIR"/snd*.wav; do
        cp "$sound" "$STAGE/games/celeste/$(basename -- "$sound")"
        installed=$((installed + 1))
    done
    shopt -u nullglob
    echo "ccleste: staged /disk/games/celeste ($installed files)"
fi

SYNC_ARGS=(
    --image "$IMAGE"
    --source "$STAGE"
    --cli "$SXFS_CLI"
    --sectors "${SAVANXP_SXFS_SECTORS:-2097152}"
)
if ((NO_COMPACT)); then
    SYNC_ARGS+=(--no-compact)
fi
"$PYTHON" "$REPO/tools/sxfs_sync.py" "${SYNC_ARGS[@]}"

echo "ccleste: installed /disk/bin/ccleste"
if ((WITH_ASSETS)); then
    echo "ccleste: installed $DATA_TARGET"
fi
