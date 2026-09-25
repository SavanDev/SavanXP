#!/usr/bin/env bash
# Build and install the official SavanXP DoomGeneric port.
set -euo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
ROOT=$(cd -- "$SCRIPT_DIR/../.." && pwd)
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
SECTORS="${SAVANXP_SXFS_SECTORS:-131072}"
CC="${SAVANXP_CLANG:-clang}"
LD="${SAVANXP_LD:-ld.lld}"
OBJCOPY="${SAVANXP_OBJCOPY:-llvm-objcopy}"
READELF="${SAVANXP_READELF:-llvm-readelf}"
PYTHON="${SAVANXP_PYTHON:-python3}"

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

SDK="$ROOT/subsystems/posix/sdk/v1"
COMMON_FLAGS=(
    -target x86_64-unknown-none-elf
    -ffreestanding
    -fstack-protector-strong
    -fno-pic
    -fno-pie
    -mno-red-zone
    -mcmodel=small
    -mno-mmx
    -mno-sse
    -mno-sse2
    -mgeneral-regs-only
    -Wall
    -Wextra
    -Wpedantic
    -Wno-language-extension-token
    -Wno-c23-extensions
    -fmacro-prefix-map="$SRC=ports/doomgeneric/source/doomgeneric"
)
INCLUDE_FLAGS=(
    -I "$SDK/include"
    -I "$ROOT/include"
    -I "$SRC"
)

compile_c() {
    local source=$1
    local object=$2
    shift 2
    "$CC" -c -x c "$source" -o "$object" "${COMMON_FLAGS[@]}" "${INCLUDE_FLAGS[@]}" "$@"
}

compile_asm() {
    local source=$1
    local object=$2
    "$CC" -c "$source" -o "$object" "${COMMON_FLAGS[@]}" "${INCLUDE_FLAGS[@]}"
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

crt0="$OBJ/crt0.o"
libc="$OBJ/libc.o"
posix="$OBJ/posix.o"
gfx="$OBJ/gfx.o"
gfx2d="$OBJ/gfx2d.o"
setjmp_object="$OBJ/setjmp.o"
audio="$OBJ/audio.o"
compile_asm "$SDK/runtime/crt0.S" "$crt0"
compile_c "$SDK/runtime/libc.c" "$libc"
compile_c "$SDK/runtime/posix.c" "$posix"
compile_c "$SDK/runtime/gfx.c" "$gfx"
compile_c "$SDK/runtime/gfx2d.c" "$gfx2d"
compile_asm "$SDK/runtime/setjmp.S" "$setjmp_object"
compile_c "$SDK/runtime/audio.c" "$audio"

"$LD" -nostdlib -static -T "$SDK/linker.ld" \
    -o "$FRESH_ELF" \
    "$crt0" "$libc" "$posix" "$gfx" "$gfx2d" "$setjmp_object" "$audio" \
    "${APP_OBJECTS[@]}"

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

if [[ -f "$WAD_PATH" ]]; then
    cp "$WAD_PATH" "$STAGE/games/doom/$(basename -- "$WAD_PATH")"
    echo "doomgeneric: WAD selected: $WAD_PATH"
else
    echo "doomgeneric: WAD not found: $WAD_PATH" >&2
    echo "doomgeneric: Freedoom download: https://freedoom.github.io/download.html" >&2
fi

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

echo "doomgeneric: installed /disk/bin/doomgeneric"
if [[ -f "$WAD_PATH" ]]; then
    echo "doomgeneric: installed /disk/games/doom/$(basename -- "$WAD_PATH")"
fi
