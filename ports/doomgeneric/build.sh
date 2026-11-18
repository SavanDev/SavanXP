#!/usr/bin/env bash
# Build and install the official SavanXP DoomGeneric port.
set -euo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
ROOT=$(cd -- "$SCRIPT_DIR/../.." && pwd)
# shellcheck source=../../tools/lib/sdk_flags.sh
. "$ROOT/tools/lib/sdk_flags.sh"
PORT_DIR="$SCRIPT_DIR"
OUTPUT_ROOT="${SAVANXP_OUTPUT_ROOT:-$ROOT/build}"
BUILD_ROOT="${SAVANXP_DOOM_BUILD_ROOT:-$OUTPUT_ROOT/ports/doomgeneric}"
EXTERNAL_DIR="${SAVANXP_EXTERNAL_ROOT:-$OUTPUT_ROOT/external}"
OUTPUT="$EXTERNAL_DIR/doomgeneric.elf"
IMAGE="${SAVANXP_DISK_IMAGE:-$OUTPUT_ROOT/disk.img}"
SXFS_CLI="${SAVANXP_SXFS_CLI:-$OUTPUT_ROOT/tools/sxfs-cli}"
WAD_PATH="$PORT_DIR/wad/freedoom1.wad"
NO_INSTALL=0
NO_COMPACT=0
SECTORS="${SAVANXP_SXFS_SECTORS:-2097152}"
CC="${SAVANXP_CLANG:-clang}"
LINK_CC="${SAVANXP_LINK_CC:-$CC}"
LD="${SAVANXP_LD:-ld.lld}"
OBJCOPY="${SAVANXP_OBJCOPY:-llvm-objcopy}"
READELF="${SAVANXP_READELF:-llvm-readelf}"
PYTHON="${SAVANXP_PYTHON:-python3}"
# La libreria del sistema se busca donde el build base la instala (el mismo
# directorio que check_shared_libs lee). El port no la construye: la trae el
# arbol, y sin ella no hay sintetizador que enlazar.

usage() {
    cat <<'EOF'
Usage: ./ports/doomgeneric/build.sh [options]

Options:
  --wad PATH       Install the selected IWAD (default: ports/doomgeneric/wad/freedoom1.wad).
  --no-install     Build and stamp the ELF without touching build/disk.img.
  --no-compact     Disable automatic SxFS compaction during installation.
  -h, --help       Show this help.
EOF
}

while (($#)); do
    case "$1" in
        --wad)
            (($# >= 2)) || { echo "doomgeneric: --wad requires a path" >&2; exit 2; }
            WAD_PATH=$2
            shift 2
            ;;
        --no-install)
            NO_INSTALL=1
            shift
            ;;
        --no-compact)
            NO_COMPACT=1
            shift
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            echo "doomgeneric: unknown option '$1'" >&2
            usage >&2
            exit 2
            ;;
    esac
done

for tool in "$CC" "$LD" "$OBJCOPY" "$READELF" "$PYTHON" git; do
    command -v "$tool" >/dev/null 2>&1 || {
        echo "doomgeneric: required tool not found: $tool" >&2
        exit 1
    }
done
"$PYTHON" -c 'import PIL' >/dev/null 2>&1 || {
    echo "doomgeneric: Python Pillow is required to generate SXE resources" >&2
    exit 1
}
# The port builds against the SDK sysroot (docs/roadmaps/done/SDK_V2_ROADMAP.md,
# phase 4): its PIE runtime, the loader and libsxmidi.so.0.4, which the music
# links against. make-sdk.sh brings it up to date and fails, naming
# ./build.sh build, if the system's libraries are not built yet.
SAVANXP_OUTPUT_ROOT="$OUTPUT_ROOT" "$ROOT/tools/make-sdk.sh" --quiet
SYSROOT=$(SAVANXP_OUTPUT_ROOT="$OUTPUT_ROOT" "$ROOT/tools/make-sdk.sh" --print-path)
SX_MIDI_LIB="$SYSROOT/lib/shared/libsxmidi.so.0.4"

SOURCE_DIR="$PORT_DIR/source/doomgeneric"
if [[ ! -d "$SOURCE_DIR" ]]; then
    echo "doomgeneric: pinned upstream source is missing: $SOURCE_DIR" >&2
    exit 1
fi
SOURCE_TREE_SHA256=$(sed -n 's/^source_tree_sha256=//p' "$PORT_DIR/UPSTREAM" | head -n 1)
if [[ -z "$SOURCE_TREE_SHA256" ]]; then
    echo "doomgeneric: UPSTREAM is missing source_tree_sha256" >&2
    exit 1
fi
"$PYTHON" "$ROOT/tools/source_tree_digest.py" \
    --root "$SOURCE_DIR" --expected "$SOURCE_TREE_SHA256"

WORK="$BUILD_ROOT/work"
SRC="$WORK/src"
OBJ="$BUILD_ROOT/obj"
RESOURCE_DIR="$WORK/sxe"
FRESH_ELF="$BUILD_ROOT/doomgeneric.elf"
rm -rf "$WORK" "$OBJ"
mkdir -p "$WORK" "$SRC" "$OBJ" "$EXTERNAL_DIR"
cp -a "$SOURCE_DIR/." "$SRC/"
# The work tree lives below the SavanXP checkout, so give git apply a local
# repository root instead of letting it discover the parent repository.
git -C "$SRC" init -q

# Apply the reviewed engine deltas to the pristine source before adding the
# SavanXP platform overlay. A dirty or mismatched source tree fails here.
for patch in "$PORT_DIR"/patches/*.patch; do
    echo "+ apply $(basename "$patch")"
    git -C "$SRC" apply --check --whitespace=nowarn "$patch"
    git -C "$SRC" apply --whitespace=nowarn "$patch"
done
cp -a "$PORT_DIR/overlay/." "$SRC/"

# The SDK's PIE profile, as ccleste and mediaplayer use: to map a library the
# executable has to be ET_DYN with .dynsym, which takes PIC, -pie and
# --export-dynamic. The flags come from the SDK (tools/lib/sdk_flags.sh), so
# the port and the prebuilt runtime it links always agree.
mapfile -t COMMON_FLAGS < <(savanxp_sdk_pie_compile_flags)
COMMON_FLAGS+=(-fmacro-prefix-map="$SRC=ports/doomgeneric/source/doomgeneric")
mapfile -t LINK_FLAGS < <(savanxp_sdk_pie_link_flags 0)
INCLUDE_FLAGS=(
    -I "$SYSROOT/include"
    -I "$SRC"
)

compile_c() {
    local source=$1
    local object=$2
    shift 2
    "$CC" -c -x c "$source" -o "$object" "${COMMON_FLAGS[@]}" "${INCLUDE_FLAGS[@]}" "$@"
}

# The explicit source list is intentional. The pristine upstream tree also
# contains several alternate host backends with their own entry points.
APP_SOURCES=()
while IFS= read -r relative; do
    [[ -z "$relative" || "$relative" == \#* ]] && continue
    if [[ "$relative" == */* || ! -f "$SRC/$relative" ]]; then
        echo "doomgeneric: invalid source entry '$relative'" >&2
        exit 1
    fi
    APP_SOURCES+=("$SRC/$relative")
done < "$PORT_DIR/sources.txt"
for relative in doomgeneric_savanxp.c doomgeneric_savanxp_audio.c net_stubs.c savanxp_compat.c; do
    APP_SOURCES+=("$SRC/$relative")
done

APP_OBJECTS=()
index=0
for source in "${APP_SOURCES[@]}"; do
    object="$OBJ/app-$index.o"
    compile_c "$source" "$object"
    APP_OBJECTS+=("$object")
    index=$((index + 1))
done

# The runtime, prebuilt in the sysroot's PIE variant. The window runtime reads
# the program's own .sxe to publish its title and icon
# (docs/desktop/DESKTOP_ARCHITECTURE.md), so the SXE reader comes along in
# core. midi is left out: libsxmidi.so.0.4 replaces it. The loader goes inside
# the program: crt0 runs it through the weak sx_run_interpreter hook, and
# <dlfcn.h> is POSIX's layer over it.
mapfile -t RUNTIME_OBJECTS < <(savanxp_sdk_runtime_objects "$SYSROOT" pie core math audio setjmp loader dlfcn)

"$LINK_CC" "${LINK_FLAGS[@]}" \
    -o "$FRESH_ELF" \
    "${RUNTIME_OBJECTS[@]}" \
    "${APP_OBJECTS[@]}" \
    "$SX_MIDI_LIB"

BUILD_ID=$(git -C "$ROOT" rev-parse --short HEAD 2>/dev/null || true)
[[ -n "$BUILD_ID" ]] || BUILD_ID=unknown
mkdir -p "$RESOURCE_DIR"
"$PYTHON" "$ROOT/tools/gen_sxe_resources.py" \
    --project-root "$ROOT" \
    --output-dir "$RESOURCE_DIR" \
    --manifest-dir "$SRC" \
    --build-id "$BUILD_ID" \
    --program doomgeneric
"$PYTHON" "$ROOT/tools/stamp_sxe.py" \
    --binary "$FRESH_ELF" \
    --name doomgeneric \
    --resource-dir "$RESOURCE_DIR" \
    --objcopy "$OBJCOPY" \
    --readelf "$READELF"

publish_tmp="$EXTERNAL_DIR/.doomgeneric.elf.tmp.$$"
cp "$FRESH_ELF" "$publish_tmp"
chmod +x "$publish_tmp"
mv -f "$publish_tmp" "$OUTPUT"
echo "doomgeneric: ELF generated: $OUTPUT"

if ((NO_INSTALL)); then
    exit 0
fi

if [[ ! -f "$IMAGE" ]]; then
    echo "doomgeneric: persistent image is missing; run ./build.sh build first" >&2
    exit 1
fi
if [[ ! -x "$SXFS_CLI" ]]; then
    echo "doomgeneric: sxfs-cli is missing; run ./build.sh build first" >&2
    exit 1
fi

STAGE="$BUILD_ROOT/install-stage"
rm -rf "$STAGE"
mkdir -p "$STAGE/bin" "$STAGE/games/doom"
cp "$OUTPUT" "$STAGE/bin/doomgeneric"

LICENSE_NOTICES=("$ROOT/ports/doomgeneric/licenses/doomgeneric.notice")
if [[ -f "$WAD_PATH" ]]; then
    cp "$WAD_PATH" "$STAGE/games/doom/$(basename -- "$WAD_PATH")"
    echo "doomgeneric: WAD selected: $WAD_PATH"
    if [[ "$(basename -- "$WAD_PATH")" == freedoom* ]]; then
        LICENSE_NOTICES+=("$ROOT/ports/doomgeneric/licenses/freedoom.notice")
    fi
else
    echo "doomgeneric: WAD not found: $WAD_PATH" >&2
    echo "doomgeneric: Freedoom download: https://freedoom.github.io/download.html" >&2
fi

"$PYTHON" "$ROOT/tools/stage_licenses.py" stage --root "$ROOT" \
    --out "$STAGE/system/licenses" "${LICENSE_NOTICES[@]}"

SYNC_ARGS=(
    --image "$IMAGE"
    --source "$STAGE"
    --cli "$SXFS_CLI"
    --sectors "$SECTORS"
)
if ((NO_COMPACT)); then
    SYNC_ARGS+=(--no-compact)
fi
"$PYTHON" "$ROOT/tools/sxfs_sync.py" "${SYNC_ARGS[@]}"

echo "doomgeneric: installed /bin/doomgeneric"
if [[ -f "$WAD_PATH" ]]; then
    echo "doomgeneric: installed /games/doom/$(basename -- "$WAD_PATH")"
fi
